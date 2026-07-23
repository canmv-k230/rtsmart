/*
 * Copyright (c) 2022, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbd_desc.h"

#include "mtp.h"
#include "mtp_helpers.h"

#include "usb_osal.h"

#if defined (CHERRY_USB_DEVICE_FUNC_CDC_MTP) || defined (CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP)
/* Max USB packet size */
#define MTP_BULK_EP_MPS USB_DEVICE_MAX_MPS

#define MTP_OUT_EP_IDX 0
#define MTP_IN_EP_IDX  1
#define MTP_INT_EP_IDX 2

/* Describe EndPoints configuration */
static struct usbd_endpoint mtp_ep_data[3];
static mtp_ctx *mtp_context = NULL;
static usb_osal_thread_t mtp_tid;
static rt_event_t mtp_event;
static volatile uint32_t read_size;
static volatile uint32_t write_size;
static volatile uint32_t int_write_size;
#define EV_CONFIGURED 0x01
#define EV_DISCONNECT 0x02
#define EV_BULK_READ_FINISH 0x04
#define EV_BULK_WRITE_FINISH 0x08
#define EV_INT_WRITE_FINISH 0x10
#define EV_CANCEL 0x20

#define MTP_CANCEL_REQUEST_SIZE 6U
#define MTP_DEVICE_STATUS_SIZE 4U
#define MTP_CANCEL_CODE 0x4001U
static uint32_t mtp_device_status;

RT_WEAK int bank_voltage_check_is_failed(void) { return 0; }

static uint32_t mtp_bulk_transfer_limit(void)
{
    uint32_t limit;

    limit = MTP_BULK_EP_MPS * MTP_USB_BULK_MAX_PACKET_COUNT;
    if (limit > MTP_USB_BULK_MAX_TRANSFER_SIZE) {
        limit = MTP_USB_BULK_MAX_TRANSFER_SIZE;
    }
    limit -= limit % MTP_BULK_EP_MPS;

    return limit;
}

static void mtp_clear_transport_events(rt_uint32_t events)
{
    rt_uint32_t ignored;

    if (mtp_event) {
        rt_event_recv(mtp_event, events, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_NO, &ignored);
    }
}

static void mtp_reopen_endpoint(uint8_t endpoint, uint8_t attributes, uint16_t max_packet_size, uint8_t interval)
{
    struct usb_endpoint_descriptor ep = {
        .bLength = USB_SIZEOF_ENDPOINT_DESC,
        .bDescriptorType = USB_DESCRIPTOR_TYPE_ENDPOINT,
        .bEndpointAddress = endpoint,
        .bmAttributes = attributes,
        .wMaxPacketSize = max_packet_size,
        .bInterval = interval,
    };

    int close_result;
    int open_result;

    close_result = usbd_ep_close(USB_DEVICE_BUS_ID, endpoint);
    open_result = usbd_ep_open(USB_DEVICE_BUS_ID, &ep);
    if (close_result < 0 || open_result < 0) {
        USB_LOG_ERR("MTP endpoint 0x%02x reset failed\r\n", endpoint);
    }
}

static void mtp_abort_endpoints(void)
{
    if (!usb_device_is_configured(USB_DEVICE_BUS_ID)) {
        return;
    }

    /* Drop the active transfer before the responder starts the next one. */
    mtp_reopen_endpoint(mtp_ep_data[MTP_OUT_EP_IDX].ep_addr, USB_ENDPOINT_TYPE_BULK, MTP_BULK_EP_MPS, 0);
    mtp_reopen_endpoint(mtp_ep_data[MTP_IN_EP_IDX].ep_addr, USB_ENDPOINT_TYPE_BULK, MTP_BULK_EP_MPS, 0);
    read_size = 0;
    write_size = 0;
}

static void mtp_request_cancel(void)
{
    if (mtp_event) {
        rt_event_send(mtp_event, EV_CANCEL);
    }
}

static uint16_t mtp_get_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t mtp_get_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void mtp_set_device_status(uint8_t **data, uint32_t *len, uint16_t code)
{
    uint8_t *status = (uint8_t *)&mtp_device_status;

    status[0] = MTP_DEVICE_STATUS_SIZE;
    status[1] = 0;
    status[2] = code & 0xffU;
    status[3] = code >> 8;
    *data = status;
    *len = MTP_DEVICE_STATUS_SIZE;
}

int read_usb(void * ctx, unsigned char * buffer, int maxsize)
{
    rt_uint32_t re;

    (void)ctx;

    if (!mtp_event || !buffer || maxsize <= 0 || !g_usb_device_connected || !usb_device_is_configured(USB_DEVICE_BUS_ID)) {
        return -1;
    }
    if ((uint32_t)maxsize > mtp_bulk_transfer_limit()) {
        maxsize = (int)mtp_bulk_transfer_limit();
    }
    if (mtp_context && mtp_context->cancel_req) {
        mtp_abort_endpoints();
        return -2;
    }

    mtp_clear_transport_events(EV_BULK_READ_FINISH | EV_CANCEL | EV_DISCONNECT);
    if (mtp_context && mtp_context->cancel_req) {
        mtp_abort_endpoints();
        return -2;
    }
    read_size = 0;
    if (usbd_ep_start_read(USB_DEVICE_BUS_ID, mtp_ep_data[MTP_OUT_EP_IDX].ep_addr, buffer, maxsize) < 0) {
        return -1;
    }
    if (rt_event_recv(mtp_event, EV_BULK_READ_FINISH | EV_DISCONNECT | EV_CANCEL, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, &re) != RT_EOK) {
        return -1;
    }
    if (re & EV_DISCONNECT)
        return -1;
    if (re & EV_CANCEL) {
        mtp_abort_endpoints();
        return -2;
    }

    return read_size;
}

int write_usb(void * ctx, int channel, unsigned char * buffer, int size)
{
    rt_uint32_t re;
    volatile uint32_t *transfer_size;
    rt_uint32_t complete_event;
    rt_uint32_t wait_events;
    int is_bulk_write;

    (void)ctx;

    if (!mtp_event || channel < MTP_IN_EP_IDX || channel > MTP_INT_EP_IDX || (!buffer && size) || size < 0 || !g_usb_device_connected || !usb_device_is_configured(USB_DEVICE_BUS_ID)) {
        return -1;
    }
    is_bulk_write = channel == MTP_IN_EP_IDX;
    if (is_bulk_write && (uint32_t)size > mtp_bulk_transfer_limit()) {
        return -1;
    }
    if (is_bulk_write && mtp_context && mtp_context->cancel_req) {
        mtp_abort_endpoints();
        return -2;
    }

    if (channel == MTP_IN_EP_IDX) {
        transfer_size = &write_size;
        complete_event = EV_BULK_WRITE_FINISH;
    } else {
        transfer_size = &int_write_size;
        complete_event = EV_INT_WRITE_FINISH;
    }

    wait_events = complete_event | EV_DISCONNECT;
    if (is_bulk_write) {
        wait_events |= EV_CANCEL;
    }
    mtp_clear_transport_events(wait_events);
    if (is_bulk_write && mtp_context && mtp_context->cancel_req) {
        mtp_abort_endpoints();
        return -2;
    }
    *transfer_size = 0;
    if (usbd_ep_start_write(USB_DEVICE_BUS_ID, mtp_ep_data[channel].ep_addr, buffer, size) < 0) {
        return -1;
    }
    if (rt_event_recv(mtp_event, wait_events, RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, &re) != RT_EOK) {
        return -1;
    }
    if (re & EV_DISCONNECT)
        return -1;
    if (is_bulk_write && (re & EV_CANCEL)) {
        mtp_abort_endpoints();
        return -2;
    }

    return *transfer_size;
}

int mtp_fs_db_valid(void)
{
    if (!mtp_context)
        return 0;
    return mtp_context->fs_db ? 1 : 0;
}

static void mtp_thread(void *arg)
{
    mtp_ctx *ctx = arg;

    if (!ctx) {
        return;
    }

    while (1) {
        rt_event_control(mtp_event, RT_IPC_CMD_RESET, NULL);
        rt_event_recv(mtp_event, EV_CONFIGURED, RT_EVENT_FLAG_AND | RT_EVENT_FLAG_CLEAR, RT_WAITING_FOREVER, NULL);
        rt_event_control(mtp_event, RT_IPC_CMD_RESET, NULL);
        while (1) {
            int ret = mtp_incoming_packet(ctx);
            if (ret) {
                int reset_session = ret == -2;

                pthread_mutex_lock(&ctx->inotify_mutex);
                mtp_fs_db_session_end(ctx);
                if (ctx->fs_db) {
                    deinit_fs_db(ctx->fs_db);
                    ctx->fs_db = 0;
                }
                ctx->session_id = 0;
                ctx->SendObjInfoHandle = 0xFFFFFFFFU;
                ctx->SendObjInfoSize = 0;
                ctx->SendObjInfoOffset = 0;
                ctx->SetObjectPropValue_Handle = 0xFFFFFFFFU;
                ctx->SetObjectPropValue_PropCode = 0;
                ctx->pending_data_operation = 0;
                ctx->pending_data_transaction_id = 0;
                ctx->cancel_req = 0;
                ctx->cancel_status_pending = 0;
                ctx->reset_req = 0;
                ctx->transaction_active = 0;
                ctx->active_transaction_id = 0;
                ctx->transferring_file_data = 0;
                pthread_mutex_unlock(&ctx->inotify_mutex);

                if (reset_session) {
                    continue;
                }
                break;
            }
        }
    }
}

static int mtp_class_interface_request_handler(uint8_t busid, struct usb_setup_packet *setup, uint8_t **data, uint32_t *len)
{
    (void)busid;

    if (!setup || !data || !len) {
        return -1;
    }

    USB_LOG_DBG("MTP Class request: "
                "bRequest 0x%02x\r\n",
                setup->bRequest);

    switch (setup->bRequest) {
        case MTP_REQUEST_CANCEL:
            if ((setup->bmRequestType & USB_REQUEST_DIR_MASK) != USB_REQUEST_DIR_OUT || setup->wLength != MTP_CANCEL_REQUEST_SIZE || !mtp_context || !*data) {
                return -1;
            }
            if (mtp_get_le16(*data) != MTP_CANCEL_CODE) {
                return -1;
            }
            if (mtp_context->transaction_active && mtp_context->active_transaction_id == mtp_get_le32(*data + 2)) {
                mtp_context->cancel_req = 1;
                mtp_context->cancel_status_pending = 1;
                mtp_request_cancel();
            }
            *len = 0;
            break;
        case MTP_REQUEST_GET_EXT_EVENT_DATA:
            if ((setup->bmRequestType & USB_REQUEST_DIR_MASK) != USB_REQUEST_DIR_IN) {
                return -1;
            }
            *len = 0;
            break;
        case MTP_REQUEST_RESET:
            if ((setup->bmRequestType & USB_REQUEST_DIR_MASK) != USB_REQUEST_DIR_OUT || setup->wLength != 0 || !mtp_context) {
                return -1;
            }
            mtp_context->cancel_req = 1;
            mtp_context->cancel_status_pending = 0;
            mtp_context->reset_req = 1;
            mtp_request_cancel();
            *len = 0;
            break;
        case MTP_REQUEST_GET_DEVICE_STATUS:
            if ((setup->bmRequestType & USB_REQUEST_DIR_MASK) != USB_REQUEST_DIR_IN || setup->wLength < MTP_DEVICE_STATUS_SIZE) {
                return -1;
            }
            if (mtp_context && mtp_context->cancel_status_pending) {
                mtp_set_device_status(data, len, MTP_RESPONSE_TRANSACTION_CANCELLED);
                mtp_context->cancel_status_pending = 0;
            } else {
                mtp_set_device_status(data, len, MTP_RESPONSE_OK);
            }
            break;

        default:
            USB_LOG_WRN("Unhandled MTP Class bRequest 0x%02x\r\n", setup->bRequest);
            return -1;
    }

    return 0;
}

static void usbd_mtp_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;

    read_size = nbytes;
    if (mtp_event) {
        rt_event_send(mtp_event, EV_BULK_READ_FINISH);
    }
}

static void usbd_mtp_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;

    write_size = nbytes;
    if (mtp_event) {
        rt_event_send(mtp_event, EV_BULK_WRITE_FINISH);
    }
}

static void usbd_mtp_int_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;

    int_write_size = nbytes;
    if (mtp_event) {
        rt_event_send(mtp_event, EV_INT_WRITE_FINISH);
    }
}

static void mtp_notify_handler(uint8_t busid, uint8_t event, void *arg)
{
    (void)busid;
    (void)arg;

    switch (event) {
        case USBD_EVENT_RESET:
        case USBD_EVENT_DEINIT:
            read_size = 0;
            write_size = 0;
            int_write_size = 0;
            if (mtp_event) {
                rt_event_send(mtp_event, EV_DISCONNECT);
            }
            break;
        case USBD_EVENT_CONFIGURED:
            if (mtp_context) {
                mtp_context->cancel_req = 0;
                mtp_context->cancel_status_pending = 0;
                mtp_context->reset_req = 0;
                mtp_context->transaction_active = 0;
                mtp_context->active_transaction_id = 0;
                mtp_context->pending_data_operation = 0;
                mtp_context->pending_data_transaction_id = 0;
                mtp_context->SendObjInfoHandle = 0xFFFFFFFFU;
                mtp_context->SendObjInfoSize = 0;
                mtp_context->SendObjInfoOffset = 0;
                mtp_context->SetObjectPropValue_Handle = 0xFFFFFFFFU;
                mtp_context->SetObjectPropValue_PropCode = 0;
            }
            if (mtp_event) {
                rt_event_send(mtp_event, EV_CONFIGURED);
            }
            break;
        default:
            break;
    }
}

static int init_usb_mtp_buffer(mtp_ctx * ctx)
{
    if(ctx->wrbuffer)
    {
        free(ctx->wrbuffer);
        ctx->wrbuffer = NULL;
    }

    ctx->wrbuffer = malloc( ctx->usb_wr_buffer_max_size );
    if(!ctx->wrbuffer)
        goto init_error;

    memset(ctx->wrbuffer,0,ctx->usb_wr_buffer_max_size);

    if(ctx->rdbuffer)
    {
        free(ctx->rdbuffer);
        ctx->rdbuffer = NULL;
    }

    ctx->rdbuffer = malloc( ctx->usb_rd_buffer_max_size );
    if(!ctx->rdbuffer)
        goto init_error;

    memset(ctx->rdbuffer,0,ctx->usb_rd_buffer_max_size);

    if(ctx->rdbuffer2)
    {
        free(ctx->rdbuffer2);
        ctx->rdbuffer2 = NULL;
    }

    ctx->rdbuffer2 = malloc( ctx->usb_rd_buffer_max_size );
    if(!ctx->rdbuffer2)
        goto init_error;

    memset(ctx->rdbuffer2,0,ctx->usb_rd_buffer_max_size);

    return 0;
init_error:
    USB_LOG_ERR("init_usb_mtp_gadget init error !");

    if(ctx->wrbuffer)
    {
        free(ctx->wrbuffer);
        ctx->wrbuffer = NULL;
    }

    if(ctx->rdbuffer)
    {
        free(ctx->rdbuffer);
        ctx->rdbuffer = NULL;
    }

    if(ctx->rdbuffer2)
    {
        free(ctx->rdbuffer2);
        ctx->rdbuffer2 = NULL;
    }

    return -1;
}

static void mtp_device_init(void)
{
    mtp_context = mtp_init_responder();
    if (!mtp_context) {
        USB_LOG_ERR("MTP responder initialization failed\r\n");
        return;
    }

    mtp_load_config_file(mtp_context, "/bin/mtp.conf");
    if (init_usb_mtp_buffer(mtp_context)) {
        goto init_error;
    }
    mtp_set_usb_handle(mtp_context, NULL, MTP_BULK_EP_MPS);

    extern bool g_fs_mount_data_succ;
    extern bool g_fs_mount_sdcard_succ;

    if (bank_voltage_check_is_failed()) {
        mtp_add_storage(mtp_context, "/tmp", "ERROR_BANK_VOL", 0, 0, UMTP_STORAGE_READWRITE);
    } else {
        if(g_fs_mount_sdcard_succ) {
            mtp_add_storage(mtp_context, "/sdcard", "sdcard", 0, 0, UMTP_STORAGE_READWRITE);
        }

        if(g_fs_mount_data_succ) {
            mtp_add_storage(mtp_context, "/data", "data", 0, 0, UMTP_STORAGE_READWRITE);
        }

        if(!g_fs_mount_sdcard_succ && !g_fs_mount_data_succ) {
            mtp_add_storage(mtp_context, "/tmp", "ERROR", 0, 0, UMTP_STORAGE_READWRITE);
        }
    }

    mtp_event = rt_event_create("mtp", RT_IPC_FLAG_FIFO);
    if (!mtp_event) {
        USB_LOG_ERR("MTP event allocation failed\r\n");
        goto init_error;
    }

    mtp_tid = rt_thread_create("mtp", mtp_thread, mtp_context, CONFIG_USBDEV_MTP_STACKSIZE, CONFIG_USBDEV_MTP_PRIO, 10);
    if (!mtp_tid) {
        USB_LOG_ERR("MTP thread allocation failed\r\n");
        goto init_error;
    }

    if (rt_thread_startup((rt_thread_t)mtp_tid) != RT_EOK) {
        USB_LOG_ERR("MTP thread startup failed\r\n");
        rt_thread_delete((rt_thread_t)mtp_tid);
        mtp_tid = NULL;
        goto init_error;
    }

    return;

init_error:
    if (mtp_tid) {
        rt_thread_delete((rt_thread_t)mtp_tid);
        mtp_tid = NULL;
    }
    if (mtp_event) {
        rt_event_delete(mtp_event);
        mtp_event = NULL;
    }
    mtp_deinit_responder(mtp_context);
    mtp_context = NULL;
}

static struct usbd_interface usbd_mtp_intf;

void canmv_usb_device_mtp_init(void)
{
    usbd_mtp_intf.class_interface_handler = mtp_class_interface_request_handler;
    usbd_mtp_intf.class_endpoint_handler = NULL;
    usbd_mtp_intf.vendor_handler = NULL;
    usbd_mtp_intf.notify_handler = mtp_notify_handler;

    mtp_ep_data[MTP_OUT_EP_IDX].ep_addr = MTP_OUT_EP;
    mtp_ep_data[MTP_OUT_EP_IDX].ep_cb = usbd_mtp_bulk_out;
    mtp_ep_data[MTP_IN_EP_IDX].ep_addr = MTP_IN_EP;
    mtp_ep_data[MTP_IN_EP_IDX].ep_cb = usbd_mtp_bulk_in;
    mtp_ep_data[MTP_INT_EP_IDX].ep_addr = MTP_INT_EP;
    mtp_ep_data[MTP_INT_EP_IDX].ep_cb = usbd_mtp_int_in;

    usbd_add_interface(USB_DEVICE_BUS_ID, &usbd_mtp_intf);

    usbd_add_endpoint(USB_DEVICE_BUS_ID, &mtp_ep_data[MTP_OUT_EP_IDX]);
    usbd_add_endpoint(USB_DEVICE_BUS_ID, &mtp_ep_data[MTP_IN_EP_IDX]);
    usbd_add_endpoint(USB_DEVICE_BUS_ID, &mtp_ep_data[MTP_INT_EP_IDX]);

    mtp_device_init();
}
#endif
