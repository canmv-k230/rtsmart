/*
 * Copyright (c) 2024, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_core.h"
#include "usbh_cdc_ncm.h"

#undef USB_DBG_TAG
#define USB_DBG_TAG "usbh_cdc_ncm"
#include "usb_log.h"

#define DEV_FORMAT                            "/dev/cdc_ncm"

/* general descriptor field offsets */
#define DESC_bLength                          0 /** Length offset */
#define DESC_bDescriptorType                  1 /** Descriptor type offset */
#define DESC_bDescriptorSubType               2 /** Descriptor subtype offset */

/* interface descriptor field offsets */
#define INTF_DESC_bInterfaceNumber            2 /** Interface number offset */
#define INTF_DESC_bAlternateSetting           3 /** Alternate setting offset */

#define CONFIG_USBHOST_CDC_NCM_ETH_MAX_SEGSZE 1522U

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_cdc_ncm_rx_buffer[USB_ALIGN_UP(CONFIG_USBHOST_CDC_NCM_ETH_MAX_SEGSZE * 4, CONFIG_USB_ALIGN_SIZE)];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_cdc_ncm_tx_buffer[USB_ALIGN_UP(CONFIG_USBHOST_CDC_NCM_ETH_MAX_SEGSZE * 4, CONFIG_USB_ALIGN_SIZE)];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_cdc_ncm_inttx_buffer[USB_ALIGN_UP(16, CONFIG_USB_ALIGN_SIZE)];

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_cdc_ncm_buf[32];

static struct usbh_cdc_ncm g_cdc_ncm_class;
static usb_osal_mutex_t g_cdc_ncm_tx_mutex;

static void usbh_cdc_ncm_set_link_state(struct usbh_cdc_ncm *cdc_ncm_class, bool connected)
{
    if (cdc_ncm_class->connect_status != connected) {
        cdc_ncm_class->connect_status = connected;
        usbh_cdc_ncm_link_changed(cdc_ncm_class, connected);
    } else {
        cdc_ncm_class->connect_status = connected;
    }
}

static bool usbh_cdc_ncm_is_active(struct usbh_cdc_ncm *cdc_ncm_class)
{
    return !cdc_ncm_class->stop_requested &&
           usbh_find_class_instance(DEV_FORMAT) == cdc_ncm_class;
}

static bool usbh_cdc_ncm_can_xmit(struct usbh_cdc_ncm *cdc_ncm_class)
{
    return usbh_cdc_ncm_is_active(cdc_ncm_class) &&
           cdc_ncm_class->connect_status &&
           cdc_ncm_class->hport &&
           cdc_ncm_class->bulkout;
}

static void usbh_cdc_ncm_handle_rx_error(struct usbh_cdc_ncm *cdc_ncm_class)
{
    usbh_cdc_ncm_set_link_state(cdc_ncm_class, false);
}

static int usbh_cdc_ncm_get_ntb_parameters(struct usbh_cdc_ncm *cdc_ncm_class, struct cdc_ncm_ntb_parameters *param)
{
    struct usb_setup_packet *setup = cdc_ncm_class->hport->setup;
    int ret;

    setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    setup->bRequest = CDC_REQUEST_GET_NTB_PARAMETERS;
    setup->wValue = 0;
    setup->wIndex = cdc_ncm_class->ctrl_intf;
    setup->wLength = 28;

    ret = usbh_control_transfer(cdc_ncm_class->hport, setup, g_cdc_ncm_buf);
    if (ret < 0) {
        return ret;
    }

    if (ret < 8 || ret > sizeof(g_cdc_ncm_buf)) {
        USB_LOG_ERR("CDC NCM NTB parameter length invalid: %d\r\n", ret);
        return -USB_ERR_IO;
    }

    usb_memcpy((uint8_t *)param, g_cdc_ncm_buf, ret - 8);
    return 0;
}

static void print_ntb_parameters(struct cdc_ncm_ntb_parameters *param)
{
    USB_LOG_RAW("CDC NCM ntb parameters:\r\n");
    USB_LOG_RAW("wLength: 0x%02x             \r\n", param->wLength);
    USB_LOG_RAW("bmNtbFormatsSupported: %s     \r\n", param->bmNtbFormatsSupported ? "NTB16" : "NTB32");

    USB_LOG_RAW("dwNtbInMaxSize: 0x%04x           \r\n", param->dwNtbInMaxSize);
    USB_LOG_RAW("wNdbInDivisor: 0x%02x \r\n", param->wNdbInDivisor);
    USB_LOG_RAW("wNdbInPayloadRemainder: 0x%02x      \r\n", param->wNdbInPayloadRemainder);
    USB_LOG_RAW("wNdbInAlignment: 0x%02x    \r\n", param->wNdbInAlignment);

    USB_LOG_RAW("dwNtbOutMaxSize: 0x%04x     \r\n", param->dwNtbOutMaxSize);
    USB_LOG_RAW("wNdbOutDivisor: 0x%02x     \r\n", param->wNdbOutDivisor);
    USB_LOG_RAW("wNdbOutPayloadRemainder: 0x%02x     \r\n", param->wNdbOutPayloadRemainder);
    USB_LOG_RAW("wNdbOutAlignment: 0x%02x     \r\n", param->wNdbOutAlignment);

    USB_LOG_RAW("wNtbOutMaxDatagrams: 0x%02x     \r\n", param->wNtbOutMaxDatagrams);
}

int usbh_cdc_ncm_get_connect_status(struct usbh_cdc_ncm *cdc_ncm_class)
{
    int ret;

    usbh_int_urb_fill(&cdc_ncm_class->intin_urb, cdc_ncm_class->hport, cdc_ncm_class->intin, g_cdc_ncm_inttx_buffer, 16, USB_OSAL_WAITING_FOREVER, NULL, NULL);
    ret = usbh_submit_urb(&cdc_ncm_class->intin_urb);
    if (ret < 0) {
        return ret;
    }

    if (g_cdc_ncm_inttx_buffer[1] == CDC_ECM_NOTIFY_CODE_NETWORK_CONNECTION) {
        if (g_cdc_ncm_inttx_buffer[2] == CDC_ECM_NET_CONNECTED) {
            usbh_cdc_ncm_set_link_state(cdc_ncm_class, true);
        } else {
            usbh_cdc_ncm_set_link_state(cdc_ncm_class, false);
        }
    } else if (g_cdc_ncm_inttx_buffer[1] == CDC_ECM_NOTIFY_CODE_CONNECTION_SPEED_CHANGE) {
        usb_memcpy(cdc_ncm_class->speed, &g_cdc_ncm_inttx_buffer[8], 8);
    }
    return 0;
}

static int usbh_cdc_ncm_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usb_endpoint_descriptor *ep_desc;
    int ret;
    uint8_t altsetting = 0;
    char mac_buffer[12];
    uint8_t *p;
    uint8_t cur_iface = 0xff;
    uint8_t mac_str_idx = 0xff;

    struct usbh_cdc_ncm *cdc_ncm_class = &g_cdc_ncm_class;

    if (g_cdc_ncm_tx_mutex == NULL) {
        g_cdc_ncm_tx_mutex = usb_osal_mutex_create();
        if (g_cdc_ncm_tx_mutex == NULL) {
            return -USB_ERR_NOMEM;
        }
    }

    usb_memset(cdc_ncm_class, 0, sizeof(struct usbh_cdc_ncm));

    cdc_ncm_class->hport = hport;
    cdc_ncm_class->ctrl_intf = intf;
    cdc_ncm_class->data_intf = intf + 1;

    hport->config.intf[intf].priv = cdc_ncm_class;
    hport->config.intf[intf + 1].priv = NULL;

    p = hport->raw_config_desc;
    while (p[DESC_bLength]) {
        switch (p[DESC_bDescriptorType]) {
            case USB_DESCRIPTOR_TYPE_INTERFACE:
                cur_iface = p[INTF_DESC_bInterfaceNumber];
                //cur_alt_setting = p[INTF_DESC_bAlternateSetting];
                break;
            case CDC_CS_INTERFACE:
                if ((cur_iface == cdc_ncm_class->ctrl_intf) && p[DESC_bDescriptorSubType] == CDC_FUNC_DESC_ETHERNET_NETWORKING) {
                    struct cdc_eth_descriptor *desc = (struct cdc_eth_descriptor *)p;
                    mac_str_idx = desc->iMACAddress;
                    cdc_ncm_class->max_segment_size = desc->wMaxSegmentSize;
                    goto get_mac;
                }
                break;

            default:
                break;
        }
        /* skip to next descriptor */
        p += p[DESC_bLength];
    }

get_mac:
    if (mac_str_idx == 0xff) {
        USB_LOG_ERR("Do not find cdc ncm mac string\r\n");
        return -1;
    }

    usb_memset(mac_buffer, 0, 12);
    ret = usbh_get_string_desc(cdc_ncm_class->hport, mac_str_idx, (uint8_t *)mac_buffer, sizeof(mac_buffer));
    if (ret < 0) {
        return ret;
    }

    for (int i = 0, j = 0; i < 12; i += 2, j++) {
        char byte_str[3];
        byte_str[0] = mac_buffer[i];
        byte_str[1] = mac_buffer[i + 1];
        byte_str[2] = '\0';

        uint32_t byte = strtoul(byte_str, NULL, 16);
        cdc_ncm_class->mac[j] = (unsigned char)byte;
    }

    USB_LOG_INFO("CDC NCM MAC address %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                 cdc_ncm_class->mac[0],
                 cdc_ncm_class->mac[1],
                 cdc_ncm_class->mac[2],
                 cdc_ncm_class->mac[3],
                 cdc_ncm_class->mac[4],
                 cdc_ncm_class->mac[5]);

    if (cdc_ncm_class->max_segment_size > sizeof(g_cdc_ncm_rx_buffer)) {
        USB_LOG_ERR("CDC NCM Max Segment Size is overflow, buffer is %u, but device reports %u\r\n", (unsigned)sizeof(g_cdc_ncm_rx_buffer), cdc_ncm_class->max_segment_size);
    } else {
        USB_LOG_INFO("CDC NCM Max Segment Size:%u\r\n", cdc_ncm_class->max_segment_size);
    }

    ret = usbh_cdc_ncm_get_ntb_parameters(cdc_ncm_class, &cdc_ncm_class->ntb_param);
    if (ret < 0) {
        return ret;
    }
    print_ntb_parameters(&cdc_ncm_class->ntb_param);

    /* enable int ep */
    ep_desc = &hport->config.intf[intf].altsetting[0].ep[0].ep_desc;
    USBH_EP_INIT(cdc_ncm_class->intin, ep_desc);

    if (hport->config.intf[intf + 1].altsetting_num > 1) {
        altsetting = hport->config.intf[intf + 1].altsetting_num - 1;

        for (uint8_t i = 0; i < hport->config.intf[intf + 1].altsetting[altsetting].intf_desc.bNumEndpoints; i++) {
            ep_desc = &hport->config.intf[intf + 1].altsetting[altsetting].ep[i].ep_desc;

            if (ep_desc->bEndpointAddress & 0x80) {
                USBH_EP_INIT(cdc_ncm_class->bulkin, ep_desc);
            } else {
                USBH_EP_INIT(cdc_ncm_class->bulkout, ep_desc);
            }
        }

        USB_LOG_INFO("Select cdc ncm altsetting: %d\r\n", altsetting);
        usbh_set_interface(cdc_ncm_class->hport, cdc_ncm_class->data_intf, altsetting);
    } else {
        for (uint8_t i = 0; i < hport->config.intf[intf + 1].altsetting[0].intf_desc.bNumEndpoints; i++) {
            ep_desc = &hport->config.intf[intf + 1].altsetting[0].ep[i].ep_desc;

            if (ep_desc->bEndpointAddress & 0x80) {
                USBH_EP_INIT(cdc_ncm_class->bulkin, ep_desc);
            } else {
                USBH_EP_INIT(cdc_ncm_class->bulkout, ep_desc);
            }
        }
    }

    usb_memcpy(hport->config.intf[intf].devname, DEV_FORMAT, CONFIG_USBHOST_DEV_NAMELEN);

    USB_LOG_INFO("Register CDC NCM Class:%s\r\n", hport->config.intf[intf].devname);

    usbh_cdc_ncm_run(cdc_ncm_class);
    return ret;
}

static int usbh_cdc_ncm_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    int ret = 0;
    bool registered;

    struct usbh_cdc_ncm *cdc_ncm_class = (struct usbh_cdc_ncm *)hport->config.intf[intf].priv;

    if (cdc_ncm_class) {
        cdc_ncm_class->stop_requested = true;
        usbh_cdc_ncm_set_link_state(cdc_ncm_class, false);
        registered = hport->config.intf[intf].devname[0] != '\0';
        hport->config.intf[intf].priv = NULL;
        hport->config.intf[intf].devname[0] = '\0';

        if (cdc_ncm_class->bulkin) {
            usbh_kill_urb(&cdc_ncm_class->bulkin_urb);
        }

        if (cdc_ncm_class->bulkout) {
            usbh_kill_urb(&cdc_ncm_class->bulkout_urb);
        }

        if (cdc_ncm_class->intin) {
            usbh_kill_urb(&cdc_ncm_class->intin_urb);
        }

        if (g_cdc_ncm_tx_mutex) {
            usb_osal_mutex_take(g_cdc_ncm_tx_mutex);
            usb_osal_mutex_give(g_cdc_ncm_tx_mutex);
        }

        if (registered) {
            USB_LOG_INFO("Unregister CDC NCM Class:%s\r\n", DEV_FORMAT);
            usbh_cdc_ncm_stop(cdc_ncm_class);
        }

        usb_memset(cdc_ncm_class, 0, sizeof(struct usbh_cdc_ncm));
    }

    return ret;
}

void usbh_cdc_ncm_rx_thread(void *argument)
{
    uint32_t g_cdc_ncm_rx_length;
    int ret;
    err_t err;
    struct pbuf *p;
    struct netif *netif = (struct netif *)argument;

    USB_LOG_INFO("Create cdc ncm rx thread\r\n");
    g_cdc_ncm_class.rx_thread_running = true;
    // clang-format off
find_class:
    // clang-format on
    usbh_cdc_ncm_set_link_state(&g_cdc_ncm_class, false);
    if (!usbh_cdc_ncm_is_active(&g_cdc_ncm_class)) {
        goto delete;
    }

    while (!g_cdc_ncm_class.stop_requested && g_cdc_ncm_class.connect_status == false) {
        ret = usbh_cdc_ncm_get_connect_status(&g_cdc_ncm_class);
        if (ret < 0) {
            if (!usbh_cdc_ncm_is_active(&g_cdc_ncm_class)) {
                goto delete;
            }
            usb_osal_msleep(100);
            goto find_class;
        }
    }

    if (g_cdc_ncm_class.stop_requested) {
        goto delete;
    }

    g_cdc_ncm_rx_length = 0;
    while (!g_cdc_ncm_class.stop_requested) {
        if (!g_cdc_ncm_class.bulkin || !g_cdc_ncm_class.hport || !usbh_cdc_ncm_is_active(&g_cdc_ncm_class)) {
            goto find_class;
        }

        uint32_t max_pkt = USB_GET_MAXPACKETSIZE(g_cdc_ncm_class.bulkin->wMaxPacketSize);

        if (g_cdc_ncm_rx_length + max_pkt > sizeof(g_cdc_ncm_rx_buffer)) {
            USB_LOG_ERR("CDC NCM RX overflow, dropping frame (len=%u)\r\n", g_cdc_ncm_rx_length);
            /* Drain remaining packets of this frame until short packet (end of frame) */
            for (int drain = 0; drain < 128; drain++) {
                usbh_bulk_urb_fill(&g_cdc_ncm_class.bulkin_urb, g_cdc_ncm_class.hport, g_cdc_ncm_class.bulkin, g_cdc_ncm_rx_buffer, max_pkt, USB_OSAL_WAITING_FOREVER, NULL, NULL);
                ret = usbh_submit_urb(&g_cdc_ncm_class.bulkin_urb);
                if (ret < 0) {
                    usbh_cdc_ncm_handle_rx_error(&g_cdc_ncm_class);
                    goto find_class;
                }
                if (g_cdc_ncm_class.bulkin_urb.actual_length != max_pkt) {
                    break; /* Short packet = end of frame */
                }
            }
            g_cdc_ncm_rx_length = 0;
            continue;
        }

        usbh_bulk_urb_fill(&g_cdc_ncm_class.bulkin_urb, g_cdc_ncm_class.hport, g_cdc_ncm_class.bulkin, &g_cdc_ncm_rx_buffer[g_cdc_ncm_rx_length], max_pkt, USB_OSAL_WAITING_FOREVER, NULL, NULL);
        ret = usbh_submit_urb(&g_cdc_ncm_class.bulkin_urb);
        if (ret < 0) {
            usbh_cdc_ncm_handle_rx_error(&g_cdc_ncm_class);
            goto find_class;
        }

        g_cdc_ncm_rx_length += g_cdc_ncm_class.bulkin_urb.actual_length;

        if (g_cdc_ncm_class.bulkin_urb.actual_length != max_pkt) {
            USB_LOG_DBG("rxlen:%d\r\n", g_cdc_ncm_rx_length);

            struct cdc_ncm_nth16 *nth16 = (struct cdc_ncm_nth16 *)&g_cdc_ncm_rx_buffer[0];
            if ((nth16->dwSignature != CDC_NCM_NTH16_SIGNATURE) ||
                (nth16->wHeaderLength != 12) ||
                (nth16->wBlockLength != g_cdc_ncm_rx_length)) {
                USB_LOG_ERR("invalid rx nth16\r\n");
                g_cdc_ncm_rx_length = 0;
                continue;
            }

            if (nth16->wNdpIndex + 8 > g_cdc_ncm_rx_length) {
                USB_LOG_ERR("CDC NCM NDP index out of bounds\r\n");
                g_cdc_ncm_rx_length = 0;
                continue;
            }

            struct cdc_ncm_ndp16 *ndp16 = (struct cdc_ncm_ndp16 *)&g_cdc_ncm_rx_buffer[nth16->wNdpIndex];
            if ((ndp16->dwSignature != CDC_NCM_NDP16_SIGNATURE_NCM0) && (ndp16->dwSignature != CDC_NCM_NDP16_SIGNATURE_NCM1)) {
                USB_LOG_ERR("invalid rx ndp16\r\n");
                g_cdc_ncm_rx_length = 0;
                continue;
            }

            if (ndp16->wLength < 8 || nth16->wNdpIndex + ndp16->wLength > g_cdc_ncm_rx_length) {
                USB_LOG_ERR("CDC NCM NDP length invalid\r\n");
                g_cdc_ncm_rx_length = 0;
                continue;
            }

            uint16_t datagram_num = (ndp16->wLength - 8) / 4;

            USB_LOG_DBG("datagram num:%02x\r\n", datagram_num);
            for (uint16_t i = 0; i < datagram_num; i++) {
                struct cdc_ncm_ndp16_datagram *ndp16_datagram = (struct cdc_ncm_ndp16_datagram *)&g_cdc_ncm_rx_buffer[nth16->wNdpIndex + 8 + 4 * i];
                if (ndp16_datagram->wDatagramIndex && ndp16_datagram->wDatagramLength) {
                    if ((uint32_t)ndp16_datagram->wDatagramIndex + ndp16_datagram->wDatagramLength > g_cdc_ncm_rx_length) {
                        USB_LOG_ERR("CDC NCM datagram out of bounds\r\n");
                        break;
                    }
                    USB_LOG_DBG("ndp16_datagram index:%02x, length:%02x\r\n", ndp16_datagram->wDatagramIndex, ndp16_datagram->wDatagramLength);

                    p = pbuf_alloc(PBUF_RAW, ndp16_datagram->wDatagramLength, PBUF_POOL);
                    if (p != NULL) {
                        usb_memcpy(p->payload, (uint8_t *)&g_cdc_ncm_rx_buffer[ndp16_datagram->wDatagramIndex], ndp16_datagram->wDatagramLength);

                        err = netif->input(p, netif);
                        if (err != ERR_OK) {
                            pbuf_free(p);
                        }
                    } else {
                        USB_LOG_ERR("No memory to alloc pbuf for cdc ncm rx\r\n");
                    }
                }
            }

            g_cdc_ncm_rx_length = 0;

        } else {
        }
    }
    // clang-format off
delete:
    g_cdc_ncm_class.rx_thread_running = false;
    USB_LOG_INFO("Delete cdc ncm rx thread\r\n");
    usb_osal_thread_delete(NULL);
    // clang-format on
}

err_t usbh_cdc_ncm_linkoutput(struct netif *netif, struct pbuf *p)
{
    int ret;
    struct pbuf *q;
    uint8_t *buffer;
    struct cdc_ncm_ndp16_datagram *ndp16_datagram;

    if (g_cdc_ncm_tx_mutex == NULL) {
        return ERR_BUF;
    }

    usb_osal_mutex_take(g_cdc_ncm_tx_mutex);

    if (!usbh_cdc_ncm_can_xmit(&g_cdc_ncm_class)) {
        usb_osal_mutex_give(g_cdc_ncm_tx_mutex);
        return ERR_BUF;
    }

    if (16 + 16 + USB_ALIGN_UP(p->tot_len, 4) > sizeof(g_cdc_ncm_tx_buffer)) {
        USB_LOG_ERR("CDC NCM TX frame too large (%u)\r\n", p->tot_len);
        usb_osal_mutex_give(g_cdc_ncm_tx_mutex);
        return ERR_BUF;
    }

    struct cdc_ncm_nth16 *nth16 = (struct cdc_ncm_nth16 *)&g_cdc_ncm_tx_buffer[0];
    
    nth16->dwSignature = CDC_NCM_NTH16_SIGNATURE;
    nth16->wHeaderLength = 12;
    nth16->wSequence = g_cdc_ncm_class.bulkout_sequence++;
    nth16->wBlockLength = 16 + 16 + USB_ALIGN_UP(p->tot_len, 4);
    nth16->wNdpIndex = 16 + USB_ALIGN_UP(p->tot_len, 4);

    struct cdc_ncm_ndp16 *ndp16 = (struct cdc_ncm_ndp16 *)&g_cdc_ncm_tx_buffer[nth16->wNdpIndex];

    ndp16->dwSignature = CDC_NCM_NDP16_SIGNATURE_NCM0;
    ndp16->wLength = 16;
    ndp16->wNextNdpIndex = 0;

    ndp16_datagram = (struct cdc_ncm_ndp16_datagram *)&g_cdc_ncm_tx_buffer[nth16->wNdpIndex + 8 + 4 * 0];
    ndp16_datagram->wDatagramIndex = 16;
    ndp16_datagram->wDatagramLength = p->tot_len;

    ndp16_datagram = (struct cdc_ncm_ndp16_datagram *)&g_cdc_ncm_tx_buffer[nth16->wNdpIndex + 8 + 4 * 1];
    ndp16_datagram->wDatagramIndex = 0;
    ndp16_datagram->wDatagramLength = 0;

    buffer = &g_cdc_ncm_tx_buffer[16];

    for (q = p; q != NULL; q = q->next) {
        usb_memcpy(buffer, q->payload, q->len);
        buffer += q->len;
    }

    USB_LOG_DBG("txlen:%d\r\n", nth16->wBlockLength);

    usbh_bulk_urb_fill(&g_cdc_ncm_class.bulkout_urb, g_cdc_ncm_class.hport, g_cdc_ncm_class.bulkout, g_cdc_ncm_tx_buffer, nth16->wBlockLength, USB_OSAL_WAITING_FOREVER, NULL, NULL);
    ret = usbh_submit_urb(&g_cdc_ncm_class.bulkout_urb);
    if (ret < 0) {
        usbh_cdc_ncm_set_link_state(&g_cdc_ncm_class, false);
        usb_osal_mutex_give(g_cdc_ncm_tx_mutex);
        return ERR_BUF;
    }

    usb_osal_mutex_give(g_cdc_ncm_tx_mutex);
    return ERR_OK;
}

__WEAK void usbh_cdc_ncm_run(struct usbh_cdc_ncm *cdc_ncm_class)
{
}

__WEAK void usbh_cdc_ncm_stop(struct usbh_cdc_ncm *cdc_ncm_class)
{
}

__WEAK void usbh_cdc_ncm_link_changed(struct usbh_cdc_ncm *cdc_ncm_class, bool state)
{
}

const struct usbh_class_driver cdc_ncm_class_driver = {
    .driver_name = "cdc_ncm",
    .connect = usbh_cdc_ncm_connect,
    .disconnect = usbh_cdc_ncm_disconnect
};

CLASS_INFO_DEFINE const struct usbh_class_info cdc_ncm_class_info = {
    .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
    .class = USB_DEVICE_CLASS_CDC,
    .subclass = CDC_NETWORK_CONTROL_MODEL,
    .protocol = CDC_COMMON_PROTOCOL_NONE,
    .vid = 0x00,
    .pid = 0x00,
    .class_driver = &cdc_ncm_class_driver
};
