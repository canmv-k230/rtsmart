/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted.h"
#include "esp_hosted_hci.h"
#include "esp_hosted_transport.h"
#include "esp_hosted_mcu_log.h"

#ifdef ESP_HOSTED_BLE

#include <rtdevice.h>
#include <drivers/bt_hci.h>

#define DBG_TAG "esp_hosted.hci"
#define DBG_LVL ESP_HOSTED_MCU_DBG_LVL
#include <rtdbg.h>

static struct rt_bt_hci_device g_esp_hci;
static uint8_t g_esp_hci_rx_buffer[ESP_HOSTED_BT_HCI_RX_BUFFER_SIZE]
    __attribute__((aligned(RT_ALIGN_SIZE)));
static volatile rt_bool_t g_esp_hci_open;
static rt_bool_t g_esp_hci_registered;

static rt_err_t eh_hci_open(struct rt_bt_hci_device *hci)
{
    rt_err_t result;

    (void)hci;
    esp_hosted_transport_flush_hci();
    result = rt_esp_hosted_bt_controller_start();
    if (result == RT_EOK)
    {
        g_esp_hci_open = RT_TRUE;
    }
    return result;
}

static rt_err_t eh_hci_close(struct rt_bt_hci_device *hci)
{
    (void)hci;
    g_esp_hci_open = RT_FALSE;
    esp_hosted_transport_flush_hci();
    (void)rt_esp_hosted_bt_controller_stop();
    return RT_EOK;
}

static rt_err_t eh_hci_send(struct rt_bt_hci_device *hci,
                            rt_uint8_t packet_type, const rt_uint8_t *data,
                            rt_size_t length)
{
    (void)hci;
    if (!g_esp_hci_open || !rt_esp_hosted_is_ready())
    {
        return -RT_EBUSY;
    }
    if (!rt_esp_hosted_bt_supported())
    {
        return -RT_ENOSYS;
    }
    return esp_hosted_transport_send_hci(packet_type, data, length,
                                         RT_NULL, RT_NULL);
}

static const struct rt_bt_hci_ops g_esp_hci_ops = {
    .open = eh_hci_open,
    .close = eh_hci_close,
    .send = eh_hci_send,
};

rt_err_t esp_hosted_hci_init(void)
{
    rt_err_t result;

    if (g_esp_hci_registered)
    {
        return RT_EOK;
    }
    result = rt_bt_hci_register_auto(&g_esp_hci, &g_esp_hci_ops,
                                     g_esp_hci_rx_buffer,
                                     sizeof(g_esp_hci_rx_buffer), RT_NULL);
    if (result == RT_EOK)
    {
        g_esp_hci_registered = RT_TRUE;
        LOG_I("Bluetooth HCI registered as /dev/%s",
              g_esp_hci.parent.parent.name);
    }
    return result;
}

void esp_hosted_hci_deinit(void)
{
    if (!g_esp_hci_registered)
    {
        return;
    }
    g_esp_hci_open = RT_FALSE;
    esp_hosted_transport_flush_hci();
    rt_bt_hci_flush(&g_esp_hci);
    if (rt_device_unregister(&g_esp_hci.parent) == RT_EOK)
    {
        rt_mutex_detach(&g_esp_hci.tx_lock);
        rt_memset(&g_esp_hci, 0, sizeof(g_esp_hci));
        g_esp_hci_registered = RT_FALSE;
    }
}

void esp_hosted_hci_receive(const uint8_t *data, size_t length)
{
    rt_err_t result;

    if (!data || length < 2)
    {
        return;
    }
    result = rt_bt_hci_receive(&g_esp_hci, data[0], data + 1, length - 1);
    if (result != RT_EOK && result != -RT_EFULL)
    {
        LOG_W("invalid HCI packet: type=%u length=%u result=%d",
              data[0], (unsigned int)(length - 1), result);
    }
}

void esp_hosted_hci_reset(void)
{
    g_esp_hci_open = RT_FALSE;
    esp_hosted_transport_flush_hci();
    rt_bt_hci_flush(&g_esp_hci);
}

#endif /* ESP_HOSTED_BLE */
