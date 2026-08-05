/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __RT_BT_HCI_H__
#define __RT_BT_HCI_H__

#include <rtthread.h>
#include <rthw.h>
#include <ipc/ringbuffer.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_BT_HCI_H4_CMD 0x01
#define RT_BT_HCI_H4_ACL 0x02
#define RT_BT_HCI_H4_SCO 0x03
#define RT_BT_HCI_H4_EVT 0x04
#define RT_BT_HCI_H4_ISO 0x05

#define RT_BT_HCI_AUTO_NAME_ATTEMPTS 1024U

struct rt_bt_hci_device;

/* Packet data passed through this API excludes the leading H:4 type byte. */
struct rt_bt_hci_ops
{
    rt_err_t (*open)(struct rt_bt_hci_device *hci);
    rt_err_t (*close)(struct rt_bt_hci_device *hci);
    rt_err_t (*send)(struct rt_bt_hci_device *hci, rt_uint8_t packet_type,
                     const rt_uint8_t *data, rt_size_t length);
};

struct rt_bt_hci_stats
{
    rt_uint32_t tx_packets;
    rt_uint32_t tx_bytes;
    rt_uint32_t tx_errors;
    rt_uint32_t rx_packets;
    rt_uint32_t rx_bytes;
    rt_uint32_t rx_dropped;
    rt_uint32_t rx_invalid;
};

struct rt_bt_hci_device
{
    struct rt_device parent;
    const struct rt_bt_hci_ops *ops;
    struct rt_ringbuffer rx_ring;
#ifdef RT_USING_SMP
    struct rt_spinlock rx_lock;
#else
    rt_spinlock_t rx_lock;
#endif
    struct rt_mutex tx_lock;
    struct rt_bt_hci_stats stats;
};

/* Passing RT_NULL for name selects the next available /dev/hciX name. */
rt_err_t rt_bt_hci_register(struct rt_bt_hci_device *hci, const char *name,
                            const struct rt_bt_hci_ops *ops,
                            rt_uint8_t *rx_buffer, rt_uint16_t rx_buffer_size,
                            void *user_data);

/* Register the controller with the next available /dev/hciX name. At most
 * RT_BT_HCI_AUTO_NAME_ATTEMPTS candidates are checked before -RT_EFULL is
 * returned. */
rt_err_t rt_bt_hci_register_auto(struct rt_bt_hci_device *hci,
                                  const struct rt_bt_hci_ops *ops,
                                  rt_uint8_t *rx_buffer,
                                  rt_uint16_t rx_buffer_size,
                                  void *user_data);
rt_err_t rt_bt_hci_send(struct rt_bt_hci_device *hci, rt_uint8_t packet_type,
                        const rt_uint8_t *data, rt_size_t length);
rt_err_t rt_bt_hci_receive(struct rt_bt_hci_device *hci,
                           rt_uint8_t packet_type, const rt_uint8_t *data,
                           rt_size_t length);
void rt_bt_hci_flush(struct rt_bt_hci_device *hci);
rt_size_t rt_bt_hci_rx_length(struct rt_bt_hci_device *hci);
void rt_bt_hci_get_stats(struct rt_bt_hci_device *hci,
                         struct rt_bt_hci_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* __RT_BT_HCI_H__ */
