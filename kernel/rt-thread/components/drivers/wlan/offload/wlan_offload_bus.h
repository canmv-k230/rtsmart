/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __RT_WLAN_OFFLOAD_BUS_H__
#define __RT_WLAN_OFFLOAD_BUS_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

enum rt_wlan_offload_bus_type
{
    RT_WLAN_OFFLOAD_BUS_SPI = 0,
    RT_WLAN_OFFLOAD_BUS_SDIO,
    RT_WLAN_OFFLOAD_BUS_USB,
    RT_WLAN_OFFLOAD_BUS_CUSTOM,
};

enum rt_wlan_offload_bus_state
{
    RT_WLAN_OFFLOAD_BUS_UNINITIALIZED = 0,
    RT_WLAN_OFFLOAD_BUS_STOPPED,
    RT_WLAN_OFFLOAD_BUS_STARTING,
    RT_WLAN_OFFLOAD_BUS_STARTED,
    RT_WLAN_OFFLOAD_BUS_SUSPENDED,
    RT_WLAN_OFFLOAD_BUS_STOPPING,
    RT_WLAN_OFFLOAD_BUS_FAILED,
};

enum rt_wlan_offload_bus_event
{
    RT_WLAN_OFFLOAD_BUS_EVENT_AVAILABLE = 0,
    RT_WLAN_OFFLOAD_BUS_EVENT_UNAVAILABLE,
    RT_WLAN_OFFLOAD_BUS_EVENT_ERROR,
    RT_WLAN_OFFLOAD_BUS_EVENT_WAKE,
};

enum rt_wlan_offload_bus_priority
{
    RT_WLAN_OFFLOAD_BUS_PRIORITY_LOW = 0,
    RT_WLAN_OFFLOAD_BUS_PRIORITY_NORMAL,
    RT_WLAN_OFFLOAD_BUS_PRIORITY_HIGH,
    RT_WLAN_OFFLOAD_BUS_PRIORITY_CONTROL,
};

#define RT_WLAN_OFFLOAD_BUS_CAP_PACKET       (1U << 0)
#define RT_WLAN_OFFLOAD_BUS_CAP_FULL_DUPLEX  (1U << 1)
#define RT_WLAN_OFFLOAD_BUS_CAP_HOTPLUG      (1U << 2)
#define RT_WLAN_OFFLOAD_BUS_CAP_SUSPEND      (1U << 3)
#define RT_WLAN_OFFLOAD_BUS_CAP_DMA          (1U << 4)
#define RT_WLAN_OFFLOAD_BUS_CAP_TX_PRIORITY  (1U << 5)

struct rt_wlan_offload_bus;

/* Return -RT_EEMPTY for padding/no frame and another error for malformed RX. */
typedef rt_err_t (*rt_wlan_offload_bus_rx_handler_t)(struct rt_wlan_offload_bus *bus,
                                                const void *data,
                                                rt_size_t length,
                                                void *parameter);
typedef void (*rt_wlan_offload_bus_event_handler_t)(struct rt_wlan_offload_bus *bus,
                                                enum rt_wlan_offload_bus_event event,
                                                rt_err_t status,
                                                void *parameter);

struct rt_wlan_offload_bus_ops
{
    /* transmit() is synchronous with respect to ownership of data. */
    rt_err_t (*start)(struct rt_wlan_offload_bus *bus);
    rt_err_t (*stop)(struct rt_wlan_offload_bus *bus);
    rt_err_t (*transmit)(struct rt_wlan_offload_bus *bus,
                         const void *data, rt_size_t length);
    rt_err_t (*transmit_priority)(struct rt_wlan_offload_bus *bus,
                                 enum rt_wlan_offload_bus_priority priority,
                                 const void *data, rt_size_t length);
    rt_err_t (*reset)(struct rt_wlan_offload_bus *bus);
    rt_err_t (*suspend)(struct rt_wlan_offload_bus *bus);
    rt_err_t (*resume)(struct rt_wlan_offload_bus *bus);
    rt_err_t (*control)(struct rt_wlan_offload_bus *bus, int command, void *argument);
};

struct rt_wlan_offload_bus_config
{
    enum rt_wlan_offload_bus_type type;
    const struct rt_wlan_offload_bus_ops *ops;
    rt_uint32_t capabilities;
    rt_size_t max_tx_size;
    rt_size_t max_rx_size;
    rt_uint16_t alignment;
    rt_uint16_t headroom;
    rt_uint16_t tailroom;
    void *driver_data;
};

struct rt_wlan_offload_bus
{
    enum rt_wlan_offload_bus_type type;
    enum rt_wlan_offload_bus_state state;
    const struct rt_wlan_offload_bus_ops *ops;
    rt_uint32_t capabilities;
    rt_size_t max_tx_size;
    rt_size_t max_rx_size;
    rt_uint16_t alignment;
    rt_uint16_t headroom;
    rt_uint16_t tailroom;
    void *driver_data;
    struct rt_mutex state_lock;
    struct rt_mutex tx_lock;
    rt_wlan_offload_bus_rx_handler_t rx_handler;
    rt_wlan_offload_bus_event_handler_t event_handler;
    void *callback_parameter;
};

rt_err_t rt_wlan_offload_bus_init(struct rt_wlan_offload_bus *bus,
                             const struct rt_wlan_offload_bus_config *config);
rt_err_t rt_wlan_offload_bus_deinit(struct rt_wlan_offload_bus *bus);
rt_err_t rt_wlan_offload_bus_start(struct rt_wlan_offload_bus *bus);
rt_err_t rt_wlan_offload_bus_stop(struct rt_wlan_offload_bus *bus);
rt_err_t rt_wlan_offload_bus_transmit(struct rt_wlan_offload_bus *bus,
                                 const void *data, rt_size_t length);
rt_err_t rt_wlan_offload_bus_transmit_priority(
    struct rt_wlan_offload_bus *bus, enum rt_wlan_offload_bus_priority priority,
    const void *data, rt_size_t length);
rt_err_t rt_wlan_offload_bus_reset(struct rt_wlan_offload_bus *bus);
rt_err_t rt_wlan_offload_bus_suspend(struct rt_wlan_offload_bus *bus);
rt_err_t rt_wlan_offload_bus_resume(struct rt_wlan_offload_bus *bus);
rt_err_t rt_wlan_offload_bus_control(struct rt_wlan_offload_bus *bus,
                                int command, void *argument);

void rt_wlan_offload_bus_set_callbacks(struct rt_wlan_offload_bus *bus,
                                  rt_wlan_offload_bus_rx_handler_t rx_handler,
                                  rt_wlan_offload_bus_event_handler_t event_handler,
                                  void *parameter);
/* RX and event notification must be made from thread/worker context. */
rt_err_t rt_wlan_offload_bus_rx(struct rt_wlan_offload_bus *bus,
                           const void *data, rt_size_t length);
void rt_wlan_offload_bus_notify(struct rt_wlan_offload_bus *bus,
                           enum rt_wlan_offload_bus_event event,
                           rt_err_t status);

void *rt_wlan_offload_bus_get_driver_data(struct rt_wlan_offload_bus *bus);

#ifdef __cplusplus
}
#endif

#endif /* __RT_WLAN_OFFLOAD_BUS_H__ */
