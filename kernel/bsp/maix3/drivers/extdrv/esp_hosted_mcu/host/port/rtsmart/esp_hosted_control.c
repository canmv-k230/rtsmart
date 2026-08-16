/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP-Hosted private capability and host-configuration protocol.
 */
#include "esp_hosted_control.h"
#include "esp_hosted_transport.h"
#include "esp_hosted_mcu_log.h"

#define DBG_TAG "esp_hosted.control"
#define DBG_LVL ESP_HOSTED_MCU_DBG_LVL
#include <rtdbg.h>

#define EH_PRIV_EVENT_INIT      0x22
#define EH_PRIV_TAG_CAPABILITY  0x11
#define EH_PRIV_TAG_CHIP_ID     0x12
#define EH_PRIV_TAG_EXT_CAP     0x16
#define EH_PRIV_TAG_FW_VERSION  0x17
#define EH_HOST_CAPABILITIES    0x44
#define EH_HOST_CHIP_ID         0x45
#define EH_HOST_RAW_TP          0x46
#define EH_HOST_THROTTLE_HIGH   0x47
#define EH_HOST_THROTTLE_LOW    0x48
#define EH_CAP_BLE_ONLY         (1U << 3)
#define EH_CAP_BR_EDR_ONLY      (1U << 4)
#define EH_CAP_BT_SPI           (1U << 6)
#define EH_EXT_CAP_BT_INTERFACE (1U << 5)

static struct esp_hosted_control_callbacks g_callbacks;
static void *g_callback_argument;

static void eh_config_done(void *argument, rt_err_t result)
{
    (void)argument;
    if (g_callbacks.configured)
    {
        g_callbacks.configured(result, g_callback_argument);
    }
}

static rt_err_t eh_send_config(uint8_t chip_id)
{
    uint8_t event[17] = {
        EH_PRIV_EVENT_INIT, 15,
        EH_HOST_CAPABILITIES, 1, 0,
        EH_HOST_CHIP_ID, 1, 0,
        EH_HOST_RAW_TP, 1, 0,
        EH_HOST_THROTTLE_HIGH, 1, 0,
        EH_HOST_THROTTLE_LOW, 1, 0,
    };

    event[7] = chip_id;
    return esp_hosted_transport_send(ESP_HOSTED_TRANSPORT_IF_PRIVATE, 0,
                                     event, sizeof(event), RT_TRUE,
                                     eh_config_done, RT_NULL);
}

void esp_hosted_control_init(
    const struct esp_hosted_control_callbacks *callbacks, void *argument)
{
    if (callbacks)
    {
        g_callbacks = *callbacks;
    }
    else
    {
        rt_memset(&g_callbacks, 0, sizeof(g_callbacks));
    }
    g_callback_argument = argument;
}

void esp_hosted_control_receive(const uint8_t *data, size_t length)
{
    struct esp_hosted_control_info info;
    size_t offset = 2;
    uint8_t capabilities = 0;
    uint32_t ext_capabilities = 0;
    rt_err_t result;

    if (length < 2 || data[0] != EH_PRIV_EVENT_INIT || data[1] > length - 2)
    {
        return;
    }
    rt_memset(&info, 0, sizeof(info));
    info.coprocessor_id = 0xff;
    while (offset + 2 <= length)
    {
        uint8_t tag = data[offset];
        uint8_t item_length = data[offset + 1];
        const uint8_t *value = data + offset + 2;

        if (item_length > length - offset - 2)
        {
            return;
        }
        if (tag == EH_PRIV_TAG_CHIP_ID && item_length == 1)
        {
            info.coprocessor_id = value[0];
        }
        else if (tag == EH_PRIV_TAG_CAPABILITY && item_length == 1)
        {
            capabilities = value[0];
        }
        else if (tag == EH_PRIV_TAG_FW_VERSION && item_length == 4)
        {
            info.firmware_version = (uint32_t)value[0] |
                                    ((uint32_t)value[1] << 8) |
                                    ((uint32_t)value[2] << 16) |
                                    ((uint32_t)value[3] << 24);
        }
        else if (tag == EH_PRIV_TAG_EXT_CAP && item_length == 4)
        {
            ext_capabilities = (uint32_t)value[0] |
                               ((uint32_t)value[1] << 8) |
                               ((uint32_t)value[2] << 16) |
                               ((uint32_t)value[3] << 24);
        }
        offset += item_length + 2;
    }
    if (info.coprocessor_id == 0xff)
    {
        LOG_E("initialization event has no coprocessor ID");
        return;
    }

    info.bluetooth_supported =
        (capabilities & EH_CAP_BLE_ONLY) &&
        ((capabilities & EH_CAP_BT_SPI) ||
         (ext_capabilities & EH_EXT_CAP_BT_INTERFACE));
    info.bluetooth_dual_mode = (capabilities & EH_CAP_BR_EDR_ONLY) != 0;
    if (g_callbacks.new_session)
    {
        g_callbacks.new_session(&info, g_callback_argument);
    }

    result = esp_hosted_transport_set_slave_capabilities(capabilities,
                                                         ext_capabilities);
    if (result == RT_EOK)
    {
        result = eh_send_config(info.coprocessor_id);
    }
    if (result != RT_EOK && g_callbacks.configured)
    {
        g_callbacks.configured(result, g_callback_argument);
    }
}
