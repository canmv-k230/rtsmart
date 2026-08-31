/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * AIC8800 boot-ROM firmware loader for RT-Smart USB and SDIO transports.
 */
#include "aic8800_wifi.h"
#include "aic8800_firmware_dc.h"
#include "tick.h"

#include <dfs_posix.h>

#define DBG_TAG "aic8800.fw"
#define DBG_LVL AIC8800_DBG_LVL
#include <rtdbg.h>

#define AIC_FW_DMA_ALIGNMENT              64U
#define AIC_FW_USB_TIMEOUT_MS           1000U
#define AIC_FW_PATCH_TABLE_MAX_SIZE     4096U
#define AIC_FW_BLOCK_SIZE                512U
#define AIC_FW_BLOCK_REQUEST_SIZE        520U
#define AIC_FW_PATH_MAX                  256U
#define AIC_FW_EXT_PATCH_MAX               8U

#define AIC_USB_TYPE_CONFIG              0x10U
#define AIC_USB_TYPE_COMMAND             0x11U

#define AIC_TASK_DBG                        1U
#define AIC_DRIVER_TASK                   100U
#define AIC_DBG_MSG(_index) ((rt_uint16_t)((AIC_TASK_DBG << 10) | (_index)))
#define AIC_DBG_MEM_READ_REQ       AIC_DBG_MSG(0)
#define AIC_DBG_MEM_READ_CFM       AIC_DBG_MSG(1)
#define AIC_DBG_MEM_WRITE_REQ      AIC_DBG_MSG(2)
#define AIC_DBG_MEM_WRITE_CFM      AIC_DBG_MSG(3)
#define AIC_DBG_MEM_BLOCK_REQ      AIC_DBG_MSG(11)
#define AIC_DBG_MEM_BLOCK_CFM      AIC_DBG_MSG(12)
#define AIC_DBG_START_APP_REQ      AIC_DBG_MSG(13)
#define AIC_DBG_START_APP_CFM      AIC_DBG_MSG(14)
#define AIC_DBG_MEM_MASK_REQ       AIC_DBG_MSG(17)
#define AIC_DBG_MEM_MASK_CFM       AIC_DBG_MSG(18)

#define AIC_START_APP_AUTO                 1U
#define AIC_START_APP_REBOOT               3U
#define AIC_START_APP_FUNCTION             4U
#define AIC_START_APP_DUMMY                5U
#define AIC_REBOOT_DELAY_MS              2000U

#define AIC_DC_CONFIG_BASE         0x00010164U
#define AIC_DC_CALIB_ADDRESS       0x00130000U
#define AIC_DC_CALIB_ENTRY         0x00130009U
#define AIC_DC_ROM_ENTRY           0x00150000U
#define AIC_DC_PATCH_ADDRESS       0x00180000U
#define AIC_DC_PATCH_DESCRIBE_SIZE        128U
#define AIC_DC_MCU_PATCH_ENABLE    0x40100020U
#define AIC_DC_TABLE_CHUNK_SIZE           512U
#define AIC_DC_PATCH_VAR_MAGIC      0x47564150U
#define AIC_DC_USER_TX_POWER_FLAG  (1U << 1)

#define AIC_CHIP_REV_U01                  0x01U
#define AIC_CHIP_REV_U02                  0x03U
#define AIC_CHIP_REV_U03                  0x07U
#define AIC_CHIP_REV_U05                  0x1fU
#define AIC_CHIP_SUB_REV_U04              0x20U
#define AIC_CHIP_ID_H_MASK                0xc0U

#define AIC_PATCH_TAG              "AICBT_PT_TAG"
#define AIC_PATCH_INFO_TYPE                0U
#define AIC_PATCH_BTMODE_TYPE              3U
#define AIC_PATCH_POWER_ON_TYPE            4U
#define AIC_PATCH_VERSION_TYPE             6U

#define AIC_PATCH_MAGIC             0x48435450UL
#define AIC_PATCH_MAGIC_2           0x50544348UL
#define AIC_PATCH_STRUCT_MAGIC_OFFSET       0U
#define AIC_PATCH_STRUCT_PAIR_START_OFFSET  4U
#define AIC_PATCH_STRUCT_MAGIC_2_OFFSET     8U
#define AIC_PATCH_STRUCT_PAIR_COUNT_OFFSET 12U
#define AIC_PATCH_STRUCT_BLOCK_SIZE_OFFSET 48U

enum aic_firmware_family
{
    AIC_FW_FAMILY_NONE = 0,
    AIC_FW_FAMILY_8800,
    AIC_FW_FAMILY_D80,
    AIC_FW_FAMILY_D80X2,
    AIC_FW_FAMILY_DC,
};

enum aic_firmware_state
{
    AIC_FW_STATE_COLD = 0,
    AIC_FW_STATE_WAIT_BOOT,
    AIC_FW_STATE_WAIT_RUNTIME,
    AIC_FW_STATE_READY,
};

struct aic_firmware_pair
{
    rt_uint32_t address;
    rt_uint32_t value;
};

struct aic_firmware_mask_pair
{
    rt_uint32_t address;
    rt_uint32_t mask;
    rt_uint32_t value;
};

struct aic_patch_info
{
    rt_uint32_t adid_address;
    rt_uint32_t patch_address;
    rt_uint32_t ext_count;
    rt_uint32_t ext_id[AIC_FW_EXT_PATCH_MAX];
    rt_uint32_t ext_address[AIC_FW_EXT_PATCH_MAX];
};

static enum aic_firmware_state g_firmware_state = AIC_FW_STATE_COLD;
static enum aic_firmware_family g_transition_family = AIC_FW_FAMILY_NONE;

static rt_uint16_t aic_fw_get_le16(const rt_uint8_t *data)
{
    return (rt_uint16_t)data[0] | ((rt_uint16_t)data[1] << 8);
}

static rt_uint32_t aic_fw_get_le32(const rt_uint8_t *data)
{
    return (rt_uint32_t)data[0] | ((rt_uint32_t)data[1] << 8) |
           ((rt_uint32_t)data[2] << 16) | ((rt_uint32_t)data[3] << 24);
}

static rt_uint32_t aic_firmware_crc32_update(rt_uint32_t crc,
                                             const rt_uint8_t *data,
                                             rt_size_t length)
{
    rt_size_t index;

    for (index = 0; index < length; index++)
    {
        rt_uint32_t bit;

        crc ^= data[index];
        for (bit = 0; bit < 8U; bit++)
        {
            crc = (crc >> 1) ^ (0xedb88320UL &
                  (rt_uint32_t)-(rt_int32_t)(crc & 1U));
        }
    }
    return crc;
}

static void aic_fw_put_le16(rt_uint8_t *data, rt_uint16_t value)
{
    data[0] = (rt_uint8_t)value;
    data[1] = (rt_uint8_t)(value >> 8);
}

static void aic_fw_put_le32(rt_uint8_t *data, rt_uint32_t value)
{
    data[0] = (rt_uint8_t)value;
    data[1] = (rt_uint8_t)(value >> 8);
    data[2] = (rt_uint8_t)(value >> 16);
    data[3] = (rt_uint8_t)(value >> 24);
}

static rt_size_t aic_fw_align4(rt_size_t length)
{
    return (length + 3U) & ~3U;
}

static rt_bool_t aic_firmware_product_is_runtime(rt_uint16_t product_id)
{
    switch (product_id)
    {
    case AIC8800_USB_PID_AIC8801:
    case AIC8800_USB_PID_AIC8800DC:
    case AIC8800_USB_PID_AIC8800DW:
    case AIC8800_USB_PID_AIC8800D81:
    case AIC8800_USB_PID_AIC8800D83:
    case AIC8800_USB_PID_AIC8800D84:
    case AIC8800_USB_PID_AIC8800D85:
    case AIC8800_USB_PID_AIC8800D86:
    case AIC8800_USB_PID_AIC8800D88:
    case AIC8800_USB_PID_AIC8800D41:
    case AIC8800_USB_PID_AIC8800D81X2:
    case AIC8800_USB_PID_AIC8800D89X2:
        return RT_TRUE;
    default:
        return RT_FALSE;
    }
}

static enum aic_firmware_family aic_firmware_family_from_product(
    rt_uint16_t vendor_id, rt_uint16_t product_id)
{
    if (vendor_id == AIC8800_USB_VENDOR_ID)
    {
        if (product_id == AIC8800_USB_PID_AIC8800 ||
            product_id == AIC8800_USB_PID_AIC8801)
        {
            return AIC_FW_FAMILY_8800;
        }
        if (product_id == AIC8800_USB_PID_AIC8800D80 ||
            product_id == AIC8800_USB_PID_AIC8800D81 ||
            product_id == AIC8800_USB_PID_AIC8800D83 ||
            product_id == AIC8800_USB_PID_AIC8800D84 ||
            product_id == AIC8800_USB_PID_AIC8800D85 ||
            product_id == AIC8800_USB_PID_AIC8800D86 ||
            product_id == AIC8800_USB_PID_AIC8800D88 ||
            product_id == AIC8800_USB_PID_AIC8800D40 ||
            product_id == AIC8800_USB_PID_AIC8800D41)
        {
            return AIC_FW_FAMILY_D80;
        }
        if (product_id == AIC8800_USB_PID_AIC8800DC ||
            product_id == AIC8800_USB_PID_AIC8800DW)
        {
            return AIC_FW_FAMILY_DC;
        }
    }
    if (vendor_id == AIC8800_USB_VENDOR_ID_V2)
    {
        if (product_id == AIC8800_USB_PID_AIC8800D80X2 ||
            product_id == AIC8800_USB_PID_AIC8800D81X2 ||
            product_id == AIC8800_USB_PID_AIC8800D89X2)
        {
            return AIC_FW_FAMILY_D80X2;
        }
        /* Rebranded D80/D81 modules keep the D80 firmware set and only
         * change the USB identity. */
        if (product_id == AIC8800_USB_PID_AIC8800D81 ||
            product_id == AIC8800_USB_PID_AIC8800D83 ||
            product_id == AIC8800_USB_PID_AIC8800D84 ||
            product_id == AIC8800_USB_PID_AIC8800D85 ||
            product_id == AIC8800_USB_PID_AIC8800D86 ||
            product_id == AIC8800_USB_PID_AIC8800D88)
        {
            return AIC_FW_FAMILY_D80;
        }
    }
    return AIC_FW_FAMILY_NONE;
}

static const char *aic_firmware_subdirectory(enum aic_firmware_family family)
{
    switch (family)
    {
    case AIC_FW_FAMILY_8800:
        return "aic8800";
    case AIC_FW_FAMILY_D80:
        return "aic8800D80";
    case AIC_FW_FAMILY_D80X2:
        return "aic8800D80X2";
    case AIC_FW_FAMILY_DC:
        return "aic8800DC";
    default:
        return "";
    }
}

int aic8800_firmware_open_file(const struct aic8800_context *context,
                               const char *directory, const char *name,
                               char *path, rt_size_t path_capacity)
{
    int descriptor;

#if defined(AIC8800_WIFI_TRANSPORT_USB) && \
    defined(AIC8800_WIFI_TRANSPORT_SDIO)
    const char *transport_directory;

    if (!context || !directory || !name || !path || !path_capacity)
    {
        return -1;
    }
    transport_directory = context->transport == AIC8800_TRANSPORT_SDIO ?
                          "sdio" : "usb";
    rt_snprintf(path, path_capacity, "%s/%s/%s/%s",
                AIC8800_WIFI_FIRMWARE_PATH, transport_directory,
                directory, name);
    descriptor = open(path, O_RDONLY, 0);
    if (descriptor >= 0)
    {
        return descriptor;
    }
#else
    (void)context;
    if (!directory || !name || !path || !path_capacity)
    {
        return -1;
    }
#endif

    rt_snprintf(path, path_capacity, "%s/%s/%s",
                AIC8800_WIFI_FIRMWARE_PATH, directory, name);
    descriptor = open(path, O_RDONLY, 0);
    if (descriptor >= 0)
    {
        return descriptor;
    }

    rt_snprintf(path, path_capacity, "%s/%s",
                AIC8800_WIFI_FIRMWARE_PATH, name);
    return open(path, O_RDONLY, 0);
}

static int aic_firmware_open(const struct aic8800_context *context,
                             enum aic_firmware_family family,
                             const char *name, char path[AIC_FW_PATH_MAX])
{
    return aic8800_firmware_open_file(
        context, aic_firmware_subdirectory(family), name,
        path, AIC_FW_PATH_MAX);
}

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
rt_err_t aic8800_firmware_wait_available(struct aic8800_context *context,
                                          rt_uint32_t timeout_ms)
{
    enum aic_firmware_family family;
    const char *probe_name;
    char path[AIC_FW_PATH_MAX];
    rt_tick_t start;
    rt_tick_t timeout;

    if (!context || !context->transport_connected)
    {
        return -RT_EINVAL;
    }
    family = aic_firmware_family_from_product(context->vendor_id,
                                               context->product_id);
    if (family == AIC_FW_FAMILY_8800)
    {
        probe_name = "fmacfw.bin";
    }
    else if (family == AIC_FW_FAMILY_D80)
    {
        probe_name = "fw_patch_table_8800d80_u02.bin";
    }
    else if (family == AIC_FW_FAMILY_DC)
    {
        probe_name = "fmacfw_patch_8800dc_u02.bin";
    }
    else
    {
        return -RT_ENOSYS;
    }

    start = rt_tick_get();
    timeout = rt_tick_from_millisecond(timeout_ms);
    do
    {
        int descriptor = aic_firmware_open(
            context, family, probe_name, path);

        if (descriptor >= 0)
        {
            close(descriptor);
            return RT_EOK;
        }
        if (!context->transport_connected)
        {
            return -RT_EIO;
        }
        rt_thread_mdelay(50U);
    }
    while ((rt_tick_t)(rt_tick_get() - start) < timeout);

    LOG_E("firmware file did not become available: %s", path);
    return -RT_ETIMEOUT;
}
#endif

#ifdef AIC8800_WIFI_TRANSPORT_USB
static rt_err_t aic_firmware_usb_result(int result)
{
    if (!result)
    {
        return RT_EOK;
    }
    if (aic8800_usb_is_timeout(result))
    {
        return -RT_ETIMEOUT;
    }
    return -RT_EIO;
}

static struct usb_endpoint_descriptor *aic_firmware_tx_endpoint(
    struct aic8800_context *context)
{
    if (!context)
    {
        return RT_NULL;
    }
    if (aic_firmware_product_is_runtime(context->product_id) &&
        context->message_out)
    {
        return context->message_out;
    }
    return context->data_out;
}

static struct usb_endpoint_descriptor *aic_firmware_rx_endpoint(
    struct aic8800_context *context)
{
    if (!context)
    {
        return RT_NULL;
    }
    if (aic_firmware_product_is_runtime(context->product_id) &&
        context->message_in)
    {
        return context->message_in;
    }
    return context->data_in;
}

static rt_err_t aic_firmware_usb_transmit(
    struct aic8800_context *context, const void *data, rt_size_t length)
{
    struct usb_endpoint_descriptor *endpoint;
    rt_err_t lock_result;
    rt_err_t transfer_result;
    int result;

    if (!context || !context->transport_connected || !context->hport ||
        !context->hport->connected ||
        !context->tx_mutex_initialized || !data || !length)
    {
        return -RT_EINVAL;
    }
    if (length > AIC8800_USB_MAX_COMMAND_SIZE)
    {
        return -RT_EFULL;
    }
    lock_result = rt_mutex_take(context->tx_mutex, RT_WAITING_FOREVER);
    if (lock_result != RT_EOK)
    {
        return lock_result;
    }
    if (!context->transport_connected || !context->hport ||
        !context->hport->connected)
    {
        rt_mutex_release(context->tx_mutex);
        return -RT_EIO;
    }
    /* The runtime interface replaces the endpoint descriptors after USB
     * re-enumeration.  Resolve it only while the TX lock is held so a stale
     * boot-interface descriptor can never be submitted. */
    endpoint = aic_firmware_tx_endpoint(context);
    if (!endpoint)
    {
        rt_mutex_release(context->tx_mutex);
        return -RT_EIO;
    }
    usbh_bulk_urb_fill(&context->tx_urb, context->hport, endpoint,
                       (rt_uint8_t *)data,
                       length, AIC8800_WIFI_COMMAND_TIMEOUT_MS,
                       RT_NULL, RT_NULL);
    context->tx_urb.transfer_flags = URB_ZERO_PACKET;
    result = usbh_submit_urb(&context->tx_urb);
    if (!result && context->tx_urb.actual_length != length)
    {
        transfer_result = -RT_EIO;
    }
    else
    {
        transfer_result = aic_firmware_usb_result(result);
    }
    rt_mutex_release(context->tx_mutex);
    return transfer_result;
}
#endif

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
static rt_err_t aic_firmware_sdio_transmit(
    struct aic8800_context *context, const void *data, rt_size_t length)
{
    return aic8800_sdio_firmware_transmit(context, data, length);
}
#endif

static rt_err_t aic_firmware_transport_transmit(
    struct aic8800_context *context, const void *data, rt_size_t length)
{
    if (!context)
    {
        return -RT_EINVAL;
    }
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
        return aic_firmware_sdio_transmit(context, data, length);
#else
        return -RT_ENOSYS;
#endif
    }
#ifdef AIC8800_WIFI_TRANSPORT_USB
    return aic_firmware_usb_transmit(context, data, length);
#else
    return -RT_ENOSYS;
#endif
}

static rt_err_t aic_firmware_find_confirmation(
    const rt_uint8_t *buffer, rt_size_t length, rt_uint16_t expected_id,
    void *response, rt_size_t response_capacity, rt_size_t *response_length)
{
    const rt_uint8_t *cursor;
    rt_size_t remaining;

    if (!buffer || (response_capacity && !response))
    {
        return -RT_EINVAL;
    }
    if (response_length)
    {
        *response_length = 0;
    }
    cursor = buffer;
    remaining = length;

    while (remaining >= AIC8800_USB_HEADER_SIZE)
    {
        rt_uint16_t packet_length = aic_fw_get_le16(cursor);
        rt_uint8_t type = cursor[2] & 0x7fU;
        rt_size_t raw_length;
        rt_size_t record_length;

        if (!packet_length && !type)
        {
            break;
        }
        raw_length = (type & AIC_USB_TYPE_CONFIG) == AIC_USB_TYPE_CONFIG ?
                     (rt_size_t)packet_length + 4U :
                     (rt_size_t)packet_length + AIC8800_USB_RX_HEADER_SIZE;
        if (raw_length < AIC8800_USB_HEADER_SIZE || raw_length > remaining)
        {
            return -RT_EIO;
        }
        record_length = aic_fw_align4(raw_length);
        if (record_length > remaining)
        {
            record_length = raw_length;
        }
        if (type == AIC_USB_TYPE_COMMAND)
        {
            rt_uint16_t message_id;
            rt_uint16_t parameter_length;

            if (raw_length < 16U)
            {
                return -RT_EIO;
            }
            message_id = aic_fw_get_le16(cursor + 4);
            parameter_length = aic_fw_get_le16(cursor + 10);
            if ((rt_size_t)parameter_length > raw_length - 16U)
            {
                return -RT_EIO;
            }
            if (message_id == expected_id)
            {
                if (response && parameter_length > response_capacity)
                {
                    return -RT_EFULL;
                }
                if (parameter_length && response)
                {
                    rt_memcpy(response, cursor + 16, parameter_length);
                }
                if (response_length)
                {
                    *response_length = parameter_length;
                }
                return RT_EOK;
            }
        }
        cursor += record_length;
        remaining -= record_length;
    }
    return -RT_EEMPTY;
}

#ifdef AIC8800_WIFI_TRANSPORT_USB
/* One armed bulk IN request.  Confirmations are collected through completion
 * callbacks rather than a blocking submit so a slow command never causes the
 * request to be cancelled: aborting a bulk IN that the device is in the middle
 * of answering loses the reply and leaves the endpoint's data toggle out of
 * step with the device, which kills the command channel for good.  The vendor
 * drivers avoid this the same way, by keeping the pipe permanently armed. */
struct aic_firmware_receive_slot
{
    struct usbh_urb urb;
    struct rt_semaphore *ready;
    struct usb_endpoint_descriptor *endpoint;
    rt_uint8_t *buffer;
    volatile int result;
    volatile rt_bool_t complete;
    rt_bool_t armed;
};

static void aic_firmware_usb_receive_complete(void *parameter, int length)
{
    struct aic_firmware_receive_slot *slot = parameter;

    if (!slot || !slot->ready)
    {
        return;
    }
    slot->result = length;
    slot->complete = RT_TRUE;
    rt_sem_release(slot->ready);
}

static int aic_firmware_usb_arm_receive(
    struct aic8800_context *context,
    struct aic_firmware_receive_slot *slot)
{
    int result;

    slot->result = 0;
    slot->complete = RT_FALSE;
    usbh_bulk_urb_fill(&slot->urb, context->hport, slot->endpoint,
                       slot->buffer, AIC8800_WIFI_RX_BUFFER_SIZE, 0,
                       aic_firmware_usb_receive_complete, slot);
    result = usbh_submit_urb(&slot->urb);
    slot->armed = result == 0;
    return result;
}

static rt_uint8_t *aic_firmware_usb_receive_buffer(
    struct aic8800_context *context,
    const struct usb_endpoint_descriptor *endpoint)
{
    if (endpoint == context->message_in && context->message_worker.slots)
    {
        return context->message_worker.slots[0].buffer;
    }
    if (context->data_worker.slots)
    {
        return context->data_worker.slots[0].buffer;
    }
    return RT_NULL;
}

static rt_err_t aic_firmware_usb_receive_confirmation(
    struct aic8800_context *context, rt_uint16_t expected_id,
    void *response, rt_size_t response_capacity, rt_size_t *response_length)
{
    struct aic_firmware_receive_slot slots[2];
    struct rt_semaphore ready;
    struct usb_endpoint_descriptor *primary;
    rt_size_t slot_count = 0;
    rt_size_t index;
    rt_tick_t deadline;
    rt_err_t result = -RT_ETIMEOUT;
    rt_bool_t finished = RT_FALSE;

    if (response_length)
    {
        *response_length = 0;
    }
    if (!context || !context->transport_connected || !context->hport ||
        !context->hport->connected)
    {
        return -RT_EIO;
    }
    rt_memset(slots, 0, sizeof(slots));
    primary = aic_firmware_rx_endpoint(context);
    if (!primary)
    {
        return -RT_EIO;
    }
    slots[slot_count].endpoint = primary;
    slots[slot_count].buffer =
        aic_firmware_usb_receive_buffer(context, primary);
    if (!slots[slot_count].buffer)
    {
        return -RT_EIO;
    }
    slot_count++;
    /* The boot ROM normally returns debug confirmations on the dedicated
     * message endpoint.  A function-call helper can use the data endpoint
     * while it owns the device, so firmware probe must service both just as
     * the vendor driver does with its parallel RX queues. */
    if (aic_firmware_product_is_runtime(context->product_id) &&
        context->data_in && context->data_in != primary)
    {
        slots[slot_count].endpoint = context->data_in;
        slots[slot_count].buffer =
            aic_firmware_usb_receive_buffer(context, context->data_in);
        if (slots[slot_count].buffer &&
            slots[slot_count].buffer != slots[0].buffer)
        {
            slot_count++;
        }
    }

    rt_sem_init(&ready, "aicfwrx", 0, RT_IPC_FLAG_FIFO);
    for (index = 0; index < slot_count; index++)
    {
        slots[index].ready = &ready;
        if (aic_firmware_usb_arm_receive(context, &slots[index]) != 0 &&
            index == 0)
        {
            result = -RT_EIO;
            finished = RT_TRUE;
            break;
        }
    }

    deadline = rt_tick_get() +
               rt_tick_from_millisecond(AIC8800_WIFI_COMMAND_TIMEOUT_MS);
    while (!finished && context->transport_connected && context->hport &&
           context->hport->connected)
    {
        rt_int32_t remaining = (rt_int32_t)(deadline - rt_tick_get());

        if (remaining <= 0 || rt_sem_take(&ready, remaining) != RT_EOK)
        {
            result = -RT_ETIMEOUT;
            break;
        }
        for (index = 0; index < slot_count && !finished; index++)
        {
            struct aic_firmware_receive_slot *slot = &slots[index];
            rt_err_t parse_result;
            int length;

            if (!slot->complete)
            {
                continue;
            }
            length = slot->result;
            slot->complete = RT_FALSE;
            slot->armed = RT_FALSE;
            if (length < 0)
            {
                /* Only the endpoint the ROM answers on is fatal; the
                 * speculative data endpoint is simply left idle. */
                if (index == 0)
                {
                    result = aic_firmware_usb_result(length);
                    finished = RT_TRUE;
                }
                continue;
            }
            if (length > 0)
            {
                parse_result = aic_firmware_find_confirmation(
                    slot->buffer, (rt_size_t)length, expected_id,
                    response, response_capacity, response_length);
                if (parse_result != -RT_EEMPTY)
                {
                    result = parse_result;
                    finished = RT_TRUE;
                    continue;
                }
            }
            (void)aic_firmware_usb_arm_receive(context, slot);
        }
    }

    for (index = 0; index < slot_count; index++)
    {
        if (slots[index].armed)
        {
            /* Synchronous: no completion can run past this point, so the
             * stack-allocated requests and semaphore stay valid. */
            usbh_kill_urb(&slots[index].urb);
        }
    }
    rt_sem_detach(&ready);
    if (result == -RT_ETIMEOUT && !context->transport_connected)
    {
        result = -RT_EIO;
    }
    return result;
}
#endif

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
static rt_err_t aic_firmware_sdio_receive_confirmation(
    struct aic8800_context *context, rt_uint16_t expected_id,
    void *response, rt_size_t response_capacity, rt_size_t *response_length)
{
    rt_uint32_t attempts = AIC8800_WIFI_COMMAND_TIMEOUT_MS /
                           AIC_FW_USB_TIMEOUT_MS + 1U;

    if (response_length)
    {
        *response_length = 0;
    }
    if (!context || !context->transport_connected ||
        !context->sdio_rx_buffer)
    {
        return -RT_EIO;
    }
    while (attempts-- && context->transport_connected)
    {
        rt_size_t receive_length = 0;
        rt_err_t result = aic8800_sdio_firmware_receive(
            context, context->sdio_rx_buffer,
            AIC8800_WIFI_SDIO_RX_BUFFER_SIZE, &receive_length,
            AIC_FW_USB_TIMEOUT_MS);

        if (result == -RT_ETIMEOUT || result == -RT_EEMPTY)
        {
            continue;
        }
        if (result != RT_EOK)
        {
            return result;
        }
        result = aic_firmware_find_confirmation(
            context->sdio_rx_buffer, receive_length, expected_id,
            response, response_capacity, response_length);
        if (result != -RT_EEMPTY)
        {
            return result;
        }
    }
    return context->transport_connected ? -RT_ETIMEOUT : -RT_EIO;
}
#endif

static rt_err_t aic_firmware_receive_confirmation(
    struct aic8800_context *context, rt_uint16_t expected_id,
    void *response, rt_size_t response_capacity, rt_size_t *response_length)
{
    if (!context)
    {
        return -RT_EINVAL;
    }
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
        return aic_firmware_sdio_receive_confirmation(
            context, expected_id, response, response_capacity,
            response_length);
#else
        return -RT_ENOSYS;
#endif
    }
#ifdef AIC8800_WIFI_TRANSPORT_USB
    return aic_firmware_usb_receive_confirmation(
        context, expected_id, response, response_capacity, response_length);
#else
    return -RT_ENOSYS;
#endif
}

static rt_err_t aic_firmware_debug_command(
    struct aic8800_context *context, rt_uint16_t request_id,
    rt_uint16_t confirmation_id, const void *request,
    rt_size_t request_length, void *response, rt_size_t response_capacity,
    rt_size_t *response_length)
{
    rt_uint8_t *frame;
    rt_size_t frame_length;
    rt_err_t result;

    if ((request_length && !request) ||
        request_length > AIC8800_USB_MAX_COMMAND_SIZE - 16U)
    {
        return -RT_EINVAL;
    }
    frame_length = 16U + request_length;
    frame = rt_malloc_align(frame_length, AIC_FW_DMA_ALIGNMENT);
    if (!frame)
    {
        return -RT_ENOMEM;
    }
    rt_memset(frame, 0, frame_length);
    aic_fw_put_le16(frame, (rt_uint16_t)(request_length + 12U));
    frame[2] = AIC_USB_TYPE_COMMAND;
    aic_fw_put_le16(frame + 8, request_id);
    aic_fw_put_le16(frame + 10, AIC_TASK_DBG);
    aic_fw_put_le16(frame + 12, AIC_DRIVER_TASK);
    aic_fw_put_le16(frame + 14, (rt_uint16_t)request_length);
    if (request_length)
    {
        rt_memcpy(frame + 16, request, request_length);
    }
    result = aic_firmware_transport_transmit(context, frame, frame_length);
    rt_free_align(frame);
    if (result != RT_EOK || !confirmation_id)
    {
        return result;
    }
    return aic_firmware_receive_confirmation(
        context, confirmation_id, response, response_capacity,
        response_length);
}

static rt_err_t aic_firmware_mem_read(struct aic8800_context *context,
                                      rt_uint32_t address,
                                      rt_uint32_t *value)
{
    rt_uint8_t request[4];
    rt_uint8_t response[8];
    rt_size_t response_length = 0;
    rt_err_t result;

    if (!value)
    {
        return -RT_EINVAL;
    }

    aic_fw_put_le32(request, address);
    result = aic_firmware_debug_command(
        context, AIC_DBG_MEM_READ_REQ, AIC_DBG_MEM_READ_CFM,
        request, sizeof(request), response, sizeof(response), &response_length);
    if (result != RT_EOK)
    {
        return result;
    }
    if (response_length < sizeof(response) ||
        aic_fw_get_le32(response) != address)
    {
        return -RT_EIO;
    }
    *value = aic_fw_get_le32(response + 4);
    return RT_EOK;
}

static rt_err_t aic_firmware_mem_write(struct aic8800_context *context,
                                       rt_uint32_t address,
                                       rt_uint32_t value)
{
    rt_uint8_t request[8];

    aic_fw_put_le32(request, address);
    aic_fw_put_le32(request + 4, value);
    return aic_firmware_debug_command(
        context, AIC_DBG_MEM_WRITE_REQ, AIC_DBG_MEM_WRITE_CFM,
        request, sizeof(request), RT_NULL, 0, RT_NULL);
}

static rt_err_t aic_firmware_mem_mask_write(
    struct aic8800_context *context, rt_uint32_t address,
    rt_uint32_t mask, rt_uint32_t value)
{
    rt_uint8_t request[12];

    aic_fw_put_le32(request, address);
    aic_fw_put_le32(request + 4, mask);
    aic_fw_put_le32(request + 8, value);
    return aic_firmware_debug_command(
        context, AIC_DBG_MEM_MASK_REQ, AIC_DBG_MEM_MASK_CFM,
        request, sizeof(request), RT_NULL, 0, RT_NULL);
}

static rt_err_t aic_firmware_mem_block_write(
    struct aic8800_context *context, rt_uint32_t address,
    const rt_uint8_t *data, rt_size_t length)
{
    rt_uint8_t *request;
    rt_uint8_t response[4];
    rt_size_t response_length = 0;
    rt_err_t result;

    if (!data || !length || length > AIC_FW_BLOCK_SIZE)
    {
        return -RT_EINVAL;
    }
    request = rt_malloc_align(AIC_FW_BLOCK_REQUEST_SIZE,
                              AIC_FW_DMA_ALIGNMENT);
    if (!request)
    {
        return -RT_ENOMEM;
    }
    rt_memset(request, 0, AIC_FW_BLOCK_REQUEST_SIZE);
    aic_fw_put_le32(request, address);
    aic_fw_put_le32(request + 4, (rt_uint32_t)length);
    rt_memcpy(request + 8, data, length);
    result = aic_firmware_debug_command(
        context, AIC_DBG_MEM_BLOCK_REQ, AIC_DBG_MEM_BLOCK_CFM,
        request, AIC_FW_BLOCK_REQUEST_SIZE, response, sizeof(response),
        &response_length);
    rt_free_align(request);
    if (result == RT_EOK && response_length >= 4U &&
        aic_fw_get_le32(response) != 0)
    {
        return -RT_ERROR;
    }
    return result;
}

static rt_err_t aic_firmware_start_app(struct aic8800_context *context,
                                       rt_uint32_t address,
                                       rt_uint32_t type)
{
    rt_uint8_t request[8];
    rt_err_t result;

    aic_fw_put_le32(request, address);
    aic_fw_put_le32(request + 4, type);
    result = aic_firmware_debug_command(
        context, AIC_DBG_START_APP_REQ, 0, request, sizeof(request),
        RT_NULL, 0, RT_NULL);
    if (result == RT_EOK)
    {
        rt_thread_mdelay(20);
    }
    return result;
}

static rt_err_t aic_firmware_start_app_confirmed(
    struct aic8800_context *context, rt_uint32_t address, rt_uint32_t type)
{
    rt_uint8_t request[8];
    rt_uint8_t response[4];
    rt_size_t response_length = 0;
    rt_err_t result;

    aic_fw_put_le32(request, address);
    aic_fw_put_le32(request + 4, type);
    result = aic_firmware_debug_command(
        context, AIC_DBG_START_APP_REQ, AIC_DBG_START_APP_CFM,
        request, sizeof(request), response, sizeof(response), &response_length);
    if (result != RT_EOK)
    {
        return result;
    }
    if (response_length < sizeof(response))
    {
        return -RT_EIO;
    }
    rt_thread_mdelay(20);
    return RT_EOK;
}

static rt_err_t aic_firmware_write_pairs(
    struct aic8800_context *context, const struct aic_firmware_pair *pairs,
    rt_size_t count)
{
    rt_size_t index;

    for (index = 0; index < count; index++)
    {
        rt_err_t result = aic_firmware_mem_write(
            context, pairs[index].address, pairs[index].value);

        if (result != RT_EOK)
        {
            LOG_E("write 0x%08x failed: %d", pairs[index].address, result);
            return result;
        }
    }
    return RT_EOK;
}

static rt_bool_t aic_firmware_file_available(
    const struct aic8800_context *context,
    enum aic_firmware_family family, const char *name)
{
    char path[AIC_FW_PATH_MAX];
    int descriptor = aic_firmware_open(context, family, name, path);

    if (descriptor < 0)
    {
        return RT_FALSE;
    }
    close(descriptor);
    return RT_TRUE;
}

#ifdef AIC8800_WIFI_VERIFY_FIRMWARE
static rt_err_t aic_firmware_verify_full(struct aic8800_context *context,
                                         const char *path,
                                         rt_uint32_t address,
                                         rt_uint32_t total)
{
    rt_uint8_t word[4];
    int descriptor;
    rt_uint32_t offset = 0;

    descriptor = open(path, O_RDONLY, 0);
    if (descriptor < 0)
    {
        return -RT_EIO;
    }
    while (offset < total)
    {
        rt_size_t expected = total - offset;
        rt_size_t used = 0;
        rt_uint32_t source;
        rt_uint32_t target;
        rt_uint32_t mask;

        if (expected > sizeof(word))
        {
            expected = sizeof(word);
        }
        while (used < expected)
        {
            ssize_t length = read(descriptor, word + used, expected - used);

            if (length <= 0)
            {
                close(descriptor);
                return -RT_EIO;
            }
            used += (rt_size_t)length;
        }
        source = aic_fw_get_le32(word);
        if (aic_firmware_mem_read(context, address + offset, &target) != RT_EOK)
        {
            close(descriptor);
            return -RT_EIO;
        }
        mask = expected == sizeof(word) ? 0xffffffffUL :
               ((1UL << (expected * 8U)) - 1U);
        if ((source & mask) != (target & mask))
        {
            LOG_E("full verify failed at 0x%08x: %08x/%08x",
                  address + offset, source, target);
            close(descriptor);
            return -RT_EIO;
        }
        offset += (rt_uint32_t)expected;
    }
    close(descriptor);
    LOG_D("full firmware readback verified: %u bytes", (unsigned int)total);
    return RT_EOK;
}
#endif

static rt_err_t aic_firmware_upload(struct aic8800_context *context,
                                    enum aic_firmware_family family,
                                    const char *name, rt_uint32_t address)
{
    rt_uint8_t *block;
    rt_uint32_t total = 0;
    rt_uint32_t first_word = 0;
    rt_uint32_t last_word = 0;
    rt_uint32_t crc = 0xffffffffUL;
    char path[AIC_FW_PATH_MAX];
    int descriptor;
    rt_err_t result = RT_EOK;

    descriptor = aic_firmware_open(context, family, name, path);
    if (descriptor < 0)
    {
        LOG_E("firmware file not found: %s/%s/%s",
              AIC8800_WIFI_FIRMWARE_PATH,
              aic_firmware_subdirectory(family), name);
        return -RT_EIO;
    }
    block = rt_malloc_align(AIC_FW_BLOCK_SIZE, AIC_FW_DMA_ALIGNMENT);
    if (!block)
    {
        close(descriptor);
        return -RT_ENOMEM;
    }
    LOG_D("uploading %s to 0x%08x", path, address);
    while (1)
    {
        ssize_t length = read(descriptor, block, AIC_FW_BLOCK_SIZE);

        if (length < 0)
        {
            result = -RT_EIO;
            break;
        }
        if (!length)
        {
            break;
        }
        if ((rt_uint32_t)length > 0xffffffffUL - total)
        {
            result = -RT_EFULL;
            break;
        }
        if (!total && length >= 4)
        {
            first_word = aic_fw_get_le32(block);
        }
        if (length >= 4)
        {
            last_word = aic_fw_get_le32(block + length - 4);
        }
        crc = aic_firmware_crc32_update(crc, block, (rt_size_t)length);
        result = aic_firmware_mem_block_write(
            context, address + total, block, (rt_size_t)length);
        if (result != RT_EOK)
        {
            LOG_E("upload %s failed at offset 0x%x: %d",
                  name, total, result);
            break;
        }
        total += (rt_uint32_t)length;
    }
    rt_free_align(block);
    close(descriptor);
    if (result == RT_EOK && !total)
    {
        result = -RT_EIO;
    }
    /* DC/DW exposes stale patch-RAM values through DBG_MEM_READ until the
     * patched ROM is started.  Its vendor loaders use the block-write CFM as
     * completion and do not perform immediate readback. */
    if (result == RT_EOK && total >= 4U && family != AIC_FW_FAMILY_DC)
    {
        rt_uint32_t target_first;
        rt_uint32_t target_last;

        result = aic_firmware_mem_read(context, address, &target_first);
        if (result == RT_EOK)
        {
            result = aic_firmware_mem_read(
                context, address + total - 4U, &target_last);
        }
        if (result == RT_EOK &&
            (target_first != first_word || target_last != last_word))
        {
            LOG_E("verify %s failed: first=%08x/%08x last=%08x/%08x",
                  name, first_word, target_first, last_word, target_last);
            result = -RT_EIO;
        }
        else if (result == RT_EOK)
        {
            LOG_D("verified %s transport: first=%08x last=%08x crc32=%08x",
                  name, first_word, last_word, crc ^ 0xffffffffUL);
        }
#ifdef AIC8800_WIFI_VERIFY_FIRMWARE
        if (result == RT_EOK)
        {
            result = aic_firmware_verify_full(context, path, address, total);
        }
#endif
    }
    if (result == RT_EOK)
    {
        LOG_I("uploaded %s (%u bytes)", name, total);
    }
    return result;
}

static rt_err_t aic_firmware_read_blob(
    const struct aic8800_context *context,
    enum aic_firmware_family family, const char *name,
    char path[AIC_FW_PATH_MAX], rt_uint8_t **data, rt_size_t *data_length)
{
    rt_uint8_t *buffer;
    rt_size_t used = 0;
    int descriptor;
    rt_err_t result = RT_EOK;

    if (!path || !data || !data_length)
    {
        return -RT_EINVAL;
    }
    descriptor = aic_firmware_open(context, family, name, path);
    if (descriptor < 0)
    {
        return -RT_EIO;
    }
    buffer = rt_malloc(AIC_FW_PATCH_TABLE_MAX_SIZE + 1U);
    if (!buffer)
    {
        close(descriptor);
        return -RT_ENOMEM;
    }
    while (used <= AIC_FW_PATCH_TABLE_MAX_SIZE)
    {
        ssize_t length = read(descriptor, buffer + used,
                              AIC_FW_PATCH_TABLE_MAX_SIZE + 1U - used);

        if (length < 0)
        {
            result = -RT_EIO;
            break;
        }
        if (!length)
        {
            break;
        }
        used += (rt_size_t)length;
        if (used > AIC_FW_PATCH_TABLE_MAX_SIZE)
        {
            result = -RT_EFULL;
            break;
        }
    }
    close(descriptor);
    if (result != RT_EOK || !used)
    {
        if (result == RT_EOK)
        {
            result = -RT_EIO;
        }
        rt_free(buffer);
        return result;
    }
    *data = buffer;
    *data_length = used;
    return RT_EOK;
}

static rt_err_t aic_firmware_read_patch_table(
    const struct aic8800_context *context,
    enum aic_firmware_family family, const char *name,
    rt_uint8_t **table, rt_size_t *table_length)
{
    char path[AIC_FW_PATH_MAX];
    rt_err_t result;

    result = aic_firmware_read_blob(
        context, family, name, path, table, table_length);
    if (result != RT_EOK)
    {
        LOG_E("patch table read failed: %s (%d)", name, result);
        return result;
    }
    if (*table_length < 16U ||
        rt_memcmp(*table, AIC_PATCH_TAG, sizeof(AIC_PATCH_TAG)) != 0)
    {
        LOG_E("invalid tagged patch table: %s", path);
        rt_free(*table);
        *table = RT_NULL;
        *table_length = 0;
        return -RT_EIO;
    }
    LOG_I("loaded patch table %s (%u bytes)", path,
          (unsigned int)*table_length);
    return RT_EOK;
}

static rt_err_t aic_firmware_patch_info(const rt_uint8_t *table,
                                        rt_size_t table_length,
                                        struct aic_patch_info *info)
{
    const rt_uint8_t *data;
    rt_uint32_t count;
    rt_uint32_t index;

    if (!table || table_length < 40U || !info)
    {
        return -RT_EINVAL;
    }
    if (aic_fw_get_le32(table + 32) != AIC_PATCH_INFO_TYPE)
    {
        return -RT_EIO;
    }
    count = aic_fw_get_le32(table + 36);
    if (count < 4U || count > (table_length - 40U) / 8U)
    {
        return -RT_EIO;
    }
    data = table + 40;
    rt_memset(info, 0, sizeof(*info));
    info->adid_address = aic_fw_get_le32(data + 4);
    info->patch_address = aic_fw_get_le32(data + 12);
    if (count >= 5U)
    {
        info->ext_count = aic_fw_get_le32(data + 36);
        if (info->ext_count > AIC_FW_EXT_PATCH_MAX ||
            5U + info->ext_count > count)
        {
            return -RT_EIO;
        }
        for (index = 0; index < info->ext_count; index++)
        {
            info->ext_id[index] =
                aic_fw_get_le32(data + (5U + index) * 8U);
            info->ext_address[index] =
                aic_fw_get_le32(data + (5U + index) * 8U + 4U);
        }
    }
    return RT_EOK;
}

static void aic_firmware_configure_btmode(
    enum aic_firmware_family family, rt_uint8_t *data, rt_uint32_t count)
{
    if (count < 9U)
    {
        return;
    }
    aic_fw_put_le32(data + 4, 1U);
    aic_fw_put_le32(data + 12, 0xffffffffUL);
    aic_fw_put_le32(data + 20, 0U);
    aic_fw_put_le32(data + 28,
                    family == AIC_FW_FAMILY_8800 ? 2U : 5U);
    aic_fw_put_le32(data + 36, 1U);
    aic_fw_put_le32(data + 44, 1500000U);
    aic_fw_put_le32(data + 52, 1U);
    aic_fw_put_le32(data + 60, 0U);
    aic_fw_put_le32(data + 68,
                    family == AIC_FW_FAMILY_8800 ? 0x00006020U :
                                                   0x00006f2fU);
}

static rt_err_t aic_firmware_apply_patch_table(
    struct aic8800_context *context, enum aic_firmware_family family,
    rt_uint8_t *table, rt_size_t table_length)
{
    rt_size_t offset = 16U;

    while (offset < table_length)
    {
        rt_uint8_t *data;
        rt_uint32_t type;
        rt_uint32_t count;
        rt_uint32_t index;

        if (table_length - offset < 24U)
        {
            return -RT_EIO;
        }
        type = aic_fw_get_le32(table + offset + 16U);
        count = aic_fw_get_le32(table + offset + 20U);
        if (count > (table_length - offset - 24U) / 8U)
        {
            return -RT_EIO;
        }
        data = table + offset + 24U;
        if (type == AIC_PATCH_VERSION_TYPE)
        {
            LOG_D("patch table version block: %.*s", 16, table + offset);
            offset += 24U + (rt_size_t)count * 8U;
            continue;
        }
        if (type == AIC_PATCH_BTMODE_TYPE)
        {
            aic_firmware_configure_btmode(family, data, count);
        }

        LOG_D("applying patch block type=%u pairs=%u", type, count);
        for (index = 0; index < count; index++)
        {
            rt_err_t result = aic_firmware_mem_write(
                context, aic_fw_get_le32(data + index * 8U),
                aic_fw_get_le32(data + index * 8U + 4U));

            if (result != RT_EOK)
            {
                return result;
            }
        }
        if (type == AIC_PATCH_POWER_ON_TYPE)
        {
            cpu_ticks_delay_us(500);
        }
        offset += 24U + (rt_size_t)count * 8U;
    }
    return offset == table_length ? RT_EOK : -RT_EIO;
}

static rt_err_t aic_firmware_upload_external_patches(
    struct aic8800_context *context, enum aic_firmware_family family,
    const char *prefix, const struct aic_patch_info *info)
{
    rt_uint32_t index;

    for (index = 0; index < info->ext_count; index++)
    {
        char name[64];

        rt_snprintf(name, sizeof(name), "%s%u.bin", prefix,
                    info->ext_id[index]);
        if (aic_firmware_upload(context, family, name,
                                info->ext_address[index]) != RT_EOK)
        {
            return -RT_ERROR;
        }
    }
    return RT_EOK;
}

static const struct aic_firmware_pair g_aic8800_system_u02[] = {
    {0x40040000, 0x00001ac8},
    {0x40040084, 0x00011580},
    {0x40040080, 0x00000001},
    {0x40100058, 0x00000000},
};

static const struct aic_firmware_pair g_aic8800_system_u04[] = {
    {0x40040000, 0x0000042c}, {0x40040004, 0x0000dd44},
    {0x40040008, 0x00000448}, {0x4004000c, 0x0000044c},
    {0x0019b800, 0xb9f0f19b}, {0x0019b804, 0x0019b81d},
    {0x0019b808, 0xbf00fa79}, {0x0019b80c, 0xf007bf00},
    {0x0019b810, 0x4605b672}, {0x0019b814, 0x21e0f04f},
    {0x0019b818, 0xbe0bf664}, {0x0019b81c, 0xf665b510},
    {0x0019b820, 0x4804fc9d}, {0x0019b824, 0xfa9ef66c},
    {0x0019b828, 0xfca8f665}, {0x0019b82c, 0x4010e8bd},
    {0x0019b830, 0xbac6f66c}, {0x0019b834, 0x0019a0c4},
    {0x40040084, 0x0019b800}, {0x40040080, 0x0000000f},
    {0x40100058, 0x00000000},
};

static const struct aic_firmware_pair g_aic8800_system_common[] = {
    {0x40500014, 0x00000101},
    {0x40500018, 0x0000010d},
    {0x40500004, 0x00000010},
    {0x50000000, 0x03220204},
    {0x50019150, 0x00000002},
    {0x50017008, 0x00000000},
};

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
static const struct aic_firmware_pair g_aic8800_system_sdio[] = {
    {0x40500014, 0x00000101},
    {0x40500018, 0x00000109},
    {0x40500004, 0x00000010},
    {0x40040000, 0x00001ac8},
    {0x40040084, 0x00011580},
    {0x40040080, 0x00000001},
    {0x40100058, 0x00000000},
    {0x50000000, 0x03220204},
    {0x50019150, 0x00000002},
    {0x50017008, 0x00000000},
};
#endif

static const struct aic_firmware_pair g_aic8800_config_patch[] = {
    {0x0044, 0x00000002}, {0x0048, 0x00000060},
    {0x004c, 0x00040050}, {0x0050, 0x00000000},
    {0x0054, 0x00190000}, {0x0058, 0x00190140},
    {0x005c, 0x00000ee0}, {0x0060, 0x00191020},
    {0x0064, 0x0002efe0}, {0x0068, 0x00000008},
    {0x006c, 0x00000040}, {0x0070, 0x00000040},
    {0x0074, 0x00000020}, {0x0078, 0x00000000},
    {0x007c, 0x00000020}, {0x0080, 0x001d0000},
    {0x0084, 0x0000fc00}, {0x0088, 0x001dfc00},
    {0x00a8, 0x8d080103}, {0x00d0, 0x00010103},
    {0x00d4, 0x0404087c}, {0x00d8, 0x001c0000},
    {0x00dc, 0x00008000}, {0x00e0, 0x04020a08},
    {0x00e4, 0x00000001}, {0x00fc, 0x00000302},
    {0x0100, 0x0000000f},
};

static rt_err_t aic_firmware_system_config_8800(
    struct aic8800_context *context, rt_uint8_t *chip_id)
{
    rt_uint32_t value;
    rt_uint8_t sub_id;
    rt_err_t result;

    result = aic_firmware_mem_read(context, 0x40500000, &value);
    if (result != RT_EOK)
    {
        return result;
    }
    *chip_id = (rt_uint8_t)(value >> 16);
    result = aic_firmware_mem_read(context, 0x00000004, &value);
    if (result != RT_EOK)
    {
        return result;
    }
    sub_id = (rt_uint8_t)(value >> 4);
    LOG_I("AIC8800 chip revision=0x%02x sub-revision=0x%02x",
          *chip_id, sub_id);
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
        return aic_firmware_write_pairs(
            context, g_aic8800_system_sdio,
            sizeof(g_aic8800_system_sdio) /
            sizeof(g_aic8800_system_sdio[0]));
    }
#endif
    if (*chip_id == AIC_CHIP_REV_U02)
    {
        result = aic_firmware_write_pairs(
            context, g_aic8800_system_u02,
            sizeof(g_aic8800_system_u02) /
            sizeof(g_aic8800_system_u02[0]));
        if (result != RT_EOK)
        {
            return result;
        }
    }
    if (*chip_id == AIC_CHIP_REV_U03 && sub_id == AIC_CHIP_SUB_REV_U04)
    {
        result = aic_firmware_write_pairs(
            context, g_aic8800_system_u04,
            sizeof(g_aic8800_system_u04) /
            sizeof(g_aic8800_system_u04[0]));
        if (result != RT_EOK)
        {
            return result;
        }
    }
    return aic_firmware_write_pairs(
        context, g_aic8800_system_common,
        sizeof(g_aic8800_system_common) /
        sizeof(g_aic8800_system_common[0]));
}

#ifdef AIC8800_WIFI_TRANSPORT_USB
static rt_err_t aic_firmware_config_patch_8800_usb(
    struct aic8800_context *context)
{
    rt_uint32_t config_base;
    rt_uint32_t pair_count = 0;
    rt_uint32_t index;
    rt_err_t result;

    result = aic_firmware_mem_read(context, 0x00110180, &config_base);
    if (result != RT_EOK)
    {
        return result;
    }
    for (index = 0; index < sizeof(g_aic8800_config_patch) /
                                  sizeof(g_aic8800_config_patch[0]); index++)
    {
        if (g_aic8800_config_patch[index].address == 0x00fc &&
            !context->message_out)
        {
            continue;
        }
        pair_count++;
    }
    result = aic_firmware_mem_write(context, 0x001e5318, 0x001e6000);
    if (result == RT_EOK)
    {
        /* The original firmware ABI expects a word count here. */
        result = aic_firmware_mem_write(context, 0x001e531c,
                                        pair_count * 2U);
    }
    for (index = 0, pair_count = 0;
         result == RT_EOK &&
         index < sizeof(g_aic8800_config_patch) /
                 sizeof(g_aic8800_config_patch[0]); index++)
    {
        rt_uint32_t offset = g_aic8800_config_patch[index].address;
        rt_uint32_t value = g_aic8800_config_patch[index].value;

        if (offset == 0x00fc && !context->message_out)
        {
            continue;
        }
        if (!context->message_out)
        {
            if (offset == 0x004c)
            {
                value = 0x00000052;
            }
            else if (offset == 0x00d4)
            {
                value = 0x0000087c;
            }
        }
        result = aic_firmware_mem_write(
            context, 0x001e6000 + pair_count * 8U,
            config_base + offset);
        if (result == RT_EOK)
        {
            result = aic_firmware_mem_write(
                context, 0x001e6004 + pair_count * 8U, value);
        }
        pair_count++;
    }
    return result;
}
#endif

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
static rt_err_t aic_firmware_config_patch_8800_sdio(
    struct aic8800_context *context)
{
    rt_uint32_t config_base;
    rt_err_t result;

    result = aic_firmware_mem_read(context, 0x00120180, &config_base);
    if (result != RT_EOK || !config_base)
    {
        return result == RT_EOK ? -RT_EIO : result;
    }
    result = aic_firmware_mem_write(context, 0x001e5318, 0x001e6000);
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_write(context, 0x001e531c, 0U);
    }
    return result;
}
#endif

static rt_err_t aic_firmware_config_patch_8800(
    struct aic8800_context *context)
{
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
        return aic_firmware_config_patch_8800_sdio(context);
#else
        return -RT_ENOSYS;
#endif
    }
#ifdef AIC8800_WIFI_TRANSPORT_USB
    return aic_firmware_config_patch_8800_usb(context);
#else
    return -RT_ENOSYS;
#endif
}

static rt_err_t aic_firmware_download_8800(
    struct aic8800_context *context)
{
    rt_uint8_t *patch_table = RT_NULL;
    rt_size_t patch_table_length = 0;
    rt_uint8_t chip_id;
    const char *adid_name;
    const char *patch_name;
    const char *table_name;
    rt_uint32_t firmware_base;
    rt_err_t result;

    result = aic_firmware_system_config_8800(context, &chip_id);
    if (result != RT_EOK)
    {
        return result;
    }
    adid_name = chip_id == AIC_CHIP_REV_U03 ?
                "fw_adid_u03.bin" : "fw_adid.bin";
    patch_name = chip_id == AIC_CHIP_REV_U03 ?
                 "fw_patch_u03.bin" : "fw_patch.bin";
    table_name = chip_id == AIC_CHIP_REV_U03 ?
                 "fw_patch_table_u03.bin" : "fw_patch_table.bin";
    firmware_base = context->transport == AIC8800_TRANSPORT_SDIO ?
                    0x00120000 : 0x00110000;

    result = aic_firmware_upload(context, AIC_FW_FAMILY_8800,
                                 "fmacfw.bin", firmware_base);
    if (result == RT_EOK)
    {
        result = aic_firmware_upload(context, AIC_FW_FAMILY_8800,
                                     adid_name, 0x00161928);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_upload(context, AIC_FW_FAMILY_8800,
                                     patch_name, 0x00100000);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_read_patch_table(
            context, AIC_FW_FAMILY_8800, table_name, &patch_table,
            &patch_table_length);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_apply_patch_table(
            context, AIC_FW_FAMILY_8800, patch_table, patch_table_length);
    }
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (result == RT_EOK &&
        context->transport == AIC8800_TRANSPORT_SDIO)
    {
        result = aic_firmware_upload(context, AIC_FW_FAMILY_8800,
                                     "fmacfw_patch.bin", 0x00190000);
    }
#endif
    if (result == RT_EOK)
    {
        result = aic_firmware_config_patch_8800(context);
    }
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (result == RT_EOK &&
        context->transport == AIC8800_TRANSPORT_SDIO)
    {
        result = aic_firmware_mem_mask_write(
            context, 0x40506024, 0x000000ff, 0x000000df);
    }
#endif
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_mask_write(
            context, 0x40344058, 0x00800000, 0x00000000);
    }
    if (patch_table)
    {
        rt_free(patch_table);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_start_app(
            context, firmware_base, AIC_START_APP_AUTO);
    }
    return result;
}

static rt_err_t aic_firmware_config_patch_d80(
    struct aic8800_context *context, rt_uint32_t firmware_base,
    rt_uint32_t setting_offset, rt_bool_t always_use_buffer)
{
    struct aic_firmware_pair patch_pairs[] = {
#ifdef AIC8800_WIFI_5GHZ
        {0x00b4, 0xf3010001},
#else
        {0x00b4, 0xf3010000},
#endif
        {0x0170, 0},
        {0x0188, 0x00000001},
    };
    rt_uint32_t setting_address = firmware_base + setting_offset;
    rt_uint32_t patch_struct_address;
    rt_uint32_t config_base;
    rt_uint32_t patch_buffer =
        context->transport == AIC8800_TRANSPORT_SDIO ?
        0x0016f800 : 0x001d7000;
    rt_uint32_t version;
    rt_uint32_t index;
    rt_uint32_t patch_count = always_use_buffer ? 0U :
                              sizeof(patch_pairs) / sizeof(patch_pairs[0]);
    rt_err_t result;

    /* K230 uses the conservative USB RX aggregation setting from the vendor's
     * DWC2-oriented platform configuration.  A count of ten causes protocol
     * errors on some D80 H modules while the firmware is delivering data and
     * message records concurrently. */
    patch_pairs[1].value = context->transport == AIC8800_TRANSPORT_SDIO ?
                           0x0100000a : 0x00010001;

    result = aic_firmware_mem_read(context, setting_address, &config_base);
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_read(context, setting_address + 8U,
                                       &patch_struct_address);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_read(context, firmware_base + 0x1cU,
                                       &version);
    }
    if (result != RT_EOK)
    {
        return result;
    }
    if (always_use_buffer || version > 0x06090100U)
    {
        result = aic_firmware_mem_read(context, setting_address + 12U,
                                       &patch_buffer);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    LOG_I("firmware version=0x%08x config=0x%08x patch=0x%08x",
          version, config_base, patch_buffer);
    result = aic_firmware_mem_write(
        context, patch_struct_address + AIC_PATCH_STRUCT_MAGIC_OFFSET,
        AIC_PATCH_MAGIC);
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_write(
            context, patch_struct_address + AIC_PATCH_STRUCT_MAGIC_2_OFFSET,
            AIC_PATCH_MAGIC_2);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_write(
            context, patch_struct_address + AIC_PATCH_STRUCT_PAIR_START_OFFSET,
            patch_buffer);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_write(
            context, patch_struct_address + AIC_PATCH_STRUCT_PAIR_COUNT_OFFSET,
            patch_count);
    }
    for (index = 0; result == RT_EOK && index < patch_count; index++)
    {
        result = aic_firmware_mem_write(
            context, patch_buffer + index * 8U,
            config_base + patch_pairs[index].address);
        if (result == RT_EOK)
        {
            result = aic_firmware_mem_write(
                context, patch_buffer + index * 8U + 4U,
                patch_pairs[index].value);
        }
    }
    for (index = 0; result == RT_EOK && index < 4U; index++)
    {
        result = aic_firmware_mem_write(
            context, patch_struct_address +
                     AIC_PATCH_STRUCT_BLOCK_SIZE_OFFSET + index * 4U, 0U);
    }
    if (result == RT_EOK)
    {
        rt_uint32_t magic;
        rt_uint32_t magic_2;
        rt_uint32_t pair_start;
        rt_uint32_t pair_count_value;

        result = aic_firmware_mem_read(context, patch_struct_address, &magic);
        if (result == RT_EOK)
        {
            result = aic_firmware_mem_read(
                context, patch_struct_address +
                         AIC_PATCH_STRUCT_MAGIC_2_OFFSET, &magic_2);
        }
        if (result == RT_EOK)
        {
            result = aic_firmware_mem_read(
                context, patch_struct_address +
                         AIC_PATCH_STRUCT_PAIR_START_OFFSET, &pair_start);
        }
        if (result == RT_EOK)
        {
            result = aic_firmware_mem_read(
                context, patch_struct_address +
                         AIC_PATCH_STRUCT_PAIR_COUNT_OFFSET,
                &pair_count_value);
        }
        if (result == RT_EOK &&
            (magic != AIC_PATCH_MAGIC || magic_2 != AIC_PATCH_MAGIC_2 ||
             pair_start != patch_buffer || pair_count_value != patch_count))
        {
            LOG_E("D80 patch config verify failed: magic=%08x/%08x pairs=%08x/%u",
                  magic, magic_2, pair_start, pair_count_value);
            result = -RT_EIO;
        }
        else if (result == RT_EOK)
        {
            for (index = 0; result == RT_EOK && index < patch_count; index++)
            {
                rt_uint32_t pair_address;
                rt_uint32_t pair_value;
                rt_uint32_t expected_address =
                    config_base + patch_pairs[index].address;

                result = aic_firmware_mem_read(
                    context, patch_buffer + index * 8U, &pair_address);
                if (result == RT_EOK)
                {
                    result = aic_firmware_mem_read(
                        context, patch_buffer + index * 8U + 4U,
                        &pair_value);
                }
                if (result == RT_EOK &&
                    (pair_address != expected_address ||
                     pair_value != patch_pairs[index].value))
                {
                    LOG_E("D80 patch pair %u verify failed: %08x/%08x=%08x/%08x",
                          index, pair_address, expected_address, pair_value,
                          patch_pairs[index].value);
                    result = -RT_EIO;
                }
            }
            if (result == RT_EOK)
            {
                LOG_D("D80 patch config verified: structure=0x%08x buffer=0x%08x pairs=%u",
                      patch_struct_address, patch_buffer, patch_count);
            }
        }
    }
    return result;
}

static rt_err_t aic_firmware_system_config_d80(
    struct aic8800_context *context, rt_uint8_t *chip_id)
{
    rt_uint32_t value;
    rt_err_t result = aic_firmware_mem_read(context, 0x40500000, &value);

    if (result == RT_EOK)
    {
        *chip_id = (rt_uint8_t)(value >> 16);
        LOG_I("AIC8800D80 chip revision=0x%02x high=%u mcu=%u",
              *chip_id & (rt_uint8_t)~AIC_CHIP_ID_H_MASK,
              (*chip_id & AIC_CHIP_ID_H_MASK) == AIC_CHIP_ID_H_MASK,
              ((value >> 25) & 1U) == 0U);
    }
    return result;
}

static rt_err_t aic_firmware_download_d80(
    struct aic8800_context *context)
{
    struct aic_patch_info info;
    rt_uint8_t *patch_table = RT_NULL;
    rt_size_t patch_table_length = 0;
    rt_uint8_t chip_id;
    rt_uint8_t revision;
    rt_bool_t high_id;
    const char *table_name;
    const char *adid_name;
    const char *patch_name;
    const char *fmac_name;
    rt_uint32_t firmware_base;
    rt_err_t result;

    result = aic_firmware_system_config_d80(context, &chip_id);
    if (result != RT_EOK)
    {
        return result;
    }
    revision = chip_id & ~AIC_CHIP_ID_H_MASK;
    high_id = (chip_id & AIC_CHIP_ID_H_MASK) == AIC_CHIP_ID_H_MASK;
    if (revision == AIC_CHIP_REV_U01 && !high_id)
    {
        table_name = "fw_patch_table_8800d80.bin";
        adid_name = "fw_adid_8800d80.bin";
        patch_name = "fw_patch_8800d80.bin";
        fmac_name = "fmacfw_8800d80.bin";
        firmware_base = 0x00100000;
    }
    else
    {
        table_name = "fw_patch_table_8800d80_u02.bin";
        adid_name = "fw_adid_8800d80_u02.bin";
        patch_name = "fw_patch_8800d80_u02.bin";
        fmac_name = high_id ?
                    "fmacfw_8800d80_h_u02.bin" :
                    "fmacfw_8800d80_u02.bin";
        firmware_base = 0x00120000;
    }
    result = aic_firmware_read_patch_table(
        context, AIC_FW_FAMILY_D80, table_name, &patch_table,
        &patch_table_length);
    if (result == RT_EOK)
    {
        result = aic_firmware_patch_info(
            patch_table, patch_table_length, &info);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_upload(context, AIC_FW_FAMILY_D80,
                                     adid_name, info.adid_address);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_upload(context, AIC_FW_FAMILY_D80,
                                     patch_name, info.patch_address);
    }
    if (result == RT_EOK && info.ext_count)
    {
        result = aic_firmware_upload_external_patches(
            context, AIC_FW_FAMILY_D80, "fw_patch_8800d80_u02_ext",
            &info);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_apply_patch_table(
            context, AIC_FW_FAMILY_D80, patch_table, patch_table_length);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_upload(context, AIC_FW_FAMILY_D80,
                                     fmac_name, firmware_base);
    }
    if (result == RT_EOK &&
        (revision != AIC_CHIP_REV_U01 || high_id))
    {
        result = aic_firmware_config_patch_d80(
            context, firmware_base, 0x198U, RT_FALSE);
    }
    if (patch_table)
    {
        rt_free(patch_table);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_start_app(
            context, firmware_base, AIC_START_APP_AUTO);
    }
    return result;
}

static rt_err_t aic_firmware_system_config_d80x2(
    struct aic8800_context *context, rt_uint8_t *chip_id,
    rt_bool_t *mcu_variant)
{
    static const struct aic_firmware_pair system_config[] = {
        {0x40500010, 0x00000006},
        {0x40500024, 0x0000001f},
    };
    rt_uint32_t value;
    rt_err_t result;

    result = aic_firmware_mem_read(context, 0x40500000, &value);
    if (result != RT_EOK)
    {
        return result;
    }
    *chip_id = (rt_uint8_t)(value >> 16);
    result = aic_firmware_mem_read(context, 0x40500004, &value);
    if (result != RT_EOK)
    {
        return result;
    }
    *mcu_variant = ((value >> 17) & 1U) == 0U;
    LOG_I("AIC8800D80X2 chip revision=0x%02x mcu=%u",
          *chip_id, *mcu_variant);
    return aic_firmware_write_pairs(
        context, system_config,
        sizeof(system_config) / sizeof(system_config[0]));
}

static rt_err_t aic_firmware_power_on_d80x2(
    struct aic8800_context *context)
{
    rt_uint32_t status;
    rt_uint32_t value;
    rt_uint32_t retry;
    rt_err_t result;

    result = aic_firmware_mem_read(context, 0x40506030, &status);
    if (result != RT_EOK || (status & (1U << 2)))
    {
        return result;
    }
    result = aic_firmware_mem_read(context, 0x40506004, &value);
    if (result == RT_EOK)
    {
        value = (value | (1U << 17)) & ~(1U << 18);
        result = aic_firmware_mem_write(context, 0x40506004, value);
    }
    if (result != RT_EOK)
    {
        return result;
    }
    rt_thread_mdelay(1);
    for (retry = 0; retry <= 20U; retry++)
    {
        result = aic_firmware_mem_read(context, 0x40506030, &status);
        if (result != RT_EOK || (status & (1U << 2)))
        {
            return result;
        }
        rt_thread_mdelay(1);
    }
    return -RT_ETIMEOUT;
}

static rt_err_t aic_firmware_download_d80x2(
    struct aic8800_context *context)
{
    struct aic_patch_info info;
    rt_uint8_t *patch_table = RT_NULL;
    rt_size_t patch_table_length = 0;
    rt_uint8_t chip_id;
    rt_bool_t mcu_variant;
    const char *table_name;
    const char *adid_name;
    const char *patch_name;
    const char *ext_prefix;
    rt_err_t result;

    result = aic_firmware_system_config_d80x2(
        context, &chip_id, &mcu_variant);
    if (result != RT_EOK)
    {
        return result;
    }
    if (chip_id < AIC_CHIP_REV_U05)
    {
        table_name = "fw_patch_table_8800d80x2_u03.bin";
        adid_name = "fw_adid_8800d80x2_u03.bin";
        patch_name = "fw_patch_8800d80x2_u03.bin";
        ext_prefix = "fw_patch_8800d80x2_u03_ext";
    }
    else
    {
        table_name = "fw_patch_table_8800d80x2_u05.bin";
        adid_name = "fw_adid_8800d80x2_u05.bin";
        patch_name = "fw_patch_8800d80x2_u05.bin";
        ext_prefix = "fw_patch_8800d80x2_u05_ext";
    }
    result = aic_firmware_read_patch_table(
        context, AIC_FW_FAMILY_D80X2, table_name, &patch_table,
        &patch_table_length);
    if (result == RT_EOK)
    {
        result = aic_firmware_patch_info(
            patch_table, patch_table_length, &info);
    }
    if (result == RT_EOK && mcu_variant)
    {
        result = aic_firmware_power_on_d80x2(context);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_upload(context, AIC_FW_FAMILY_D80X2,
                                     adid_name, info.adid_address);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_upload(context, AIC_FW_FAMILY_D80X2,
                                     patch_name, info.patch_address);
    }
    if (result == RT_EOK && info.ext_count)
    {
        result = aic_firmware_upload_external_patches(
            context, AIC_FW_FAMILY_D80X2, ext_prefix, &info);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_apply_patch_table(
            context, AIC_FW_FAMILY_D80X2, patch_table,
            patch_table_length);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_upload(
            context, AIC_FW_FAMILY_D80X2,
            "fmacfw_8800d80x2.bin", 0x00120000);
    }
    if (result == RT_EOK && chip_id < AIC_CHIP_REV_U05)
    {
        result = aic_firmware_config_patch_d80(
            context, 0x00120000, 0x1a8U, RT_TRUE);
    }
    if (patch_table)
    {
        rt_free(patch_table);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_start_app(
            context, 0x00120000, AIC_START_APP_AUTO);
    }
    return result;
}

static rt_err_t aic_firmware_download(struct aic8800_context *context,
                                      enum aic_firmware_family family)
{
    switch (family)
    {
    case AIC_FW_FAMILY_8800:
        return aic_firmware_download_8800(context);
    case AIC_FW_FAMILY_D80:
        return aic_firmware_download_d80(context);
    case AIC_FW_FAMILY_D80X2:
        return aic_firmware_download_d80x2(context);
    default:
        return -RT_ENOSYS;
    }
}

static rt_bool_t aic_firmware_required_files_available(
    const struct aic8800_context *context,
    enum aic_firmware_family family)
{
    rt_bool_t base;

    switch (family)
    {
    case AIC_FW_FAMILY_8800:
        return aic_firmware_file_available(
                   context, family, "fmacfw.bin") &&
               aic_firmware_file_available(
                   context, family, "fw_adid.bin") &&
               aic_firmware_file_available(
                   context, family, "fw_patch.bin") &&
               aic_firmware_file_available(
                   context, family, "fw_patch_table.bin");
    case AIC_FW_FAMILY_D80:
        base = aic_firmware_file_available(
                   context, family, "fmacfw_8800d80.bin") &&
               aic_firmware_file_available(
                   context, family, "fw_adid_8800d80.bin") &&
               aic_firmware_file_available(
                   context, family, "fw_patch_8800d80.bin") &&
               aic_firmware_file_available(
                   context, family, "fw_patch_table_8800d80.bin");
        return base ||
               (aic_firmware_file_available(
                    context, family, "fw_adid_8800d80_u02.bin") &&
                aic_firmware_file_available(
                    context, family, "fw_patch_8800d80_u02.bin") &&
                aic_firmware_file_available(
                    context, family, "fw_patch_table_8800d80_u02.bin") &&
                (aic_firmware_file_available(
                     context, family, "fmacfw_8800d80_u02.bin") ||
                 aic_firmware_file_available(
                     context, family, "fmacfw_8800d80_h_u02.bin")));
    case AIC_FW_FAMILY_D80X2:
        base = aic_firmware_file_available(
                   context, family, "fmacfw_8800d80x2.bin") &&
               aic_firmware_file_available(
                   context, family, "fw_adid_8800d80x2_u03.bin") &&
               aic_firmware_file_available(
                   context, family, "fw_patch_8800d80x2_u03.bin") &&
               aic_firmware_file_available(
                   context, family, "fw_patch_table_8800d80x2_u03.bin");
        return base ||
               (aic_firmware_file_available(
                    context, family, "fmacfw_8800d80x2.bin") &&
                aic_firmware_file_available(
                    context, family, "fw_adid_8800d80x2_u05.bin") &&
                aic_firmware_file_available(
                    context, family, "fw_patch_8800d80x2_u05.bin") &&
                aic_firmware_file_available(
                    context, family, "fw_patch_table_8800d80x2_u05.bin"));
    default:
        return RT_FALSE;
    }
}

static rt_err_t aic_firmware_probe_dc_revision(
    struct aic8800_context *context)
{
    rt_uint32_t value;
    rt_err_t result;

    context->chip_revision_valid = RT_FALSE;
    result = aic_firmware_mem_read(context, 0x40500000U, &value);
    if (result != RT_EOK)
    {
        LOG_E("DC/DW chip-ID read failed: %d", result);
        return result;
    }
    context->chip_id = (rt_uint8_t)(value >> 16);
    context->chip_mcu_id = ((value >> 25) & 1U) ? 0U : 1U;
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
        context->product_id = ((value >> 26) & 1U) ?
                              AIC8800_USB_PID_AIC8800DC :
                              AIC8800_USB_PID_AIC8800DW;
    }

    result = aic_firmware_mem_read(context, 0x00000020U, &value);
    if (result != RT_EOK)
    {
        LOG_E("DC/DW chip-sub-ID read failed: %d", result);
        return result;
    }
    context->chip_sub_id = (rt_uint8_t)value;
    context->chip_revision_valid = RT_TRUE;
    LOG_I("DC/DW silicon: chip=0x%02x sub=%u mcu=%u%s",
          context->chip_id, context->chip_sub_id, context->chip_mcu_id,
          (context->chip_id & AIC_CHIP_ID_H_MASK) == AIC_CHIP_ID_H_MASK ?
          " H" : "");
    return RT_EOK;
}

static rt_bool_t aic_firmware_dc_is_h(
    const struct aic8800_context *context)
{
    return context &&
           (context->chip_id & AIC_CHIP_ID_H_MASK) == AIC_CHIP_ID_H_MASK;
}

static const char *aic_firmware_dc_patch_name(
    const struct aic8800_context *context)
{
    if (aic_firmware_dc_is_h(context))
    {
        return "fmacfw_patch_8800dc_h_u02.bin";
    }
    return "fmacfw_patch_8800dc_u02.bin";
}

static const char *aic_firmware_dc_patch_table_name(
    const struct aic8800_context *context)
{
    if (aic_firmware_dc_is_h(context))
    {
        return "fmacfw_patch_tbl_8800dc_h_u02.bin";
    }
    return "fmacfw_patch_tbl_8800dc_u02.bin";
}

static rt_err_t aic_firmware_dc_write_words(
    struct aic8800_context *context, rt_uint32_t address,
    const rt_uint32_t *words, rt_size_t count)
{
    const rt_uint8_t *data = (const rt_uint8_t *)words;
    rt_size_t total = count * sizeof(*words);
    rt_size_t offset = 0;

    while (offset < total)
    {
        rt_size_t length = total - offset;
        rt_err_t result;

        if (length > AIC_DC_TABLE_CHUNK_SIZE)
        {
            length = AIC_DC_TABLE_CHUNK_SIZE;
        }
        result = aic_firmware_mem_block_write(
            context, address + (rt_uint32_t)offset, data + offset, length);
        if (result != RT_EOK)
        {
            return result;
        }
        offset += length;
    }
    return RT_EOK;
}

static rt_err_t aic_firmware_dc_usb_config(
    struct aic8800_context *context)
{
    static const struct aic_firmware_pair usb_config[] =
    {
        {0x40200028U, 0x0021047eU},
        {0x40200024U, 0x0000011dU},
    };
    rt_uint32_t value;
    rt_err_t result;

    result = aic_firmware_mem_read(context, 0x40200024U, &value);
    if (result != RT_EOK)
    {
        LOG_E("DC USB configuration read failed: %d", result);
        return result;
    }
    if ((value & 0xffffU) != 0x0119U)
    {
        return RT_EOK;
    }
    return aic_firmware_write_pairs(
        context, usb_config, sizeof(usb_config) / sizeof(usb_config[0]));
}

/* Parts that report a second MCU need the ROM told that a RAM patch is about
 * to be installed before it is uploaded.  The vendor Windows driver does this
 * at the end of its usb_config(); the Linux reference predates the check and
 * omits it entirely. */
static rt_err_t aic_firmware_dc_mcu_patch_enable(
    struct aic8800_context *context)
{
    rt_uint32_t value;
    rt_err_t result;

    if (context->chip_mcu_id != 1U)
    {
        return RT_EOK;
    }
    result = aic_firmware_mem_read(
        context, AIC_DC_MCU_PATCH_ENABLE, &value);
    if (result != RT_EOK)
    {
        LOG_E("DC MCU patch-enable read failed: %d", result);
        return result;
    }
    if (value & 1U)
    {
        LOG_D("DC MCU patch already enabled [%08x] = %08x",
              AIC_DC_MCU_PATCH_ENABLE, value);
        return RT_EOK;
    }
    return aic_firmware_mem_write(
        context, AIC_DC_MCU_PATCH_ENABLE, value | 1U);
}

static rt_err_t aic_firmware_dc_system_config(
    struct aic8800_context *context)
{
    static const struct aic_firmware_pair system_config[] =
    {
        {0x40500010U, 0x00000004U},
        {0x40500010U, 0x00000006U},
    };
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    static const struct aic_firmware_pair sdio_config_u02[] =
    {
        {0x40030000U, 0x00036da4U},
        {0x0011e800U, 0xe7fe4070U},
        {0x40030084U, 0x0011e800U},
        {0x40030080U, 0x00000001U},
        {0x4010001cU, 0x00000000U},
    };
#endif
    static const struct aic_firmware_mask_pair masked_config[] =
    {
        {0x7000216cU, 0x0000000cU, 0x00000004U},
        {0x700021bcU, 0x0000000cU, 0x00000004U},
        {0x70002118U, 0x000000f0U, 0x000000a0U},
        {0x70002104U, 0x0000007fU, 0x00000042U},
        {0x7000210cU, 0x0000007fU, 0x00000042U},
        {0x70002170U, 0x0000000fU, 0x00000001U},
        {0x70002190U, 0x0000003fU, 0x00000018U},
        {0x700021ccU, 0x000000f0U, 0x00000000U},
        {0x700010a0U, 0x00000800U, 0x00000800U},
        {0x70001034U, 0x1c100000U, 0x08000000U},
        {0x70001038U, 0x00000100U, 0x00000100U},
        {0x70001084U, 0x00006000U, 0x00000000U},
        {0x70001094U, 0x0000000cU, 0x00000000U},
        {0x700021d0U, 0x00000060U, 0x00000060U},
        {0x70001000U, 0x00500001U, 0x00100001U},
        {0x70001028U, 0x0000003cU, 0x00000004U},
    };
    static const struct aic_firmware_mask_pair masked_config_h[] =
    {
        {0x7000216cU, 0x0000003cU, 0x00000028U},
        {0x70002138U, 0x000000ffU, 0x000000ffU},
        {0x7000213cU, 0x000000ffU, 0x000000ffU},
        {0x70002144U, 0x000000ffU, 0x000000ffU},
        {0x700021bcU, 0x0000000cU, 0x00000004U},
        {0x70002118U, 0x000000f0U, 0x000000a0U},
        {0x70002104U, 0x0000007fU, 0x00000042U},
        {0x7000210cU, 0x0000007fU, 0x00000042U},
        {0x70002170U, 0x0000000fU, 0x00000001U},
        {0x70002190U, 0x0000003fU, 0x00000018U},
        {0x700021ccU, 0x000000f0U, 0x00000000U},
        {0x700010a0U, 0x00000800U, 0x00000800U},
        {0x70001038U, 0x00000100U, 0x00000100U},
        {0x70001084U, 0x00006000U, 0x00000000U},
        {0x70001094U, 0x0000000cU, 0x00000000U},
        {0x700021d0U, 0x00000060U, 0x00000060U},
        {0x70001000U, 0x00500001U, 0x00100001U},
        {0x70001028U, 0x0000003cU, 0x00000004U},
    };
    const struct aic_firmware_mask_pair *selected_config;
    rt_size_t selected_count;
    rt_uint32_t value;
    rt_size_t index;
    rt_err_t result;

    result = aic_firmware_mem_read(context, 0x40500148U, &value);
    if (result != RT_EOK)
    {
        LOG_E("DC crystal-source read failed: %d", result);
        return result;
    }
    if (value & 1U)
    {
        result = aic_firmware_mem_read(context, 0x40505010U, &value);
        if (result != RT_EOK)
        {
            LOG_E("DC BBPLL read failed: %d", result);
            return result;
        }
        if ((value >> 29) != 3U)
        {
            value = (value | 0x60000000U) & ~0x80000000U;
            result = aic_firmware_mem_write(
                context, 0x40505010U, value);
            if (result != RT_EOK)
            {
                LOG_E("DC BBPLL configuration failed: %d", result);
                return result;
            }
        }
    }

    result = aic_firmware_write_pairs(
        context, system_config,
        sizeof(system_config) / sizeof(system_config[0]));
    if (result != RT_EOK)
    {
        LOG_E("DC system configuration failed: %d", result);
        return result;
    }
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (context->transport == AIC8800_TRANSPORT_SDIO &&
        context->chip_mcu_id == 0U)
    {
        result = aic_firmware_write_pairs(
            context, sdio_config_u02,
            sizeof(sdio_config_u02) / sizeof(sdio_config_u02[0]));
        if (result != RT_EOK)
        {
            LOG_E("DC SDIO system configuration failed: %d", result);
            return result;
        }
    }
#endif
    if (aic_firmware_dc_is_h(context))
    {
        selected_config = masked_config_h;
        selected_count = sizeof(masked_config_h) / sizeof(masked_config_h[0]);
    }
    else
    {
        selected_config = masked_config;
        selected_count = sizeof(masked_config) / sizeof(masked_config[0]);
    }
    for (index = 0; index < selected_count; index++)
    {
        rt_uint32_t mask = selected_config[index].mask;
        rt_uint32_t config_value = selected_config[index].value;

        if (selected_config[index].address == 0x70001000U &&
            context->chip_mcu_id == 0U)
        {
            mask |= 0x00008100U;
            config_value |= 0x00008100U;
        }
        result = aic_firmware_mem_mask_write(
            context, selected_config[index].address, mask, config_value);
        if (result != RT_EOK)
        {
            LOG_E("DC system mask write 0x%08x failed: %d",
                  selected_config[index].address, result);
            return result;
        }
    }
    return RT_EOK;
}

static rt_err_t aic_firmware_dc_apply_patch_table(
    struct aic8800_context *context)
{
    const char *table_name;
    rt_uint8_t *table = RT_NULL;
    rt_size_t table_length = 0;
    rt_size_t offset;
    rt_uint32_t describe_address;
    char path[AIC_FW_PATH_MAX];
    rt_err_t result;

    table_name = aic_firmware_dc_patch_table_name(context);
    result = aic_firmware_read_blob(
        context, AIC_FW_FAMILY_DC, table_name, path, &table, &table_length);
    if (result != RT_EOK)
    {
        LOG_E("DC patch table read failed: %d", result);
        return result;
    }
    if (table_length < AIC_DC_PATCH_DESCRIBE_SIZE ||
        (table_length - AIC_DC_PATCH_DESCRIBE_SIZE) % 8U != 0U)
    {
        LOG_E("invalid DC patch table length: %u",
              (unsigned int)table_length);
        result = -RT_EIO;
        goto exit;
    }
    describe_address = aic_fw_get_le32(table);
    if (!describe_address)
    {
        LOG_E("DC patch table has no description address");
        result = -RT_EIO;
        goto exit;
    }
    LOG_I("loaded DC patch table %s (%u bytes)", path,
          (unsigned int)table_length);
    result = aic_firmware_mem_block_write(
        context, describe_address, table, AIC_DC_PATCH_DESCRIBE_SIZE);
    if (result != RT_EOK)
    {
        LOG_E("DC patch-table description upload failed: %d", result);
        goto exit;
    }
    for (offset = AIC_DC_PATCH_DESCRIBE_SIZE;
         offset < table_length; offset += 8U)
    {
        result = aic_firmware_mem_write(
            context, aic_fw_get_le32(table + offset),
            aic_fw_get_le32(table + offset + 4U));
        if (result != RT_EOK)
        {
            LOG_E("DC patch-table write at 0x%x failed: %d",
                  (unsigned int)offset, result);
            goto exit;
        }
    }
    if (aic_firmware_dc_is_h(context))
    {
        rt_uint32_t value;

        result = aic_firmware_mem_read(
            context, AIC_DC_PATCH_ADDRESS, &value);
        if (result != RT_EOK)
        {
            LOG_E("DC H patch-variable magic read failed: %d", result);
            goto exit;
        }
        if (value == AIC_DC_PATCH_VAR_MAGIC)
        {
            result = aic_firmware_mem_read(
                context, AIC_DC_PATCH_ADDRESS + 4U, &value);
            if (result != RT_EOK)
            {
                LOG_E("DC H patch-variable flags read failed: %d", result);
                goto exit;
            }
            if (!(value & AIC_DC_USER_TX_POWER_FLAG))
            {
                result = aic_firmware_mem_write(
                    context, AIC_DC_PATCH_ADDRESS + 4U,
                    value | AIC_DC_USER_TX_POWER_FLAG);
                if (result != RT_EOK)
                {
                    LOG_E("DC H patch-variable flags write failed: %d",
                          result);
                    goto exit;
                }
            }
        }
        else
        {
            LOG_I("DC H patch has no variable-group header");
        }
    }
    LOG_I("applied DC%s U02 patch table (%u bytes)",
          aic_firmware_dc_is_h(context) ? " H" : "",
          (unsigned int)table_length);

exit:
    rt_free(table);
    return result;
}

static rt_err_t aic_firmware_dc_patch_config(
    struct aic8800_context *context)
{
    static const struct aic_firmware_pair usb_wifi_settings[] =
    {
        {0x00000090U, 0x0013fc00U},
#if defined(AIC8800_WIFI_USB_TX_AGGREGATION) && \
    AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES > 1U
        {0x00000100U, 0x03021714U},
        {0x00000120U, 0x140a0100U},
#endif
        {0x000000b0U, 0xad180100U},
    };
#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    static const struct aic_firmware_pair sdio_wifi_settings[] =
    {
        {0x00000124U, 0x01001e01U},
    };
#endif
    const struct aic_firmware_pair *wifi_settings = usb_wifi_settings;
    rt_size_t wifi_settings_count =
        sizeof(usb_wifi_settings) / sizeof(usb_wifi_settings[0]);
    rt_uint32_t wifi_address;
    rt_uint32_t ldpc_address;
    rt_uint32_t agc_address;
    rt_uint32_t txgain_address;
    const rt_uint32_t *txgain_config;
    rt_size_t txgain_count;
    rt_size_t index;
    rt_err_t result;

    result = aic_firmware_mem_read(
        context, AIC_DC_CONFIG_BASE, &wifi_address);
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_read(
            context, AIC_DC_CONFIG_BASE + 8U, &ldpc_address);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_read(
            context, AIC_DC_CONFIG_BASE + 12U, &agc_address);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_read(
            context, AIC_DC_CONFIG_BASE + 16U, &txgain_address);
    }
    if (result != RT_EOK)
    {
        LOG_E("DC runtime configuration pointers unavailable: %d", result);
        return result;
    }
    if (!wifi_address || !ldpc_address || !agc_address || !txgain_address)
    {
        LOG_E("DC runtime returned an empty configuration pointer");
        return -RT_EIO;
    }
    LOG_D("DC config: wifi=%08x ldpc=%08x agc=%08x txgain=%08x",
          wifi_address, ldpc_address, agc_address, txgain_address);

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
        wifi_settings = sdio_wifi_settings;
        wifi_settings_count =
            sizeof(sdio_wifi_settings) / sizeof(sdio_wifi_settings[0]);
    }
#endif
    for (index = 0; index < wifi_settings_count; index++)
    {
        result = aic_firmware_mem_write(
            context, wifi_address + wifi_settings[index].address,
            wifi_settings[index].value);
        if (result != RT_EOK)
        {
            LOG_E("DC Wi-Fi setting 0x%x failed: %d",
                  wifi_settings[index].address, result);
            return result;
        }
    }
    if (aic_firmware_dc_is_h(context))
    {
        txgain_config = aic_dc_txgain_config_h;
        txgain_count = sizeof(aic_dc_txgain_config_h) /
                       sizeof(aic_dc_txgain_config_h[0]);
    }
    else
    {
        txgain_config = aic_dc_txgain_config;
        txgain_count = sizeof(aic_dc_txgain_config) /
                       sizeof(aic_dc_txgain_config[0]);
    }
    result = aic_firmware_dc_write_words(
        context, ldpc_address, aic_dc_ldpc_config,
        sizeof(aic_dc_ldpc_config) / sizeof(aic_dc_ldpc_config[0]));
    if (result == RT_EOK)
    {
        result = aic_firmware_dc_write_words(
            context, agc_address, aic_dc_agc_config,
            sizeof(aic_dc_agc_config) / sizeof(aic_dc_agc_config[0]));
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_dc_write_words(
            context, txgain_address, txgain_config, txgain_count);
    }
    if (result != RT_EOK)
    {
        LOG_E("DC runtime table upload failed: %d", result);
        return result;
    }
    return aic_firmware_dc_apply_patch_table(context);
}

static rt_err_t aic_firmware_dc_misc_ram_address(
    struct aic8800_context *context, rt_uint32_t *address)
{
    rt_uint32_t misc_address;
    rt_err_t result;

    result = aic_firmware_mem_read(
        context, AIC_DC_CONFIG_BASE + 20U, &misc_address);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!misc_address)
    {
        return -RT_EIO;
    }
    *address = misc_address;
    return RT_EOK;
}

static rt_err_t aic_firmware_dc_dpd_valid(
    struct aic8800_context *context, rt_bool_t *valid)
{
    rt_uint32_t misc_address;
    rt_uint32_t bit_mask[4];
    rt_size_t index;
    rt_err_t result;

    if (!valid)
    {
        return -RT_EINVAL;
    }
    *valid = RT_FALSE;
    result = aic_firmware_dc_misc_ram_address(context, &misc_address);
    if (result != RT_EOK)
    {
        return result;
    }
    for (index = 0; index < sizeof(bit_mask) / sizeof(bit_mask[0]); index++)
    {
        result = aic_firmware_mem_read(
            context, misc_address + (rt_uint32_t)index * 4U,
            &bit_mask[index]);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    *valid = bit_mask[0] == 0U &&
             (bit_mask[1] & 0xfff00000U) == 0x80000000U &&
             bit_mask[2] == 0U &&
             (bit_mask[3] & 0xffffff00U) == 0U;
    return RT_EOK;
}

static rt_err_t aic_firmware_dc_calibrate(
    struct aic8800_context *context)
{
    const char *calibration_name;
    rt_bool_t valid;
    rt_err_t result;

    result = aic_firmware_dc_dpd_valid(context, &valid);
    if (result != RT_EOK)
    {
        LOG_E("DC DPD state read failed: %d", result);
        return result;
    }
    if (valid)
    {
        LOG_I("DC DPD result already valid");
        return RT_EOK;
    }
    calibration_name = aic_firmware_dc_is_h(context) ?
                       "fmacfw_calib_8800dc_h_u02.bin" :
                       "fmacfw_calib_8800dc_u02.bin";
    result = aic_firmware_upload(
        context, AIC_FW_FAMILY_DC, calibration_name, AIC_DC_CALIB_ADDRESS);
    if (result != RT_EOK)
    {
        return result;
    }
    LOG_I("running DC%s U02 DPD calibration",
          aic_firmware_dc_is_h(context) ? " H" : "");
    result = aic_firmware_start_app_confirmed(
        context, AIC_DC_CALIB_ENTRY, AIC_START_APP_FUNCTION);
    if (result != RT_EOK)
    {
        LOG_E("DC DPD calibration helper failed: %d", result);
        return result;
    }
    result = aic_firmware_dc_dpd_valid(context, &valid);
    if (result != RT_EOK || !valid)
    {
        LOG_E("DC DPD calibration produced no valid result: %d", result);
        return result == RT_EOK ? -RT_EIO : result;
    }
    return RT_EOK;
}

static rt_err_t aic_firmware_dc_prepare_calibration(
    struct aic8800_context *context)
{
    rt_err_t result = aic_firmware_dc_calibrate(context);

    if (result != RT_EOK)
    {
        LOG_E("DC DPD calibration failed: %d", result);
    }
    return result;
}

static rt_err_t aic_firmware_prepare_dc_runtime(
    struct aic8800_context *context)
{
    const char *patch_name;
    rt_bool_t high_id;
    rt_uint32_t entry_word;
    rt_err_t result;

    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
        result = aic_firmware_probe_dc_revision(context);
    }
    else
    {
        result = aic_firmware_dc_usb_config(context);
        if (result == RT_EOK)
        {
            result = aic_firmware_probe_dc_revision(context);
        }
        if (result == RT_EOK)
        {
            result = aic_firmware_dc_mcu_patch_enable(context);
        }
    }
    if (result != RT_EOK)
    {
        return result;
    }
    high_id = aic_firmware_dc_is_h(context);
    if (!((context->chip_sub_id == 1U && !high_id) ||
          (context->chip_sub_id == 2U && high_id)))
    {
        LOG_E("unsupported DC/DW ROM patch revision: chip=0x%02x sub=%u",
              context->chip_id, context->chip_sub_id);
        return -RT_ENOSYS;
    }
    patch_name = aic_firmware_dc_patch_name(context);
    result = aic_firmware_dc_system_config(context);
    if (result == RT_EOK)
    {
        result = aic_firmware_upload(
            context, AIC_FW_FAMILY_DC, patch_name, AIC_DC_PATCH_ADDRESS);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_dc_patch_config(context);
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_dc_prepare_calibration(context);
    }
    if (result == RT_EOK && context->chip_sub_id < 2U)
    {
        result = aic_firmware_mem_mask_write(
            context, 0x4010300cU, 0x00000001U, 0x00000001U);
        if (result != RT_EOK)
        {
            LOG_E("DC watchdog configuration failed: %d", result);
        }
    }
    if (result == RT_EOK)
    {
        result = aic_firmware_mem_read(
            context, AIC_DC_ROM_ENTRY, &entry_word);
        if (result != RT_EOK)
        {
            LOG_E("DC ROM entry read failed: %d", result);
        }
    }
    if (result == RT_EOK)
    {
        LOG_D("starting DC runtime at 0x%08x [%08x]",
              AIC_DC_ROM_ENTRY, entry_word);
        result = aic_firmware_start_app_confirmed(
            context, AIC_DC_ROM_ENTRY, AIC_START_APP_DUMMY);
        if (result != RT_EOK)
        {
            LOG_E("DC runtime start failed: %d", result);
        }
    }
    if (result != RT_EOK)
    {
        LOG_E("DC U02 runtime preparation failed: %d", result);
        return result;
    }
    LOG_I("DC%s U02 ROM patch ready", high_id ? " H" : "");
    return RT_EOK;
}

rt_err_t aic8800_firmware_probe(struct aic8800_context *context,
                                rt_bool_t *attach_runtime)
{
    enum aic_firmware_family family;
    rt_bool_t runtime;
    rt_err_t result;

    if (!context || !attach_runtime)
    {
        return -RT_EINVAL;
    }
    *attach_runtime = RT_TRUE;
    family = aic_firmware_family_from_product(
        context->vendor_id, context->product_id);
    runtime = aic_firmware_product_is_runtime(context->product_id);

    if (family == AIC_FW_FAMILY_DC)
    {
        if ((context->transport != AIC8800_TRANSPORT_USB &&
             context->transport != AIC8800_TRANSPORT_SDIO) || !runtime)
        {
            return -RT_ENOSYS;
        }
        result = aic_firmware_prepare_dc_runtime(context);
        if (result != RT_EOK)
        {
            return result;
        }
        g_firmware_state = AIC_FW_STATE_READY;
        context->firmware_runtime_ready = RT_TRUE;
        return RT_EOK;
    }

#ifdef AIC8800_WIFI_TRANSPORT_SDIO
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
        (void)runtime;
        if (family != AIC_FW_FAMILY_8800 && family != AIC_FW_FAMILY_D80)
        {
            return -RT_ENOSYS;
        }
        LOG_I("SDIO device %04x:%04x detected; loading firmware",
              context->sdio_vendor_id, context->sdio_product_id);
        context->firmware_transition = RT_TRUE;
        result = aic_firmware_download(context, family);
        if (result != RT_EOK)
        {
            context->firmware_transition = RT_FALSE;
            return result;
        }
        context->product_id = family == AIC_FW_FAMILY_8800 ?
                              AIC8800_USB_PID_AIC8801 :
                              AIC8800_USB_PID_AIC8800D81;
        context->firmware_transition = RT_FALSE;
        context->firmware_runtime_ready = RT_TRUE;
        *attach_runtime = RT_TRUE;
        return RT_EOK;
    }
#endif

    if (family == AIC_FW_FAMILY_NONE)
    {
        if (runtime)
        {
            g_firmware_state = AIC_FW_STATE_READY;
            context->firmware_runtime_ready = RT_TRUE;
            return RT_EOK;
        }
        return -RT_ENOSYS;
    }

    if (runtime)
    {
        if (g_firmware_state == AIC_FW_STATE_WAIT_RUNTIME &&
            g_transition_family == family)
        {
            g_firmware_state = AIC_FW_STATE_READY;
            context->firmware_runtime_ready = RT_TRUE;
            LOG_I("firmware runtime re-enumerated as %04x:%04x",
                  context->vendor_id, context->product_id);
            return RT_EOK;
        }
        if (g_firmware_state == AIC_FW_STATE_WAIT_BOOT)
        {
            LOG_W("device returned as a runtime PID before boot download");
            g_firmware_state = AIC_FW_STATE_READY;
            context->firmware_runtime_ready = RT_TRUE;
            return RT_EOK;
        }
#ifdef AIC8800_WIFI_FORCE_FIRMWARE_DOWNLOAD
        if (g_firmware_state == AIC_FW_STATE_COLD)
        {
            if (!aic_firmware_required_files_available(context, family))
            {
                LOG_W("firmware files unavailable under %s; using running firmware",
                      AIC8800_WIFI_FIRMWARE_PATH);
                g_firmware_state = AIC_FW_STATE_READY;
                context->firmware_runtime_ready = RT_TRUE;
                return RT_EOK;
            }
            LOG_I("rebooting runtime device into the AIC USB loader");
            g_firmware_state = AIC_FW_STATE_WAIT_BOOT;
            g_transition_family = family;
            context->firmware_transition = RT_TRUE;
            result = aic_firmware_start_app(
                context, AIC_REBOOT_DELAY_MS, AIC_START_APP_REBOOT);
            if (result != RT_EOK)
            {
                g_firmware_state = AIC_FW_STATE_COLD;
                g_transition_family = AIC_FW_FAMILY_NONE;
                return result;
            }
            *attach_runtime = RT_FALSE;
            return RT_EOK;
        }
#endif
        g_firmware_state = AIC_FW_STATE_READY;
        context->firmware_runtime_ready = RT_TRUE;
        return RT_EOK;
    }

    LOG_I("boot device %04x:%04x detected; loading firmware",
          context->vendor_id, context->product_id);
    g_transition_family = family;
    g_firmware_state = AIC_FW_STATE_WAIT_RUNTIME;
    context->firmware_transition = RT_TRUE;
    result = aic_firmware_download(context, family);
    if (result != RT_EOK)
    {
        g_firmware_state = AIC_FW_STATE_COLD;
        g_transition_family = AIC_FW_FAMILY_NONE;
        return result;
    }
    *attach_runtime = RT_FALSE;
    return RT_EOK;
}

void aic8800_firmware_disconnected(struct aic8800_context *context)
{
    if (!context)
    {
        return;
    }
    context->firmware_runtime_ready = RT_FALSE;
    if (context->transport == AIC8800_TRANSPORT_SDIO)
    {
        context->firmware_transition = RT_FALSE;
        return;
    }
    if (context->firmware_transition)
    {
        return;
    }
    if (g_firmware_state == AIC_FW_STATE_READY)
    {
        g_firmware_state = AIC_FW_STATE_COLD;
        g_transition_family = AIC_FW_FAMILY_NONE;
    }
}
