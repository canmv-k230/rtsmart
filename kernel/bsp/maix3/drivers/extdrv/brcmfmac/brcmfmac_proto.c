// SPDX-License-Identifier: ISC
/* BCDC protocol support, derived from Linux 6.6 brcmfmac/bcdc.c. */
#include "brcmfmac.h"

#define DBG_TAG "brcmf.proto"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define BCDC_DCMD_ERROR             0x00000001U
#define BCDC_DCMD_SET               0x00000002U
#define BCDC_DCMD_IF_SHIFT          12U
#define BCDC_DCMD_IF_MASK           0x0000f000U
#define BCDC_DCMD_ID_SHIFT          16U
#define BCDC_DCMD_ID_MASK           0xffff0000U
#define BCDC_HEADER_VERSION         2U
#define BCDC_HEADER_VERSION_SHIFT   4U
#define BCDC_INTERFACE_MASK         0x0fU
#define BRCMF_TX_IOCTL_MAX_MSG_SIZE 1518U
#define BRCMF_EVENT_ETHERTYPE       0x886cU
#define BRCMF_EVENT_OUI0            0x00U
#define BRCMF_EVENT_OUI1            0x10U
#define BRCMF_EVENT_OUI2            0x18U
#define BRCMF_EVENT_SUBTYPE         1U

static rt_size_t brcmf_min_size(rt_size_t left, rt_size_t right)
{
    return left < right ? left : right;
}

static void brcmf_proto_control_response(struct brcmf_context *context,
                                         const rt_uint8_t *data,
                                         rt_size_t length)
{
    const struct brcmf_bcdc_dcmd *header;
    rt_uint32_t flags;
    rt_uint32_t request;
    rt_uint32_t payload_length;
    rt_int32_t firmware_status;

    if (length < sizeof(*header))
    {
        return;
    }
    header = (const struct brcmf_bcdc_dcmd *)data;
    flags = brcmf_get_le32(&header->flags);
    request = (flags & BCDC_DCMD_ID_MASK) >> BCDC_DCMD_ID_SHIFT;
    if (request != (context->command_request & 0xffffU))
    {
        return;
    }
    payload_length = brcmf_get_le32(&header->length);
    payload_length = brcmf_min_size(payload_length,
                                    length - sizeof(*header));
    payload_length = brcmf_min_size(payload_length,
                                    context->command_length);
    if (payload_length)
    {
        rt_memcpy(context->command_buffer, header->data, payload_length);
    }
    context->command_length = payload_length;
    firmware_status = (rt_int32_t)brcmf_get_le32(&header->status);
    context->command_status =
        ((flags & BCDC_DCMD_ERROR) || firmware_status) ?
        -RT_ERROR : RT_EOK;
    if (context->command_status != RT_EOK)
    {
        LOG_E("firmware rejected command %u request %u: status %d flags 0x%08x",
              brcmf_get_le32(&header->command), request, firmware_status,
              flags);
    }
    rt_completion_done(&context->command_completion);
}

static rt_err_t brcmf_proto_data_response(struct brcmf_context *context,
                                          rt_uint8_t channel,
                                          const rt_uint8_t *data,
                                          rt_size_t length)
{
    const struct brcmf_bcdc_header *header;
    const rt_uint8_t *payload;
    rt_size_t offset;
    rt_size_t payload_length;
    rt_uint8_t interface_index;
    enum rt_wlan_offload_iftype iftype;
    rt_err_t result;

    if (length < sizeof(*header))
    {
        return -RT_EIO;
    }
    header = (const struct brcmf_bcdc_header *)data;
    if ((header->flags >> BCDC_HEADER_VERSION_SHIFT) != BCDC_HEADER_VERSION)
    {
        return -RT_EIO;
    }
    offset = sizeof(*header) + ((rt_size_t)header->data_offset << 2);
    if (offset > length)
    {
        return -RT_EIO;
    }
    payload = data + offset;
    payload_length = length - offset;
    interface_index = header->flags2 & BCDC_INTERFACE_MASK;
    if (channel == BRCMF_BUS_CHANNEL_EVENT)
    {
        const struct brcmf_event_packet *packet =
            (const struct brcmf_event_packet *)payload;

        if (payload_length < sizeof(*packet) ||
            brcmf_get_be16(&packet->ethernet.type) != BRCMF_EVENT_ETHERTYPE ||
            packet->vendor.oui[0] != BRCMF_EVENT_OUI0 ||
            packet->vendor.oui[1] != BRCMF_EVENT_OUI1 ||
            packet->vendor.oui[2] != BRCMF_EVENT_OUI2 ||
            brcmf_get_be16(&packet->vendor.user_subtype) !=
                BRCMF_EVENT_SUBTYPE)
        {
            return -RT_EIO;
        }
        brcmf_wifi_handle_event(context, packet, payload_length);
        return RT_EOK;
    }
    if (channel != BRCMF_BUS_CHANNEL_DATA)
    {
        return -RT_EINVAL;
    }
    if (!payload_length)
    {
        return RT_EOK;
    }
    if (interface_index == context->sta_interface)
    {
        iftype = RT_WLAN_OFFLOAD_IFTYPE_STATION;
        context->data_rx_sta_count++;
    }
    else if (interface_index == context->ap_interface)
    {
        iftype = RT_WLAN_OFFLOAD_IFTYPE_AP;
        context->data_rx_ap_count++;
    }
    else
    {
        context->data_rx_unknown_count++;
        if (context->data_rx_unknown_count <= 4U ||
            !(context->data_rx_unknown_count &
              (context->data_rx_unknown_count - 1U)))
        {
            LOG_W("drop data for unknown firmware interface %u "
                  "(sta=%u ap=%u count=%u)", interface_index,
                  context->sta_interface, context->ap_interface,
                  (unsigned int)context->data_rx_unknown_count);
        }
        return RT_EOK;
    }
    result = rt_wlan_offload_rx(&context->radio, iftype, payload,
                                payload_length);
    if (result != RT_EOK)
    {
        context->data_rx_drop_count++;
    }
    /* A valid SDPCM/BCDC frame has been consumed even when lwIP drops it. */
    return RT_EOK;
}

rt_err_t brcmf_proto_receive(struct rt_wlan_offload_bus *bus,
                             const void *data, rt_size_t length,
                             void *parameter)
{
    struct brcmf_context *context = parameter;
    const struct brcmf_bus_record *record = data;

    (void)bus;
    if (!context || !record || length < sizeof(*record) ||
        record->length != length - sizeof(*record))
    {
        return -RT_EINVAL;
    }
    switch (record->channel)
    {
    case BRCMF_BUS_CHANNEL_CONTROL:
        brcmf_proto_control_response(context, record->payload,
                                     record->length);
        return RT_EOK;
    case BRCMF_BUS_CHANNEL_EVENT:
    case BRCMF_BUS_CHANNEL_DATA:
        return brcmf_proto_data_response(context, record->channel,
                                         record->payload, record->length);
    default:
        return -RT_EEMPTY;
    }
}

rt_err_t brcmf_proto_command(struct brcmf_context *context,
                             rt_uint8_t interface_index, rt_uint32_t command,
                             void *data, rt_size_t length, rt_bool_t set)
{
    struct brcmf_bus_record *record;
    struct brcmf_bcdc_dcmd *header;
    rt_uint8_t *request_buffer;
    rt_size_t message_length;
    rt_size_t request_length;
    rt_size_t request_data_length;
    rt_uint32_t flags;
    rt_err_t result;

    if (!context || interface_index >= BRCMF_MAX_IFS ||
        length > BRCMF_MAX_COMMAND - sizeof(*record) - sizeof(*header) ||
        (length && !data))
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&context->command_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    message_length = sizeof(*header) + length;
    message_length = brcmf_min_size(message_length,
                                    BRCMF_TX_IOCTL_MAX_MSG_SIZE);
    request_length = sizeof(*record) + message_length;
    request_buffer = rt_malloc(request_length);
    if (!request_buffer)
    {
        rt_mutex_release(&context->command_mutex);
        return -RT_ENOMEM;
    }
    record = (struct brcmf_bus_record *)request_buffer;
    header = (struct brcmf_bcdc_dcmd *)record->payload;
    record->channel = BRCMF_BUS_CHANNEL_CONTROL;
    record->interface_index = interface_index;
    record->length = message_length;
    context->command_request++;
    if (!(context->command_request & 0xffffU))
    {
        context->command_request++;
    }
    flags = ((context->command_request << BCDC_DCMD_ID_SHIFT) &
             BCDC_DCMD_ID_MASK) |
            (((rt_uint32_t)interface_index << BCDC_DCMD_IF_SHIFT) &
             BCDC_DCMD_IF_MASK) |
            (set ? BCDC_DCMD_SET : 0U);
    brcmf_put_le32(&header->command, command);
    brcmf_put_le32(&header->length, length);
    brcmf_put_le32(&header->flags, flags);
    brcmf_put_le32(&header->status, 0);
    request_data_length = message_length - sizeof(*header);
    if (request_data_length)
    {
        rt_memcpy(header->data, data, request_data_length);
    }
    context->command_length = length;
    context->command_status = -RT_ETIMEOUT;
    rt_completion_init(&context->command_completion);
    result = rt_wlan_offload_bus_transmit_priority(
        &context->bus, RT_WLAN_OFFLOAD_BUS_PRIORITY_CONTROL,
        record, request_length);
    rt_free(request_buffer);
    if (result == RT_EOK)
    {
        result = rt_completion_wait(
            &context->command_completion,
            rt_tick_from_millisecond(BRCMFMAC_CONTROL_TIMEOUT_MS));
    }
    if (result == RT_EOK)
    {
        result = context->command_status;
    }
    if (result == RT_EOK && !set && context->command_length)
    {
        rt_memcpy(data, context->command_buffer,
                  brcmf_min_size(length, context->command_length));
    }
    rt_mutex_release(&context->command_mutex);
    return result;
}

rt_err_t brcmf_proto_iovar(struct brcmf_context *context,
                           rt_uint8_t interface_index, const char *name,
                           void *data, rt_size_t length, rt_bool_t set)
{
    rt_size_t name_length;
    rt_size_t buffer_length;
    rt_uint8_t *buffer;
    rt_err_t result;

    if (!name || (!data && length))
    {
        return -RT_EINVAL;
    }
    name_length = rt_strlen(name) + 1U;
    buffer_length = name_length + length;
    if (buffer_length > BRCMF_MAX_COMMAND)
    {
        return -RT_EFULL;
    }
    buffer = rt_calloc(1, buffer_length);
    if (!buffer)
    {
        return -RT_ENOMEM;
    }
    rt_memcpy(buffer, name, name_length);
    if (length)
    {
        rt_memcpy(buffer + name_length, data, length);
    }
    result = brcmf_proto_command(
        context, interface_index,
        set ? BRCMF_C_SET_VAR : BRCMF_C_GET_VAR,
        buffer, buffer_length, set);
    if (result == RT_EOK && !set && length)
    {
        rt_memcpy(data, buffer, length);
    }
    rt_free(buffer);
    return result;
}

rt_err_t brcmf_proto_iovar_int(struct brcmf_context *context,
                               rt_uint8_t interface_index, const char *name,
                               rt_uint32_t *value, rt_bool_t set)
{
    rt_uint8_t data[4];
    rt_err_t result;

    if (!value)
    {
        return -RT_EINVAL;
    }
    brcmf_put_le32(data, *value);
    result = brcmf_proto_iovar(context, interface_index, name, data,
                               sizeof(data), set);
    if (result == RT_EOK && !set)
    {
        *value = brcmf_get_le32(data);
    }
    return result;
}

rt_err_t brcmf_proto_bsscfg_iovar(struct brcmf_context *context,
                                  rt_uint8_t interface_index,
                                  rt_uint32_t bsscfg_index, const char *name,
                                  void *data, rt_size_t length, rt_bool_t set)
{
    static const char prefix[] = "bsscfg:";
    rt_size_t name_length;
    rt_size_t buffer_length;
    rt_uint8_t *buffer;
    rt_err_t result;

    if (!name || (!data && length))
    {
        return -RT_EINVAL;
    }
    if (bsscfg_index == 0U)
    {
        return brcmf_proto_iovar(context, interface_index, name, data,
                                 length, set);
    }
    name_length = sizeof(prefix) - 1U + rt_strlen(name) + 1U;
    buffer_length = name_length + 4U + length;
    if (buffer_length > BRCMF_MAX_COMMAND)
    {
        return -RT_EFULL;
    }
    buffer = rt_calloc(1, buffer_length);
    if (!buffer)
    {
        return -RT_ENOMEM;
    }
    rt_memcpy(buffer, prefix, sizeof(prefix) - 1U);
    rt_memcpy(buffer + sizeof(prefix) - 1U, name,
              rt_strlen(name) + 1U);
    brcmf_put_le32(buffer + name_length, bsscfg_index);
    if (length)
    {
        rt_memcpy(buffer + name_length + 4U, data, length);
    }
    result = brcmf_proto_command(
        context, interface_index,
        set ? BRCMF_C_SET_VAR : BRCMF_C_GET_VAR,
        buffer, buffer_length, set);
    if (result == RT_EOK && !set && length)
    {
        rt_memcpy(data, buffer, length);
    }
    rt_free(buffer);
    return result;
}

static void brcmf_proto_configure_rx_glom(struct brcmf_context *context)
{
    struct brcmf_core *sdio_core = brcmf_chip_get_core(
        &context->chip, BRCMF_CORE_SDIO_DEV);
    rt_uint32_t value;
    rt_err_t result;

    context->tx_glom = RT_FALSE;
    if (!sdio_core)
    {
        return;
    }
    if (sdio_core->revision < 12U)
    {
        value = 0;
        result = brcmf_proto_iovar_int(
            context, 0, "bus:txglom", &value, RT_TRUE);
        if (result != RT_EOK)
        {
            LOG_W("could not disable RX glomming: %d", result);
        }
    }
    else
    {
        value = 4U;
        result = brcmf_proto_iovar_int(
            context, 0, "bus:txglomalign", &value, RT_TRUE);
        if (result == RT_EOK)
        {
            LOG_I("SDIO RX glom alignment configured");
        }
        else
        {
            LOG_W("could not configure RX glom alignment: %d", result);
        }
    }

    /* Firmware names directions from its own point of view. bus:rxglom
     * enables the Linux-compatible host-to-device aggregate header. */
    value = 1U;
    result = brcmf_proto_iovar_int(
        context, 0, "bus:rxglom", &value, RT_TRUE);
    if (result == RT_EOK)
    {
        context->tx_glom = RT_TRUE;
        LOG_I("SDIO TX glom enabled (max %u frames)",
              (unsigned int)BRCMFMAC_TX_GLOM_FRAMES);
    }
    else
    {
        LOG_W("firmware does not support SDIO TX glom: %d", result);
    }
}

rt_err_t brcmf_proto_start(struct brcmf_context *context)
{
    rt_uint8_t event_mask[BRCMF_EVENTING_MASK_LEN];
    rt_err_t result;

    rt_memset(event_mask, 0, sizeof(event_mask));
#define BRCMF_ENABLE_EVENT(_event) \
    (event_mask[(_event) / 8U] |= (rt_uint8_t)(1U << ((_event) % 8U)))
    BRCMF_ENABLE_EVENT(BRCMF_E_SET_SSID);
    BRCMF_ENABLE_EVENT(BRCMF_E_AUTH);
    BRCMF_ENABLE_EVENT(BRCMF_E_DEAUTH);
    BRCMF_ENABLE_EVENT(BRCMF_E_DEAUTH_IND);
    BRCMF_ENABLE_EVENT(BRCMF_E_ASSOC);
    BRCMF_ENABLE_EVENT(BRCMF_E_ASSOC_IND);
    BRCMF_ENABLE_EVENT(BRCMF_E_REASSOC);
    BRCMF_ENABLE_EVENT(BRCMF_E_REASSOC_IND);
    BRCMF_ENABLE_EVENT(BRCMF_E_DISASSOC_IND);
    BRCMF_ENABLE_EVENT(BRCMF_E_LINK);
    BRCMF_ENABLE_EVENT(BRCMF_E_PRUNE);
    BRCMF_ENABLE_EVENT(BRCMF_E_PSK_SUP);
    BRCMF_ENABLE_EVENT(BRCMF_E_IF);
    BRCMF_ENABLE_EVENT(BRCMF_E_AP_STARTED);
    BRCMF_ENABLE_EVENT(BRCMF_E_ESCAN_RESULT);
#undef BRCMF_ENABLE_EVENT
    result = brcmf_proto_iovar(context, 0, "event_msgs", event_mask,
                               sizeof(event_mask), RT_TRUE);
    if (result == RT_EOK)
    {
        brcmf_proto_configure_rx_glom(context);
    }
    if (result == RT_EOK)
    {
        result = brcmf_proto_iovar(context, 0, "cur_etheraddr",
                                   context->mac, sizeof(context->mac),
                                   RT_FALSE);
    }
    return result;
}

void brcmf_proto_stop(struct brcmf_context *context)
{
    context->scan_active = RT_FALSE;
    context->command_status = -RT_EIO;
    rt_completion_done(&context->command_completion);
}
