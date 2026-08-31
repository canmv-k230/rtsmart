/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <wlan_offload_control.h>
#include <wlan_offload_control_protocol.h>

#include <dfs_file.h>
#include <dfs_posix.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>

#ifdef RT_WLAN_MANAGE_ENABLE
#include <wlan_mgnt.h>
#endif

#define WLAN_OFFLOAD_CONTROL_QUEUE_DEPTH 32
#define WLAN_OFFLOAD_CONTROL_NAME_PREFIX "wlanctl"
#define WLAN_OFFLOAD_CONTROL_INDEX_MAX   100
#define WLAN_OFFLOAD_CONTROL_INDEX_NONE  0xff

_Static_assert(RTWO_CTRL_VERSION == 2 && RTWO_CTRL_CMD_GET_INFO == 1 &&
               RTWO_CTRL_CMD_EAPOL_TX == 12 &&
               RTWO_CTRL_CMD_EXTERNAL_AUTH_RESPONSE == 13 &&
               RTWO_CTRL_CMD_GET_NAMES == 14 &&
               RTWO_CTRL_EVENT_INFO == 0x8000 &&
               RTWO_CTRL_EVENT_FIRMWARE_ERROR == 0x800d &&
               RTWO_CTRL_EVENT_EXTERNAL_AUTH_REQUIRED == 0x800e &&
               RTWO_CTRL_EVENT_NAMES == 0x800f &&
               RTWO_CTRL_EVENT_TKIP_MIC_FAILURE == 0x8010,
               "wlan_offload wire identifiers changed");
_Static_assert(sizeof(struct rtwo_ctrl_header) == 20,
               "wlan_offload wire header ABI changed");
_Static_assert(sizeof(struct rtwo_ctrl_info) == 56,
               "wlan_offload info ABI changed");
_Static_assert(sizeof(struct rtwo_ctrl_names) == 76,
               "wlan_offload names ABI changed");
_Static_assert(sizeof(struct rtwo_ctrl_scan_request) == 1940,
               "wlan_offload scan ABI changed");
_Static_assert(sizeof(struct rtwo_ctrl_auth_request) == 1082,
               "wlan_offload auth ABI changed");
_Static_assert(sizeof(struct rtwo_ctrl_mgmt_frame) == 2344,
               "wlan_offload management frame ABI changed");
_Static_assert(sizeof(struct rtwo_ctrl_external_auth) == 44 &&
               sizeof(struct rtwo_ctrl_external_auth_response) == 4,
               "wlan_offload external-auth ABI changed");

struct wlan_offload_control_message
{
    rt_size_t length;
    rt_uint8_t bytes[];
};

struct rt_wlan_offload_control
{
    struct rt_device device;
    struct rt_wlan_offload_radio *radio;
    struct rt_mutex lock;
    struct rt_mutex dispatch_lock;
    struct rt_semaphore idle;
    struct wlan_offload_control_message *queue[WLAN_OFFLOAD_CONTROL_QUEUE_DEPTH];
    rt_uint8_t head;
    rt_uint8_t count;
    rt_bool_t opened;
    rt_bool_t registered;
    rt_bool_t attached;
    rt_bool_t removing;
    rt_bool_t unregistering;
    rt_bool_t destroying;
    rt_uint16_t active_operations;
    rt_uint32_t dropped;
};

static struct rt_wlan_offload_control *wlan_offload_control_from_fd(struct dfs_fd *fd)
{
    struct rt_device *device;

    if (!fd || !fd->fnode)
    {
        return RT_NULL;
    }
    device = (struct rt_device *)fd->fnode->data;
    return device ? (struct rt_wlan_offload_control *)device->user_data : RT_NULL;
}

static void wlan_offload_control_clear_queue_locked(struct rt_wlan_offload_control *control)
{
    while (control->count)
    {
        struct wlan_offload_control_message *message = control->queue[control->head];

        control->queue[control->head] = RT_NULL;
        control->head = (control->head + 1) % WLAN_OFFLOAD_CONTROL_QUEUE_DEPTH;
        control->count--;
        rt_free(message);
    }
    control->head = 0;
}

static rt_err_t wlan_offload_control_enqueue(struct rt_wlan_offload_control *control,
                                        rt_uint16_t type,
                                        rt_uint32_t request_id,
                                        rt_err_t status,
                                        enum rt_wlan_offload_iftype iftype,
                                        rt_uint8_t flags,
                                        const void *payload,
                                        rt_size_t payload_length)
{
    struct wlan_offload_control_message *message;
    struct rtwo_ctrl_header *header;
    rt_size_t length = sizeof(*header) + payload_length;
    rt_uint8_t tail;

    if (!control || length > RTWO_CTRL_MAX_MESSAGE_SIZE ||
        (payload_length && !payload))
    {
        return -RT_EINVAL;
    }
    message = rt_malloc(sizeof(*message) + length);
    if (!message)
    {
        return -RT_ENOMEM;
    }
    message->length = length;
    header = (struct rtwo_ctrl_header *)message->bytes;
    rt_memset(header, 0, sizeof(*header));
    header->version = RTWO_CTRL_VERSION;
    header->type = type;
    header->length = (rt_uint32_t)length;
    header->request_id = request_id;
    header->status = status;
    header->iftype = iftype < RT_WLAN_OFFLOAD_IFTYPE_MAX ? (rt_uint8_t)iftype : 0xff;
    header->flags = flags;
    if (payload_length)
    {
        rt_memcpy(message->bytes + sizeof(*header), payload, payload_length);
    }

    rt_mutex_take(&control->lock, RT_WAITING_FOREVER);
    if (!control->registered || control->removing || !control->opened)
    {
        rt_mutex_release(&control->lock);
        rt_free(message);
        return -RT_EBUSY;
    }
    if (control->count == WLAN_OFFLOAD_CONTROL_QUEUE_DEPTH)
    {
        struct wlan_offload_control_message *old = control->queue[control->head];

        control->queue[control->head] = RT_NULL;
        control->head = (control->head + 1) % WLAN_OFFLOAD_CONTROL_QUEUE_DEPTH;
        control->count--;
        control->dropped++;
        header->flags |= RTWO_CTRL_FLAG_OVERFLOW;
        rt_free(old);
    }
    tail = (control->head + control->count) % WLAN_OFFLOAD_CONTROL_QUEUE_DEPTH;
    control->queue[tail] = message;
    control->count++;
    rt_mutex_release(&control->lock);
    rt_wqueue_wakeup(&control->device.wait_queue, (void *)POLLIN);
    return RT_EOK;
}

static rt_err_t wlan_offload_control_get_info(struct rt_wlan_offload_control *control,
                                         const struct rtwo_ctrl_header *header)
{
    struct rt_wlan_offload_radio *radio = control->radio;
    struct rtwo_ctrl_info info;
    struct rt_wlan_offload_vif *vif;
    rt_size_t index;

    rt_memset(&info, 0, sizeof(info));
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    info.capabilities = radio->capabilities;
    info.max_frame_size = radio->max_frame_size;
    info.framework_api_version = RT_WLAN_OFFLOAD_API_VERSION;
    info.firmware_protocol_version = radio->firmware_info.protocol_version;
    info.firmware_version = radio->firmware_info.firmware_version;
    info.firmware_features = radio->firmware_info.features;
    info.firmware_generation = radio->firmware_generation;
    info.max_scan_ssids = radio->max_scan_ssids;
    info.max_scan_ie_length = radio->max_scan_ie_length;
    info.max_stations = radio->firmware_info.max_stations;
    info.max_vifs = radio->firmware_info.max_vifs;
    info.max_channel_contexts =
        radio->firmware_info.max_channel_contexts;
    for (index = 0; index < RT_WLAN_OFFLOAD_BAND_MAX; index++)
    {
        if (radio->bands[index])
        {
            info.band_mask |= 1U << index;
            info.phy_capabilities |= radio->bands[index]->phy_capabilities;
        }
    }
    for (index = 0; index < radio->cipher_suite_count; index++)
    {
        if (radio->cipher_suites[index] < 32)
        {
            info.cipher_mask |= 1U << radio->cipher_suites[index];
        }
    }
    if (radio->capabilities & RT_WLAN_OFFLOAD_CAP_STA)
    {
        info.iftype_mask |= 1U << RTWO_CTRL_IFTYPE_STATION;
    }
    if (radio->capabilities & RT_WLAN_OFFLOAD_CAP_AP)
    {
        info.iftype_mask |= 1U << RTWO_CTRL_IFTYPE_AP;
    }
    vif = rt_wlan_offload_get_vif(radio, (enum rt_wlan_offload_iftype)header->iftype);
    rt_memcpy(info.address, vif ? vif->address : radio->permanent_address,
              sizeof(info.address));
    rt_mutex_release(&radio->operation_lock);
    return wlan_offload_control_enqueue(control, RTWO_CTRL_EVENT_INFO,
                                   header->request_id, RT_EOK,
                                   (enum rt_wlan_offload_iftype)header->iftype,
                                   0, &info, sizeof(info));
}

/* Device object names are bounded by RT_NAME_MAX and need not be terminated. */
static rt_size_t wlan_offload_control_name_length(const char *name)
{
    rt_size_t length = 0;

    while (length < RT_NAME_MAX && name[length])
    {
        length++;
    }
    return length;
}

static void wlan_offload_control_copy_name(char *target, rt_size_t size,
                                           const char *source,
                                           rt_uint8_t *flags)
{
    rt_size_t length = wlan_offload_control_name_length(source);

    if (length >= size)
    {
        length = size - 1;
        *flags |= RTWO_CTRL_FLAG_TRUNCATED;
    }
    rt_memcpy(target, source, length);
    target[length] = '\0';
}

static rt_err_t wlan_offload_control_get_names(
    struct rt_wlan_offload_control *control,
    const struct rtwo_ctrl_header *header)
{
    struct rt_wlan_offload_radio *radio = control->radio;
    struct rtwo_ctrl_names names;
    rt_uint8_t flags = 0;
    rt_size_t index;

    rt_memset(&names, 0, sizeof(names));
    names.radio_index = WLAN_OFFLOAD_CONTROL_INDEX_NONE;
    wlan_offload_control_copy_name(names.control, sizeof(names.control),
                                   control->device.parent.name, &flags);
    rt_mutex_take(&radio->operation_lock, RT_WAITING_FOREVER);
    for (index = 0; index < RT_WLAN_OFFLOAD_WLAN_VIF_COUNT; index++)
    {
        const struct rt_wlan_offload_vif *vif = &radio->vifs[index];

        if (!vif->registered)
        {
            continue;
        }
        wlan_offload_control_copy_name(
            vif->iftype == RT_WLAN_OFFLOAD_IFTYPE_AP ? names.ap : names.station,
            RTWO_CTRL_MAX_DEVICE_NAME, vif->wlan.device.parent.name, &flags);
        /* Both interfaces of a radio share its phy index. */
        if (names.radio_index == WLAN_OFFLOAD_CONTROL_INDEX_NONE)
        {
            names.radio_index = vif->wlan.radio_index;
        }
    }
    rt_mutex_release(&radio->operation_lock);
    return wlan_offload_control_enqueue(control, RTWO_CTRL_EVENT_NAMES,
                                   header->request_id, RT_EOK,
                                   (enum rt_wlan_offload_iftype)header->iftype,
                                   flags, &names, sizeof(names));
}

static rt_err_t wlan_offload_control_set_interface(
    struct rt_wlan_offload_radio *radio, enum rt_wlan_offload_iftype iftype,
    rt_bool_t enabled)
{
    struct rt_wlan_offload_vif *vif = rt_wlan_offload_get_vif(radio, iftype);

    if (!vif)
    {
        return -RT_EINVAL;
    }
#ifdef RT_WLAN_MANAGE_ENABLE
    return rt_wlan_set_mode(
        vif->wlan.device.parent.name,
        enabled ? (iftype == RT_WLAN_OFFLOAD_IFTYPE_STATION ? RT_WLAN_STATION :
                                                             RT_WLAN_AP) :
                  RT_WLAN_NONE);
#else
    if (radio->state == RT_WLAN_OFFLOAD_REGISTERED ||
        radio->state == RT_WLAN_OFFLOAD_FAILED)
    {
        rt_err_t result;

        if (!enabled)
        {
            return RT_EOK;
        }
        result = rt_device_init((rt_device_t)&vif->wlan);
        if (result != RT_EOK)
        {
            return result;
        }
    }
    return rt_wlan_offload_change_interface(radio, iftype, enabled);
#endif
}

static rt_bool_t wlan_offload_control_channel_valid(
    const struct rtwo_ctrl_channel *wire, rt_bool_t allow_unspecified)
{
    if (!wire || wire->reserved[0] || wire->reserved[1] ||
        wire->width > RTWO_CTRL_CHANNEL_WIDTH_320)
    {
        return RT_FALSE;
    }
    if (wire->band == RTWO_CTRL_BAND_UNSPECIFIED)
    {
        return allow_unspecified && !wire->primary_channel &&
               !wire->primary_frequency_mhz &&
               !wire->center_frequency1_mhz &&
               !wire->center_frequency2_mhz;
    }
    return wire->band <= RTWO_CTRL_BAND_6GHZ && wire->primary_channel &&
           wire->primary_frequency_mhz && wire->center_frequency1_mhz &&
           (wire->width != RTWO_CTRL_CHANNEL_WIDTH_80P80 ||
            wire->center_frequency2_mhz);
}

static void wlan_offload_control_channel_from_wire(
    struct rt_wlan_offload_channel_definition *channel,
    const struct rtwo_ctrl_channel *wire)
{
    rt_memset(channel, 0, sizeof(*channel));
    channel->band = wire->band == RTWO_CTRL_BAND_UNSPECIFIED ?
                    RT_WLAN_OFFLOAD_BAND_MAX :
                    (enum rt_wlan_offload_band_id)wire->band;
    channel->width = (enum rt_wlan_offload_channel_width)wire->width;
    channel->primary_channel = wire->primary_channel;
    channel->primary_frequency_mhz = wire->primary_frequency_mhz;
    channel->center_frequency1_mhz = wire->center_frequency1_mhz;
    channel->center_frequency2_mhz = wire->center_frequency2_mhz;
}

static void wlan_offload_control_channel_to_wire(
    struct rtwo_ctrl_channel *wire,
    const struct rt_wlan_offload_channel_definition *channel)
{
    rt_memset(wire, 0, sizeof(*wire));
    wire->band = channel->band == RT_WLAN_OFFLOAD_BAND_MAX ?
                 RTWO_CTRL_BAND_UNSPECIFIED : (rt_uint8_t)channel->band;
    wire->width = (rt_uint8_t)channel->width;
    wire->primary_channel = channel->primary_channel;
    wire->primary_frequency_mhz = channel->primary_frequency_mhz;
    wire->center_frequency1_mhz = channel->center_frequency1_mhz;
    wire->center_frequency2_mhz = channel->center_frequency2_mhz;
}

static rt_err_t wlan_offload_control_dispatch(struct rt_wlan_offload_control *control,
                                         const rt_uint8_t *message,
                                         rt_size_t length)
{
    const struct rtwo_ctrl_header *header =
        (const struct rtwo_ctrl_header *)message;
    const void *payload = message + sizeof(*header);
    enum rt_wlan_offload_iftype iftype;
    rt_size_t expected;

    if (length < sizeof(*header) || header->version != RTWO_CTRL_VERSION ||
        header->length != length || !header->request_id ||
        header->iftype > RTWO_CTRL_IFTYPE_AP || header->flags ||
        header->reserved)
    {
        return -RT_EINVAL;
    }
    iftype = (enum rt_wlan_offload_iftype)header->iftype;

#define WLAN_OFFLOAD_EXPECT(_type)                                                   \
    do                                                                          \
    {                                                                           \
        expected = sizeof(*header) + sizeof(_type);                             \
        if (length != expected)                                                 \
        {                                                                       \
            return -RT_EINVAL;                                                  \
        }                                                                       \
    } while (0)

    switch (header->type)
    {
    case RTWO_CTRL_CMD_GET_INFO:
        if (length != sizeof(*header))
        {
            return -RT_EINVAL;
        }
        return wlan_offload_control_get_info(control, header);

    case RTWO_CTRL_CMD_GET_NAMES:
        if (length != sizeof(*header))
        {
            return -RT_EINVAL;
        }
        return wlan_offload_control_get_names(control, header);

    case RTWO_CTRL_CMD_SET_INTERFACE:
    {
        const struct rtwo_ctrl_set_interface *request = payload;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_set_interface);
        if (request->enabled > 1 || request->reserved[0] ||
            request->reserved[1] || request->reserved[2])
        {
            return -RT_EINVAL;
        }
        return wlan_offload_control_set_interface(
            control->radio, iftype,
            request->enabled ? RT_TRUE : RT_FALSE);
    }

    case RTWO_CTRL_CMD_SCAN:
    {
        const struct rtwo_ctrl_scan_request *wire = payload;
        struct rt_wlan_offload_scan_ssid ssids[RTWO_CTRL_MAX_SCAN_SSIDS];
        struct rt_wlan_offload_channel_definition
            channels[RTWO_CTRL_MAX_SCAN_CHANNELS];
        struct rt_wlan_offload_scan_request request;
        rt_size_t index;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_scan_request);
        if (wire->ssid_count > RTWO_CTRL_MAX_SCAN_SSIDS ||
            wire->channel_count > RTWO_CTRL_MAX_SCAN_CHANNELS ||
            wire->ies_length > RTWO_CTRL_MAX_IE_LENGTH ||
            (wire->flags & ~(RTWO_CTRL_SCAN_PASSIVE |
                             RTWO_CTRL_SCAN_RANDOM_MAC |
                             RTWO_CTRL_SCAN_FLUSH_CACHE)))
        {
            return -RT_EINVAL;
        }
        rt_memset(&request, 0, sizeof(request));
        rt_memset(ssids, 0, sizeof(ssids));
        rt_memset(channels, 0, sizeof(channels));
        request.request_id = header->request_id;
        request.ssid_count = wire->ssid_count;
        request.ssids = ssids;
        request.channel_count = wire->channel_count;
        request.channels = channels;
        request.flags = wire->flags;
        request.duration_ms = wire->duration_ms;
        request.ies_length = wire->ies_length;
        request.ies = wire->ies;
        rt_memcpy(request.bssid, wire->bssid, sizeof(request.bssid));
        for (index = 0; index < request.ssid_count; index++)
        {
            if (wire->ssids[index].length > RTWO_CTRL_MAX_SSID_LENGTH)
            {
                return -RT_EINVAL;
            }
            ssids[index].length = wire->ssids[index].length;
            rt_memcpy(ssids[index].value, wire->ssids[index].value,
                      ssids[index].length);
        }
        for (index = 0; index < request.channel_count; index++)
        {
            if (!wlan_offload_control_channel_valid(&wire->channels[index],
                                               RT_FALSE))
            {
                return -RT_EINVAL;
            }
            wlan_offload_control_channel_from_wire(&channels[index],
                                              &wire->channels[index]);
        }
        return rt_wlan_offload_scan(control->radio, iftype, &request);
    }

    case RTWO_CTRL_CMD_ABORT_SCAN:
        if (length != sizeof(*header))
        {
            return -RT_EINVAL;
        }
        return rt_wlan_offload_abort_scan(control->radio, iftype,
                                     header->request_id);

    case RTWO_CTRL_CMD_AUTHENTICATE:
    {
        const struct rtwo_ctrl_auth_request *wire = payload;
        struct rt_wlan_offload_auth_request request;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_auth_request);
        if (wire->ssid.length > RTWO_CTRL_MAX_SSID_LENGTH ||
            wire->data_length > RTWO_CTRL_MAX_IE_LENGTH ||
            wire->auth_type > RT_WLAN_OFFLOAD_AUTH_AUTOMATIC ||
            wire->reserved[0] || wire->reserved[1] || wire->reserved[2] ||
            !wlan_offload_control_channel_valid(&wire->channel, RT_FALSE))
        {
            return -RT_EINVAL;
        }
        rt_memset(&request, 0, sizeof(request));
        request.request_id = header->request_id;
        request.ssid.len = wire->ssid.length;
        rt_memcpy(request.ssid.val, wire->ssid.value, request.ssid.len);
        rt_memcpy(request.bssid, wire->bssid, sizeof(request.bssid));
        wlan_offload_control_channel_from_wire(&request.channel, &wire->channel);
        request.auth_type = (enum rt_wlan_offload_auth_type)wire->auth_type;
        request.auth_data = wire->data;
        request.auth_data_length = wire->data_length;
        return rt_wlan_offload_auth(control->radio, iftype, &request);
    }

    case RTWO_CTRL_CMD_ASSOCIATE:
    {
        const struct rtwo_ctrl_assoc_request *wire = payload;
        struct rt_wlan_offload_assoc_request request;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_assoc_request);
        if (wire->ies_length > RTWO_CTRL_MAX_IE_LENGTH)
        {
            return -RT_EINVAL;
        }
        rt_memset(&request, 0, sizeof(request));
        request.request_id = header->request_id;
        rt_memcpy(request.bssid, wire->bssid, sizeof(request.bssid));
        request.ies = wire->ies;
        request.ies_length = wire->ies_length;
        return rt_wlan_offload_assoc(control->radio, iftype, &request);
    }

    case RTWO_CTRL_CMD_DISCONNECT:
    {
        const struct rtwo_ctrl_disconnect_request *request = payload;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_disconnect_request);
        if (request->reserved)
        {
            return -RT_EINVAL;
        }
        return rt_wlan_offload_disconnect(control->radio, iftype,
                                     header->request_id, request->reason);
    }

    case RTWO_CTRL_CMD_SET_KEY:
    {
        const struct rtwo_ctrl_key_request *wire = payload;
        struct rt_wlan_offload_key key;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_key_request);
        /* The key index reaches the vendor driver unvalidated by the core, so
         * bound it here where it arrives from userspace. Drivers must still
         * range-check it against their own key table. */
        if (!wire->key_length || wire->key_length > RTWO_CTRL_MAX_KEY_LENGTH ||
            wire->sequence_length > RTWO_CTRL_MAX_SEQUENCE_LENGTH ||
            wire->index > RTWO_CTRL_MAX_KEY_INDEX ||
            wire->cipher > RT_WLAN_OFFLOAD_CIPHER_AES_CMAC || wire->pairwise > 1 ||
            wire->set_transmit > 1 || wire->reserved)
        {
            return -RT_EINVAL;
        }
        rt_memset(&key, 0, sizeof(key));
        key.cipher = (enum rt_wlan_offload_cipher)wire->cipher;
        key.index = wire->index;
        key.pairwise = wire->pairwise ? RT_TRUE : RT_FALSE;
        key.set_transmit = wire->set_transmit ? RT_TRUE : RT_FALSE;
        key.key_length = wire->key_length;
        key.sequence_length = wire->sequence_length;
        rt_memcpy(key.peer, wire->peer, sizeof(key.peer));
        rt_memcpy(key.key, wire->key, key.key_length);
        rt_memcpy(key.sequence, wire->sequence, key.sequence_length);
        return rt_wlan_offload_add_key(control->radio, iftype,
                                  header->request_id, &key);
    }

    case RTWO_CTRL_CMD_DELETE_KEY:
    {
        const struct rtwo_ctrl_delete_key_request *request = payload;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_delete_key_request);
        if (request->pairwise > 1 || request->index > RTWO_CTRL_MAX_KEY_INDEX)
        {
            return -RT_EINVAL;
        }
        return rt_wlan_offload_delete_key(control->radio, iftype,
                                     header->request_id, request->index,
                                     request->pairwise ? RT_TRUE : RT_FALSE,
                                     request->peer);
    }

    case RTWO_CTRL_CMD_SET_DEFAULT_KEY:
    {
        const struct rtwo_ctrl_default_key_request *request = payload;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_default_key_request);
        if (request->unicast > 1 || request->multicast > 1 ||
            request->index > RTWO_CTRL_MAX_KEY_INDEX || request->reserved)
        {
            return -RT_EINVAL;
        }
        return rt_wlan_offload_set_default_key(control->radio, iftype,
                                          header->request_id, request->index,
                                          request->unicast ? RT_TRUE : RT_FALSE,
                                          request->multicast ? RT_TRUE : RT_FALSE);
    }

    case RTWO_CTRL_CMD_MGMT_TX:
    {
        const struct rtwo_ctrl_mgmt_frame *wire = payload;
        struct rt_wlan_offload_mgmt_frame frame;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_mgmt_frame);
        if (!wire->data_length ||
            wire->data_length > RTWO_CTRL_MAX_FRAME_LENGTH ||
            wire->off_channel > 1 || wire->reserved[0] ||
            wire->reserved[1] || wire->reserved[2] ||
            !wlan_offload_control_channel_valid(&wire->channel, RT_FALSE))
        {
            return -RT_EINVAL;
        }
        rt_memset(&frame, 0, sizeof(frame));
        frame.request_id = header->request_id;
        wlan_offload_control_channel_from_wire(&frame.channel, &wire->channel);
        frame.off_channel = wire->off_channel ? RT_TRUE : RT_FALSE;
        frame.wait_ms = wire->wait_ms;
        frame.cookie = wire->cookie;
        frame.data = wire->data;
        frame.length = wire->data_length;
        return rt_wlan_offload_transmit_mgmt(control->radio, iftype, &frame);
    }

    case RTWO_CTRL_CMD_EAPOL_TX:
    {
        const struct rtwo_ctrl_eapol_frame *frame = payload;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_eapol_frame);
        if (!frame->data_length ||
            frame->data_length > RTWO_CTRL_MAX_EAPOL_LENGTH)
        {
            return -RT_EINVAL;
        }
        return rt_wlan_offload_transmit_eapol(control->radio, iftype,
                                         frame->destination, frame->data,
                                         frame->data_length);
    }

    case RTWO_CTRL_CMD_EXTERNAL_AUTH_RESPONSE:
    {
        const struct rtwo_ctrl_external_auth_response *response = payload;

        WLAN_OFFLOAD_EXPECT(struct rtwo_ctrl_external_auth_response);
        if (response->reserved)
        {
            return -RT_EINVAL;
        }
        return rt_wlan_offload_external_auth_response(control->radio, iftype,
                                                 response->status);
    }

    default:
        return -RT_ENOSYS;
    }
#undef WLAN_OFFLOAD_EXPECT
}

static int wlan_offload_control_open(struct dfs_fd *fd)
{
    struct rt_wlan_offload_control *control = wlan_offload_control_from_fd(fd);

    if (!control)
    {
        return -ENODEV;
    }
    rt_mutex_take(&control->lock, RT_WAITING_FOREVER);
    if (!control->registered || control->removing)
    {
        rt_mutex_release(&control->lock);
        return -ENODEV;
    }
    if (control->opened)
    {
        rt_mutex_release(&control->lock);
        return -EBUSY;
    }
    wlan_offload_control_clear_queue_locked(control);
    control->opened = RT_TRUE;
    rt_mutex_release(&control->lock);
    return 0;
}

static void wlan_offload_control_destroy(struct rt_wlan_offload_control *control)
{
    RT_ASSERT(!control->attached);
    rt_mutex_detach(&control->dispatch_lock);
    rt_sem_detach(&control->idle);
    rt_mutex_detach(&control->lock);
    rt_free(control);
}

static int wlan_offload_control_close(struct dfs_fd *fd)
{
    struct rt_wlan_offload_control *control = wlan_offload_control_from_fd(fd);
    rt_bool_t destroy;

    if (!control)
    {
        return -ENODEV;
    }
    rt_mutex_take(&control->lock, RT_WAITING_FOREVER);
    wlan_offload_control_clear_queue_locked(control);
    control->opened = RT_FALSE;
    destroy = !control->registered && !control->active_operations &&
              !control->unregistering && !control->destroying;
    if (destroy)
    {
        control->destroying = RT_TRUE;
    }
    rt_mutex_release(&control->lock);
    rt_wqueue_wakeup_all(&control->device.wait_queue,
                         (void *)(POLLHUP | POLLERR));
    if (destroy)
    {
        wlan_offload_control_destroy(control);
    }
    return 0;
}

static int wlan_offload_control_ioctl(struct dfs_fd *fd, int command, void *argument)
{
    struct rt_wlan_offload_control *control = wlan_offload_control_from_fd(fd);
    int available = 0;

    if (!control)
    {
        return -ENODEV;
    }
    if (command != FIONREAD || !argument)
    {
        return -ENOTTY;
    }
    rt_mutex_take(&control->lock, RT_WAITING_FOREVER);
    if (control->count)
    {
        available = (int)control->queue[control->head]->length;
    }
    rt_mutex_release(&control->lock);
    *(int *)argument = available;
    return 0;
}

static int wlan_offload_control_read(struct dfs_fd *fd, void *buffer, size_t count)
{
    struct rt_wlan_offload_control *control = wlan_offload_control_from_fd(fd);
    struct wlan_offload_control_message *message;
    int wait_result;

    if (!control || !buffer)
    {
        return -EINVAL;
    }
    for (;;)
    {
        rt_mutex_take(&control->lock, RT_WAITING_FOREVER);
        if (control->count)
        {
            message = control->queue[control->head];
            if (count < message->length)
            {
                rt_mutex_release(&control->lock);
                return -EMSGSIZE;
            }
            control->queue[control->head] = RT_NULL;
            control->head = (control->head + 1) % WLAN_OFFLOAD_CONTROL_QUEUE_DEPTH;
            control->count--;
            rt_mutex_release(&control->lock);
            rt_memcpy(buffer, message->bytes, message->length);
            count = message->length;
            rt_free(message);
            return (int)count;
        }
        if (!control->registered || control->removing || !control->opened)
        {
            rt_mutex_release(&control->lock);
            return -ENODEV;
        }
        rt_mutex_release(&control->lock);
        if (fd->flags & O_NONBLOCK)
        {
            return -EAGAIN;
        }
        wait_result = rt_wqueue_wait(&control->device.wait_queue, 0,
                                     RT_WAITING_FOREVER);
        if (wait_result != RT_EOK)
        {
            return wait_result;
        }
    }
}

static int wlan_offload_control_write(struct dfs_fd *fd, const void *buffer,
                                 size_t count)
{
    struct rt_wlan_offload_control *control = wlan_offload_control_from_fd(fd);
    rt_uint8_t *message;
    rt_err_t result;

    if (!control || !buffer || count < sizeof(struct rtwo_ctrl_header) ||
        count > RTWO_CTRL_MAX_MESSAGE_SIZE)
    {
        return -EINVAL;
    }
    message = rt_malloc(count);
    if (!message)
    {
        return -ENOMEM;
    }
    rt_memcpy(message, buffer, count);
    rt_mutex_take(&control->lock, RT_WAITING_FOREVER);
    if (!control->registered || control->removing || !control->opened)
    {
        rt_mutex_release(&control->lock);
        rt_free(message);
        return -ENODEV;
    }
    control->active_operations++;
    rt_mutex_release(&control->lock);

    rt_mutex_take(&control->dispatch_lock, RT_WAITING_FOREVER);
    result = wlan_offload_control_dispatch(control, message, count);
    rt_mutex_release(&control->dispatch_lock);
    rt_free(message);
    rt_mutex_take(&control->lock, RT_WAITING_FOREVER);
    control->active_operations--;
    if (!control->active_operations && control->unregistering)
    {
        rt_sem_release(&control->idle);
    }
    rt_mutex_release(&control->lock);
    return result == RT_EOK ? (int)count : result;
}

static int wlan_offload_control_poll(struct dfs_fd *fd, struct rt_pollreq *request)
{
    struct rt_wlan_offload_control *control = wlan_offload_control_from_fd(fd);
    int mask = 0;

    if (!control)
    {
        return POLLERR;
    }
    rt_poll_add(&control->device.wait_queue, request);
    rt_mutex_take(&control->lock, RT_WAITING_FOREVER);
    if (control->count)
    {
        mask |= POLLIN;
    }
    if (!control->registered || control->removing)
    {
        mask |= POLLHUP | POLLERR;
    }
    else
    {
        mask |= POLLOUT;
    }
    rt_mutex_release(&control->lock);
    return mask;
}

static const struct dfs_file_ops wlan_offload_control_fops =
{
    wlan_offload_control_open,
    wlan_offload_control_close,
    wlan_offload_control_ioctl,
    wlan_offload_control_read,
    wlan_offload_control_write,
    RT_NULL,
    RT_NULL,
    RT_NULL,
    wlan_offload_control_poll,
};

/* Follow the phy index so wlanctlN, phyN-sta, and phyN-ap describe one radio. */
static int wlan_offload_control_radio_index(
    const struct rt_wlan_offload_radio *radio)
{
    rt_size_t index;

    for (index = 0; index < RT_WLAN_OFFLOAD_WLAN_VIF_COUNT; index++)
    {
        const struct rt_wlan_offload_vif *vif = &radio->vifs[index];

        if (vif->registered && vif->wlan.radio_index < WLAN_OFFLOAD_CONTROL_INDEX_MAX)
        {
            return vif->wlan.radio_index;
        }
    }
    return -1;
}

static rt_err_t wlan_offload_control_register_auto(
    struct rt_wlan_offload_control *control)
{
    char candidate[RT_NAME_MAX];
    int preferred = wlan_offload_control_radio_index(control->radio);
    int attempt;

    /* Attempt 0 is the phy index; the rest scan for the first free name. */
    for (attempt = 0; attempt <= WLAN_OFFLOAD_CONTROL_INDEX_MAX; attempt++)
    {
        int index;

        if (attempt == 0)
        {
            if (preferred < 0)
            {
                continue;
            }
            index = preferred;
        }
        else
        {
            index = attempt - 1;
            if (index == preferred)
            {
                continue;
            }
        }
        rt_snprintf(candidate, sizeof(candidate), "%s%d",
                    WLAN_OFFLOAD_CONTROL_NAME_PREFIX, index);
        if (rt_device_find(candidate))
        {
            continue;
        }
        if (rt_device_register(&control->device, candidate,
                               RT_DEVICE_FLAG_RDWR |
                               RT_DEVICE_FLAG_STANDALONE) == RT_EOK)
        {
            return RT_EOK;
        }
    }
    return -RT_EFULL;
}

/* A NULL name lets the core assign wlanctlN; drivers should prefer that. */
rt_err_t rt_wlan_offload_control_register(struct rt_wlan_offload_radio *radio,
                                     const char *name)
{
    struct rt_wlan_offload_control *control;
    rt_err_t result;

    if (!radio || radio->control ||
        (name && (!name[0] || rt_strlen(name) >= RT_NAME_MAX)))
    {
        return -RT_EINVAL;
    }
    control = rt_calloc(1, sizeof(*control));
    if (!control)
    {
        return -RT_ENOMEM;
    }
    result = rt_mutex_init(&control->lock, "wo-ctl", RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_free(control);
        return result;
    }
    result = rt_sem_init(&control->idle, "wo-idle", 0, RT_IPC_FLAG_FIFO);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&control->lock);
        rt_free(control);
        return result;
    }
    result = rt_mutex_init(&control->dispatch_lock, "wo-req",
                           RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        rt_sem_detach(&control->idle);
        rt_mutex_detach(&control->lock);
        rt_free(control);
        return result;
    }
    control->radio = radio;
    control->registered = RT_TRUE;
    control->device.type = RT_Device_Class_Miscellaneous;
    control->device.user_data = control;
    result = name ? rt_device_register(&control->device, name,
                                       RT_DEVICE_FLAG_RDWR |
                                       RT_DEVICE_FLAG_STANDALONE) :
                    wlan_offload_control_register_auto(control);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&control->dispatch_lock);
        rt_sem_detach(&control->idle);
        rt_mutex_detach(&control->lock);
        rt_free(control);
        return result;
    }
    control->device.fops = &wlan_offload_control_fops;
    radio->control = control;
    control->attached = RT_TRUE;
    return RT_EOK;
}

/* Call from the driver attach or detach path, where the radio cannot vanish. */
rt_err_t rt_wlan_offload_control_get_name(
    const struct rt_wlan_offload_radio *radio, char *name, rt_size_t size)
{
    rt_size_t length;

    if (!radio || !radio->control || !name || !size)
    {
        return -RT_EINVAL;
    }
    length = wlan_offload_control_name_length(
        radio->control->device.parent.name);
    if (length >= size)
    {
        return -RT_EFULL;
    }
    rt_memcpy(name, radio->control->device.parent.name, length);
    name[length] = '\0';
    return RT_EOK;
}

rt_err_t rt_wlan_offload_control_unregister(struct rt_wlan_offload_radio *radio)
{
    struct rt_wlan_offload_control *control;
    rt_bool_t destroy;
    rt_bool_t wait;

    if (!radio || !radio->control)
    {
        return RT_EOK;
    }
    control = radio->control;
    rt_mutex_take(&control->lock, RT_WAITING_FOREVER);
    control->registered = RT_FALSE;
    control->removing = RT_TRUE;
    control->unregistering = RT_TRUE;
    wait = control->active_operations != 0;
    rt_mutex_release(&control->lock);
    rt_wqueue_wakeup_all(&control->device.wait_queue,
                         (void *)(POLLHUP | POLLERR));
    if (wait)
    {
        rt_sem_take(&control->idle, RT_WAITING_FOREVER);
    }
    rt_device_unregister(&control->device);
    rt_mutex_take(&control->lock, RT_WAITING_FOREVER);
    control->attached = RT_FALSE;
    control->radio = RT_NULL;
    control->unregistering = RT_FALSE;
    destroy = !control->opened && !control->destroying;
    if (destroy)
    {
        control->destroying = RT_TRUE;
    }
    rt_mutex_release(&control->lock);
    radio->control = RT_NULL;
    if (destroy)
    {
        wlan_offload_control_destroy(control);
    }
    return RT_EOK;
}

static void wlan_offload_control_copy_network(
    struct rtwo_ctrl_network *wire, const struct rt_wlan_offload_network *network,
    rt_uint8_t *flags)
{
    rt_size_t length;

    rt_memset(wire, 0, sizeof(*wire));
    wire->ssid.length = network->ssid.len;
    rt_memcpy(wire->ssid.value, network->ssid.val, wire->ssid.length);
    rt_memcpy(wire->bssid, network->bssid, sizeof(wire->bssid));
    wlan_offload_control_channel_to_wire(&wire->channel, &network->channel);
    wire->rssi = network->rssi;
    wire->security = network->security;
    wire->beacon_interval = network->beacon_interval;
    wire->capability = network->capability;
    length = network->ies_length;
    if (length > sizeof(wire->ies))
    {
        length = sizeof(wire->ies);
        *flags |= RTWO_CTRL_FLAG_TRUNCATED;
    }
    wire->ies_length = length;
    if (length)
    {
        rt_memcpy(wire->ies, network->ies, length);
    }
}

void rt_wlan_offload_control_report_event(struct rt_wlan_offload_radio *radio,
                                     const struct rt_wlan_offload_event *event)
{
    struct rt_wlan_offload_control *control;
    rt_uint16_t type;
    rt_uint8_t flags = 0;

    if (!radio || !event || !radio->control)
    {
        return;
    }
    control = radio->control;
    switch (event->type)
    {
    case RT_WLAN_OFFLOAD_EVENT_RADIO_ONLINE:
        type = RTWO_CTRL_EVENT_RADIO_ONLINE;
        break;
    case RT_WLAN_OFFLOAD_EVENT_RADIO_OFFLINE:
        type = RTWO_CTRL_EVENT_RADIO_OFFLINE;
        break;
    case RT_WLAN_OFFLOAD_EVENT_SCAN_RESULT:
    {
        struct rtwo_ctrl_network *network = rt_malloc(sizeof(*network));

        if (!network)
        {
            return;
        }
        wlan_offload_control_copy_network(network, &event->data.network, &flags);
        wlan_offload_control_enqueue(control, RTWO_CTRL_EVENT_SCAN_RESULT,
                                event->request_id, event->status, event->iftype,
                                flags, network, sizeof(*network));
        rt_free(network);
        return;
    }
    case RT_WLAN_OFFLOAD_EVENT_SCAN_DONE:
        type = RTWO_CTRL_EVENT_SCAN_DONE;
        break;
    case RT_WLAN_OFFLOAD_EVENT_CONNECT_RESULT:
        type = RTWO_CTRL_EVENT_CONNECT_RESULT;
        break;
    case RT_WLAN_OFFLOAD_EVENT_DISCONNECTED:
    {
        struct rtwo_ctrl_disconnected disconnected;

        rt_memset(&disconnected, 0, sizeof(disconnected));
        rt_memcpy(disconnected.bssid, event->data.disconnected.bssid,
                  sizeof(disconnected.bssid));
        disconnected.reason = event->data.disconnected.reason;
        disconnected.locally_generated = event->data.disconnected.locally_generated;
        wlan_offload_control_enqueue(control, RTWO_CTRL_EVENT_DISCONNECTED,
                                event->request_id, event->status, event->iftype,
                                0, &disconnected, sizeof(disconnected));
        return;
    }
    case RT_WLAN_OFFLOAD_EVENT_AUTH_RX:
        type = RTWO_CTRL_EVENT_AUTH_RX;
        goto management;
    case RT_WLAN_OFFLOAD_EVENT_ASSOC_RX:
        type = RTWO_CTRL_EVENT_ASSOC_RX;
        goto management;
    case RT_WLAN_OFFLOAD_EVENT_MGMT_RX:
        type = RTWO_CTRL_EVENT_MGMT_RX;
management:
    {
        struct rtwo_ctrl_rx_frame *frame = rt_calloc(1, sizeof(*frame));
        rt_size_t length = event->data.management.length;

        if (!frame)
        {
            return;
        }
        if (length > sizeof(frame->data))
        {
            length = sizeof(frame->data);
            flags |= RTWO_CTRL_FLAG_TRUNCATED;
        }
        wlan_offload_control_channel_to_wire(&frame->channel,
                                        &event->data.management.channel);
        frame->rssi = event->data.management.rssi;
        frame->data_length = length;
        if (length)
        {
            rt_memcpy(frame->data, event->data.management.data, length);
        }
        wlan_offload_control_enqueue(control, type, event->request_id, event->status,
                                event->iftype, flags, frame, sizeof(*frame));
        rt_free(frame);
        return;
    }
    case RT_WLAN_OFFLOAD_EVENT_MGMT_TX_STATUS:
    {
        struct rtwo_ctrl_tx_status *status = rt_calloc(1, sizeof(*status));
        rt_size_t length = event->data.tx_status.length;

        if (!status)
        {
            return;
        }
        if (length > sizeof(status->data))
        {
            length = sizeof(status->data);
            flags |= RTWO_CTRL_FLAG_TRUNCATED;
        }
        status->cookie = event->data.tx_status.cookie;
        status->acknowledged = event->data.tx_status.acknowledged;
        status->data_length = length;
        if (length)
        {
            rt_memcpy(status->data, event->data.tx_status.data, length);
        }
        wlan_offload_control_enqueue(control, RTWO_CTRL_EVENT_MGMT_TX_STATUS,
                                event->request_id, event->status, event->iftype,
                                flags, status, sizeof(*status));
        rt_free(status);
        return;
    }
    case RT_WLAN_OFFLOAD_EVENT_EAPOL_RX:
    {
        struct rtwo_ctrl_eapol_frame *frame = rt_calloc(1, sizeof(*frame));
        rt_size_t length = event->data.eapol.length;

        if (!frame)
        {
            return;
        }
        if (length > sizeof(frame->data))
        {
            length = sizeof(frame->data);
            flags |= RTWO_CTRL_FLAG_TRUNCATED;
        }
        rt_memcpy(frame->source, event->data.eapol.source,
                  sizeof(frame->source));
        rt_memcpy(frame->destination, event->data.eapol.destination,
                  sizeof(frame->destination));
        frame->data_length = length;
        if (length)
        {
            rt_memcpy(frame->data, event->data.eapol.data, length);
        }
        wlan_offload_control_enqueue(control, RTWO_CTRL_EVENT_EAPOL_RX,
                                event->request_id, event->status, event->iftype,
                                flags, frame, sizeof(*frame));
        rt_free(frame);
        return;
    }
    case RT_WLAN_OFFLOAD_EVENT_REGULATORY_CHANGED:
        type = RTWO_CTRL_EVENT_REGULATORY_CHANGED;
        break;
    case RT_WLAN_OFFLOAD_EVENT_FIRMWARE_ERROR:
    {
        struct rtwo_ctrl_firmware_error *error = rt_calloc(1, sizeof(*error));
        rt_size_t length = event->data.firmware.dump_length;

        if (!error)
        {
            return;
        }
        if (length > sizeof(error->dump))
        {
            length = sizeof(error->dump);
            flags |= RTWO_CTRL_FLAG_TRUNCATED;
        }
        error->reason = event->data.firmware.reason;
        error->dump_length = length;
        if (length)
        {
            rt_memcpy(error->dump, event->data.firmware.dump, length);
        }
        wlan_offload_control_enqueue(control, RTWO_CTRL_EVENT_FIRMWARE_ERROR,
                                event->request_id, event->status, event->iftype,
                                flags, error, sizeof(*error));
        rt_free(error);
        return;
    }
    case RT_WLAN_OFFLOAD_EVENT_EXTERNAL_AUTH_REQUIRED:
    {
        struct rtwo_ctrl_external_auth *auth =
            rt_calloc(1, sizeof(*auth));
        rt_size_t length = event->data.external_auth.ssid.len;

        if (!auth || !length || length > RTWO_CTRL_MAX_SSID_LENGTH)
        {
            rt_free(auth);
            return;
        }
        auth->ssid.length = length;
        rt_memcpy(auth->ssid.value, event->data.external_auth.ssid.val,
                  length);
        rt_memcpy(auth->bssid, event->data.external_auth.bssid,
                  sizeof(auth->bssid));
        auth->akm_suite = event->data.external_auth.akm_suite;
        wlan_offload_control_enqueue(control, RTWO_CTRL_EVENT_EXTERNAL_AUTH_REQUIRED,
                                event->request_id, event->status, event->iftype,
                                0, auth, sizeof(*auth));
        rt_free(auth);
        return;
    }
    case RT_WLAN_OFFLOAD_EVENT_TKIP_MIC_FAILURE:
    {
        struct rtwo_ctrl_tkip_mic_failure failure;

        rt_memset(&failure, 0, sizeof(failure));
        rt_memcpy(failure.source, event->data.mic_failure.source,
                  sizeof(failure.source));
        rt_memcpy(failure.tsc, event->data.mic_failure.tsc,
                  sizeof(failure.tsc));
        failure.key_index = event->data.mic_failure.key_index;
        failure.group = event->data.mic_failure.group;
        wlan_offload_control_enqueue(
            control, RTWO_CTRL_EVENT_TKIP_MIC_FAILURE,
            event->request_id, event->status, event->iftype, 0,
            &failure, sizeof(failure));
        return;
    }
    default:
        return;
    }
    wlan_offload_control_enqueue(control, type, event->request_id, event->status,
                            event->iftype, flags, RT_NULL, 0);
}
