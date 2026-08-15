/*
 * Copyright (c) 2026, Canaan Bright Sight Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB Bluetooth HCI transport for AIC8800 combo devices.
 */
#include "aic8800_wifi.h"

#include <drivers/bt_hci.h>

#define DBG_TAG "aic8800.bt"
#define DBG_LVL AIC8800_DBG_LVL
#include <rtdbg.h>

/* The receive URBs must expire on their own.  usbh_kill_urb() cannot cancel a
 * request that a worker is about to submit, so an unbounded receive would let
 * that request outlive both the worker thread and its transfer buffer.  Keep
 * the stop timeout above this value so a racing submission always completes
 * before the buffers are released. */
#define AIC_BT_USB_RX_TIMEOUT_MS      1000U
#define AIC_BT_USB_TX_TIMEOUT_MS      1000U
#define AIC_BT_USB_STOP_TIMEOUT_MS    3000U
#define AIC_BT_USB_DMA_ALIGNMENT        64U
#define AIC_BT_USB_MAX_ERRORS             3U

struct aic_btusb_context;

struct aic_btusb_rx_worker
{
    struct aic_btusb_context *context;
    struct usb_endpoint_descriptor *endpoint;
    struct usbh_urb urb;
    struct rt_completion stopped;
    rt_thread_t thread;
    rt_uint8_t *transfer_buffer;
    rt_uint8_t *packet_buffer;
    rt_size_t packet_length;
    rt_uint8_t packet_type;
    rt_bool_t interrupt_endpoint;
    volatile rt_bool_t active;
    /* Set when the thread could not be joined.  Its URB may still reference
     * transfer_buffer, so neither the buffers nor the interface may be
     * reused. */
    rt_bool_t orphaned;
    const char *name;
};

struct aic_btusb_context
{
    struct rt_bt_hci_device hci;
    struct rt_mutex state_mutex;
    struct usbh_hubport *hport;
    rt_uint8_t interface_slot;
    rt_uint8_t interface_number;
    rt_uint16_t vendor_id;
    rt_uint16_t product_id;
    struct usb_endpoint_descriptor *event_in;
    struct usb_endpoint_descriptor *acl_in;
    struct usb_endpoint_descriptor *acl_out;
    struct usbh_urb tx_urb;
    struct aic_btusb_rx_worker event_worker;
    struct aic_btusb_rx_worker acl_worker;
    rt_uint8_t *tx_buffer;
    rt_uint8_t hci_rx_buffer[AIC8800_BT_HCI_RX_BUFFER_SIZE]
        __attribute__((aligned(RT_ALIGN_SIZE)));
    rt_bool_t registered;
    rt_bool_t connected;
    rt_bool_t opened;
};

static struct aic_btusb_context g_aic_btusb;

static rt_uint16_t aic_btusb_get_le16(const rt_uint8_t *data)
{
    return (rt_uint16_t)data[0] | ((rt_uint16_t)data[1] << 8);
}

static rt_err_t aic_btusb_usb_result(int result)
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

static rt_bool_t aic_btusb_rx_error_recoverable(int result)
{
    return result == -71 || result == -USB_ERR_STALL || result == -32 ||
           result == -USB_ERR_BUSY || result == -USB_ERR_NOMEM;
}

static int aic_btusb_clear_halt(struct aic_btusb_context *context,
                               rt_uint8_t endpoint)
{
    struct usb_setup_packet setup;

    if (!context || !context->hport || !context->hport->connected)
    {
        return -USB_ERR_NODEV;
    }
    rt_memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD |
                          USB_REQUEST_RECIPIENT_ENDPOINT;
    setup.bRequest = USB_REQUEST_CLEAR_FEATURE;
    setup.wValue = USB_FEATURE_ENDPOINT_HALT;
    setup.wIndex = endpoint;
    return usbh_control_transfer(context->hport, &setup, RT_NULL);
}

static rt_size_t aic_btusb_expected_length(rt_uint8_t packet_type,
                                           const rt_uint8_t *data,
                                           rt_size_t length)
{
    if (packet_type == RT_BT_HCI_H4_EVT)
    {
        return length >= 2U ? 2U + data[1] : 0U;
    }
    if (packet_type == RT_BT_HCI_H4_ACL)
    {
        return length >= 4U ? 4U + aic_btusb_get_le16(data + 2) : 0U;
    }
    return (rt_size_t)-1;
}

static void aic_btusb_feed_rx(struct aic_btusb_rx_worker *worker,
                              const rt_uint8_t *data, rt_size_t length)
{
    if (length > AIC8800_BT_USB_RX_BUFFER_SIZE - worker->packet_length)
    {
        LOG_W("%s HCI reassembly overflow", worker->name);
        worker->packet_length = 0;
        if (length > AIC8800_BT_USB_RX_BUFFER_SIZE)
        {
            return;
        }
    }
    rt_memcpy(worker->packet_buffer + worker->packet_length, data, length);
    worker->packet_length += length;

    while (worker->packet_length)
    {
        rt_size_t expected = aic_btusb_expected_length(
            worker->packet_type, worker->packet_buffer,
            worker->packet_length);

        if (!expected)
        {
            return;
        }
        if (expected == (rt_size_t)-1 ||
            expected > AIC8800_BT_USB_RX_BUFFER_SIZE)
        {
            LOG_W("invalid %s HCI packet length %u", worker->name,
                  (unsigned int)expected);
            worker->packet_length = 0;
            return;
        }
        if (worker->packet_length < expected)
        {
            return;
        }
        if (worker->context->opened && worker->context->connected)
        {
            rt_err_t result = rt_bt_hci_receive(
                &worker->context->hci, worker->packet_type,
                worker->packet_buffer, expected);

            if (result != RT_EOK && result != -RT_EFULL)
            {
                LOG_W("discarded %s HCI packet: %d", worker->name, result);
            }
        }
        worker->packet_length -= expected;
        if (worker->packet_length)
        {
            rt_memmove(worker->packet_buffer,
                       worker->packet_buffer + expected,
                       worker->packet_length);
        }
    }
}

static void aic_btusb_rx_thread(void *parameter)
{
    struct aic_btusb_rx_worker *worker = parameter;
    struct aic_btusb_context *context;
    rt_uint32_t errors = 0;

    if (!worker || !worker->context)
    {
        return;
    }
    context = worker->context;
    while (worker->active && context->connected && context->opened)
    {
        int result;

        rt_memset(&worker->urb, 0, sizeof(worker->urb));
        if (worker->interrupt_endpoint)
        {
            usbh_int_urb_fill(&worker->urb, context->hport,
                              worker->endpoint, worker->transfer_buffer,
                              AIC8800_BT_USB_RX_BUFFER_SIZE,
                              AIC_BT_USB_RX_TIMEOUT_MS, RT_NULL, RT_NULL);
        }
        else
        {
            usbh_bulk_urb_fill(&worker->urb, context->hport,
                               worker->endpoint, worker->transfer_buffer,
                               AIC8800_BT_USB_RX_BUFFER_SIZE,
                               AIC_BT_USB_RX_TIMEOUT_MS, RT_NULL, RT_NULL);
        }
        /* Re-check immediately before submission.  A request posted after the
         * teardown path has run its usbh_kill_urb() would not be cancelled. */
        if (!worker->active || !context->connected || !context->opened)
        {
            break;
        }
        result = usbh_submit_urb(&worker->urb);
        if (!worker->active || !context->connected || !context->opened)
        {
            break;
        }
        if (!result)
        {
            errors = 0;
            if (worker->urb.actual_length)
            {
                aic_btusb_feed_rx(worker, worker->transfer_buffer,
                                  worker->urb.actual_length);
            }
            continue;
        }
        if (aic8800_usb_is_timeout(result) || result == -USB_ERR_NAK)
        {
            if (result == -USB_ERR_NAK && worker->interrupt_endpoint)
            {
                rt_thread_mdelay(worker->endpoint->bInterval ?
                                 worker->endpoint->bInterval : 1U);
            }
            continue;
        }
        if (aic_btusb_rx_error_recoverable(result))
        {
            errors++;
            /* EPROTO is a transaction error, not ENDPOINT_HALT.  Clearing
             * halt for it performs a blocking EP0 transfer and can stall
             * Wi-Fi traffic on this composite USB device.
             */
            if (result == -USB_ERR_STALL || result == -32)
            {
                (void)aic_btusb_clear_halt(
                    context, worker->endpoint->bEndpointAddress);
            }
            if (errors <= 4U || !(errors & (errors - 1U)))
            {
                LOG_W("%s endpoint 0x%02x error %d; recovering (%u)",
                      worker->name, worker->endpoint->bEndpointAddress,
                      result, (unsigned int)errors);
            }
            rt_thread_mdelay(errors <= 3U ? 1U :
                             (errors <= 16U ? 5U : 20U));
            continue;
        }
        errors++;
        if (errors >= AIC_BT_USB_MAX_ERRORS)
        {
            LOG_E("%s endpoint 0x%02x stopped: %d", worker->name,
                  worker->endpoint->bEndpointAddress, result);
            break;
        }
    }
    worker->active = RT_FALSE;
    rt_completion_done(&worker->stopped);
}

static rt_err_t aic_btusb_start_worker(
    struct aic_btusb_rx_worker *worker, struct aic_btusb_context *context,
    struct usb_endpoint_descriptor *endpoint, rt_uint8_t packet_type,
    rt_bool_t interrupt_endpoint, const char *name)
{
    rt_err_t result;

    rt_memset(&worker->urb, 0, sizeof(worker->urb));
    rt_completion_init(&worker->stopped);
    worker->context = context;
    worker->endpoint = endpoint;
    worker->packet_type = packet_type;
    worker->interrupt_endpoint = interrupt_endpoint;
    worker->packet_length = 0;
    worker->name = name;
    worker->active = RT_TRUE;
    worker->thread = rt_thread_create(
        name, aic_btusb_rx_thread, worker,
        AIC8800_BT_RX_THREAD_STACK_SIZE,
        AIC8800_BT_RX_THREAD_PRIORITY, 10);
    if (!worker->thread)
    {
        worker->active = RT_FALSE;
        return -RT_ENOMEM;
    }
    result = rt_thread_startup(worker->thread);
    if (result != RT_EOK)
    {
        rt_thread_delete(worker->thread);
        worker->thread = RT_NULL;
        worker->active = RT_FALSE;
    }
    return result;
}

static void aic_btusb_stop_worker(struct aic_btusb_rx_worker *worker)
{
    if (!worker->thread)
    {
        worker->active = RT_FALSE;
        worker->packet_length = 0;
        return;
    }
    worker->active = RT_FALSE;
    usbh_kill_urb(&worker->urb);
    if (rt_completion_wait(
            &worker->stopped,
            rt_tick_from_millisecond(AIC_BT_USB_STOP_TIMEOUT_MS)) != RT_EOK)
    {
        /* The receive timeout is shorter than the stop timeout, so a request
         * which raced the cancellation has already expired.  Reaching this
         * point means the thread is wedged somewhere else; deleting it leaves
         * its URB owning transfer_buffer, so retain the buffers instead of
         * handing freed memory to the host controller. */
        LOG_E("RX thread %s did not stop after USB cancellation; "
              "orphaning it and retaining its buffers", worker->name);
        worker->orphaned = RT_TRUE;
        rt_thread_delete(worker->thread);
    }
    worker->thread = RT_NULL;
    worker->packet_length = 0;
}

static rt_err_t aic_btusb_start_rx(struct aic_btusb_context *context)
{
    rt_err_t result;

    result = aic_btusb_start_worker(
        &context->event_worker, context, context->event_in,
        RT_BT_HCI_H4_EVT, RT_TRUE, "aic-bte");
    if (result != RT_EOK)
    {
        return result;
    }
    result = aic_btusb_start_worker(
        &context->acl_worker, context, context->acl_in,
        RT_BT_HCI_H4_ACL, RT_FALSE, "aic-bta");
    if (result != RT_EOK)
    {
        aic_btusb_stop_worker(&context->event_worker);
    }
    return result;
}

static void aic_btusb_stop_rx(struct aic_btusb_context *context)
{
    aic_btusb_stop_worker(&context->acl_worker);
    aic_btusb_stop_worker(&context->event_worker);
}

static rt_err_t aic_btusb_hci_open(struct rt_bt_hci_device *hci)
{
    struct aic_btusb_context *context;
    rt_err_t result;

    if (!hci)
    {
        return -RT_EINVAL;
    }
    context = hci->parent.user_data;
    if (!context)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&context->state_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!context->connected)
    {
        result = -RT_EIO;
    }
    else if (context->opened)
    {
        result = RT_EOK;
    }
    else
    {
        context->opened = RT_TRUE;
        result = aic_btusb_start_rx(context);
        if (result != RT_EOK)
        {
            context->opened = RT_FALSE;
        }
    }
    rt_mutex_release(&context->state_mutex);
    return result;
}

static rt_err_t aic_btusb_hci_close(struct rt_bt_hci_device *hci)
{
    struct aic_btusb_context *context;
    rt_err_t result;

    if (!hci)
    {
        return -RT_EINVAL;
    }
    context = hci->parent.user_data;
    if (!context)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&context->state_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    context->opened = RT_FALSE;
    aic_btusb_stop_rx(context);
    rt_bt_hci_flush(hci);
    rt_mutex_release(&context->state_mutex);
    return RT_EOK;
}

static rt_err_t aic_btusb_send_command(struct aic_btusb_context *context,
                                       const rt_uint8_t *data,
                                       rt_size_t length)
{
    struct usb_setup_packet *setup;
    int result;

    if (!context || !context->connected || !context->hport ||
        !context->hport->connected || !context->tx_buffer ||
        !data || !length || !context->hport->setup ||
        length > AIC8800_BT_USB_RX_BUFFER_SIZE)
    {
        return -RT_EINVAL;
    }
    setup = context->hport->setup;
    rt_memcpy(context->tx_buffer, data, length);
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS |
                           USB_REQUEST_RECIPIENT_DEVICE;
    setup->bRequest = 0;
    setup->wValue = 0;
    setup->wIndex = 0;
    setup->wLength = (rt_uint16_t)length;
    result = usbh_control_transfer(context->hport, setup,
                                   context->tx_buffer);
    if (result < 0)
    {
        LOG_E("HCI command USB transfer failed: %d", result);
        return aic_btusb_usb_result(result);
    }
    /* The K230 DWC2 port includes the 8-byte setup stage in actual_length;
     * other CherryUSB HCDs report only the data stage. */
    if ((rt_size_t)result != length &&
        (rt_size_t)result != length + USB_SIZEOF_SETUP_PACKET)
    {
        LOG_E("unexpected HCI command USB length: %d (payload %u)", result,
              (unsigned int)length);
        return -RT_EIO;
    }
    return RT_EOK;
}

static rt_err_t aic_btusb_send_acl(struct aic_btusb_context *context,
                                   const rt_uint8_t *data, rt_size_t length)
{
    int result;

    if (!context || !context->connected || !context->hport ||
        !context->hport->connected || !context->acl_out ||
        !context->tx_buffer || !data || !length)
    {
        return -RT_EINVAL;
    }
    if (length > AIC8800_BT_USB_RX_BUFFER_SIZE)
    {
        return -RT_EFULL;
    }
    rt_memcpy(context->tx_buffer, data, length);
    rt_memset(&context->tx_urb, 0, sizeof(context->tx_urb));
    usbh_bulk_urb_fill(&context->tx_urb, context->hport, context->acl_out,
                       context->tx_buffer, length, AIC_BT_USB_TX_TIMEOUT_MS,
                       RT_NULL, RT_NULL);
    context->tx_urb.transfer_flags = URB_ZERO_PACKET;
    result = usbh_submit_urb(&context->tx_urb);
    if (!result && context->tx_urb.actual_length != length)
    {
        return -RT_EIO;
    }
    return aic_btusb_usb_result(result);
}

static rt_err_t aic_btusb_hci_send(struct rt_bt_hci_device *hci,
                                   rt_uint8_t packet_type,
                                   const rt_uint8_t *data,
                                   rt_size_t length)
{
    struct aic_btusb_context *context;
    rt_err_t result;

    if (!hci)
    {
        return -RT_EINVAL;
    }
    context = hci->parent.user_data;
    if (!context)
    {
        return -RT_EINVAL;
    }
    if (!data || !length)
    {
        return -RT_EINVAL;
    }
    result = rt_mutex_take(&context->state_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return result;
    }
    if (!context->connected || !context->opened)
    {
        result = -RT_EIO;
    }
    else if (packet_type == RT_BT_HCI_H4_CMD)
    {
        result = aic_btusb_send_command(context, data, length);
    }
    else if (packet_type == RT_BT_HCI_H4_ACL)
    {
        result = aic_btusb_send_acl(context, data, length);
    }
    else
    {
        result = -RT_ENOSYS;
    }
    rt_mutex_release(&context->state_mutex);
    return result;
}

static const struct rt_bt_hci_ops g_aic_btusb_hci_ops = {
    .open = aic_btusb_hci_open,
    .close = aic_btusb_hci_close,
    .send = aic_btusb_hci_send,
};

static rt_err_t aic_btusb_allocate_buffers(struct aic_btusb_context *context)
{
    context->tx_buffer = rt_malloc_align(
        AIC8800_BT_USB_RX_BUFFER_SIZE, AIC_BT_USB_DMA_ALIGNMENT);
    context->event_worker.transfer_buffer = rt_malloc_align(
        AIC8800_BT_USB_RX_BUFFER_SIZE, AIC_BT_USB_DMA_ALIGNMENT);
    context->event_worker.packet_buffer = rt_malloc(
        AIC8800_BT_USB_RX_BUFFER_SIZE);
    context->acl_worker.transfer_buffer = rt_malloc_align(
        AIC8800_BT_USB_RX_BUFFER_SIZE, AIC_BT_USB_DMA_ALIGNMENT);
    context->acl_worker.packet_buffer = rt_malloc(
        AIC8800_BT_USB_RX_BUFFER_SIZE);
    if (!context->tx_buffer || !context->event_worker.transfer_buffer ||
        !context->event_worker.packet_buffer ||
        !context->acl_worker.transfer_buffer ||
        !context->acl_worker.packet_buffer)
    {
        return -RT_ENOMEM;
    }
    return RT_EOK;
}

static void aic_btusb_free_worker_buffers(
    struct aic_btusb_rx_worker *worker)
{
    if (worker->orphaned)
    {
        /* A deleted thread's URB can still reference transfer_buffer. */
        return;
    }
    if (worker->packet_buffer)
    {
        rt_free(worker->packet_buffer);
        worker->packet_buffer = RT_NULL;
    }
    if (worker->transfer_buffer)
    {
        rt_free_align(worker->transfer_buffer);
        worker->transfer_buffer = RT_NULL;
    }
}

static void aic_btusb_free_buffers(struct aic_btusb_context *context)
{
    aic_btusb_free_worker_buffers(&context->acl_worker);
    aic_btusb_free_worker_buffers(&context->event_worker);
    if (context->tx_buffer)
    {
        rt_free_align(context->tx_buffer);
        context->tx_buffer = RT_NULL;
    }
}

static int aic_btusb_connect(struct usbh_hubport *hport, rt_uint8_t intf)
{
    struct aic_btusb_context *context = &g_aic_btusb;
    struct usbh_interface_altsetting *setting;
    rt_err_t result;
    rt_uint8_t index;

    /* The HCI device is registered by aic8800_usb_component_init() before
     * USB host enumeration starts.  Do not register it from this callback: CherryUSB
     * invokes class connect from the hub thread and the device registration
     * path is too deep for that thread's stack. */
    if (!context->registered)
    {
        LOG_E("Bluetooth HCI is not registered before USB connect");
        return -USB_ERR_BUSY;
    }
    result = rt_mutex_take(&context->state_mutex, RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        return -USB_ERR_BUSY;
    }
    if (context->connected)
    {
        rt_mutex_release(&context->state_mutex);
        return -USB_ERR_BUSY;
    }
    if (context->event_worker.orphaned || context->acl_worker.orphaned)
    {
        /* An earlier teardown left a receive request owning its buffer.  The
         * interface cannot be reused without handing that request's memory to
         * a second worker. */
        LOG_E("refusing Bluetooth reconnect while a previous receive worker is still live");
        rt_mutex_release(&context->state_mutex);
        return -USB_ERR_BUSY;
    }

    context->hport = hport;
    context->interface_slot = intf;
    context->interface_number =
        hport->config.intf[intf].altsetting[0].intf_desc.bInterfaceNumber;
    context->vendor_id = hport->device_desc.idVendor;
    context->product_id = hport->device_desc.idProduct;
    setting = &hport->config.intf[intf].altsetting[0];
    if (context->interface_number != 0)
    {
        LOG_E("%04x:%04x HCI interface number %u is unsupported",
              context->vendor_id, context->product_id,
              context->interface_number);
        result = -RT_EIO;
        goto fail;
    }
    for (index = 0; index < setting->intf_desc.bNumEndpoints; index++)
    {
        struct usb_endpoint_descriptor *endpoint =
            &setting->ep[index].ep_desc;
        rt_uint8_t type = USB_GET_ENDPOINT_TYPE(endpoint->bmAttributes);

        if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK) &&
            type == USB_ENDPOINT_TYPE_INTERRUPT && !context->event_in)
        {
            USBH_EP_INIT(context->event_in, endpoint);
        }
        else if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK) &&
                 type == USB_ENDPOINT_TYPE_BULK && !context->acl_in)
        {
            USBH_EP_INIT(context->acl_in, endpoint);
        }
        else if (!(endpoint->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK) &&
                 type == USB_ENDPOINT_TYPE_BULK && !context->acl_out)
        {
            USBH_EP_INIT(context->acl_out, endpoint);
        }
    }
    if (!context->event_in || !context->acl_in || !context->acl_out)
    {
        LOG_E("%04x:%04x interface %u lacks HCI endpoints",
              context->vendor_id, context->product_id, intf);
        result = -RT_EIO;
        goto fail;
    }
    result = aic_btusb_allocate_buffers(context);
    if (result != RT_EOK)
    {
        goto fail;
    }
    context->connected = RT_TRUE;
    hport->config.intf[intf].priv = context;
    rt_snprintf(hport->config.intf[intf].devname,
                CONFIG_USBHOST_DEV_NAMELEN, "aic-bt");
    LOG_I("AIC Bluetooth connected %04x:%04x; HCI=/dev/%s",
          context->vendor_id, context->product_id,
          context->hci.parent.parent.name);
    rt_mutex_release(&context->state_mutex);
    return 0;

fail:
    aic_btusb_free_buffers(context);
    context->hport = RT_NULL;
    context->event_in = RT_NULL;
    context->acl_in = RT_NULL;
    context->acl_out = RT_NULL;
    rt_mutex_release(&context->state_mutex);
    return result == -RT_ENOMEM ? -USB_ERR_NOMEM : -USB_ERR_NODEV;
}

static int aic_btusb_disconnect(struct usbh_hubport *hport, rt_uint8_t intf)
{
    struct aic_btusb_context *context;
    rt_uint16_t vendor_id;
    rt_uint16_t product_id;

    if (!hport)
    {
        return 0;
    }
    context = hport->config.intf[intf].priv;
    if (!context)
    {
        return 0;
    }
    if (rt_mutex_take(&context->state_mutex, RT_WAITING_FOREVER) != RT_EOK)
    {
        return -USB_ERR_BUSY;
    }
    vendor_id = context->vendor_id;
    product_id = context->product_id;
    context->opened = RT_FALSE;
    context->connected = RT_FALSE;
    usbh_kill_urb(&context->tx_urb);
    aic_btusb_stop_rx(context);
    rt_bt_hci_flush(&context->hci);
    aic_btusb_free_buffers(context);
    hport->config.intf[intf].priv = RT_NULL;
    context->hport = RT_NULL;
    context->event_in = RT_NULL;
    context->acl_in = RT_NULL;
    context->acl_out = RT_NULL;
    context->vendor_id = 0;
    context->product_id = 0;
    rt_mutex_release(&context->state_mutex);
    LOG_I("AIC Bluetooth disconnected %04x:%04x", vendor_id, product_id);
    return 0;
}

static const struct usbh_class_driver g_aic_btusb_class_driver = {
    .driver_name = "aic8800-btusb",
    .connect = aic_btusb_connect,
    .disconnect = aic_btusb_disconnect,
};

#define AIC_BTUSB_CLASS_INFO(_name, _vid, _pid)                            \
    CLASS_INFO_DEFINE const struct usbh_class_info _name = {               \
        .match_flags = USB_CLASS_MATCH_VENDOR | USB_CLASS_MATCH_PRODUCT |  \
                       USB_CLASS_MATCH_INTF_CLASS |                         \
                       USB_CLASS_MATCH_INTF_SUBCLASS |                      \
                       USB_CLASS_MATCH_INTF_PROTOCOL,                       \
        .class = 0xe0,                                                      \
        .subclass = 0x01,                                                   \
        .protocol = 0x01,                                                   \
        .vid = (_vid),                                                      \
        .pid = (_pid),                                                      \
        .class_driver = &g_aic_btusb_class_driver,                          \
    }

AIC_BTUSB_CLASS_INFO(g_aic_btusb_8801,
                     AIC8800_USB_VENDOR_ID, AIC8800_USB_PID_AIC8801);
AIC_BTUSB_CLASS_INFO(g_aic_btusb_88dc,
                     AIC8800_USB_VENDOR_ID, AIC8800_USB_PID_AIC8800DC);
AIC_BTUSB_CLASS_INFO(g_aic_btusb_8d81,
                     AIC8800_USB_VENDOR_ID, AIC8800_USB_PID_AIC8800D81);
AIC_BTUSB_CLASS_INFO(g_aic_btusb_8d41,
                     AIC8800_USB_VENDOR_ID, AIC8800_USB_PID_AIC8800D41);
AIC_BTUSB_CLASS_INFO(g_aic_btusb_8d99,
                     AIC8800_USB_VENDOR_ID_V2,
                     AIC8800_USB_PID_AIC8800D89X2);
AIC_BTUSB_CLASS_INFO(g_aic_btusb_88dd,
                     AIC8800_USB_VENDOR_ID, AIC8800_USB_PID_AIC8800DW);
AIC_BTUSB_CLASS_INFO(g_aic_btusb_8d91,
                     AIC8800_USB_VENDOR_ID_V2,
                     AIC8800_USB_PID_AIC8800D81X2);

rt_err_t aic8800_btusb_driver_init(void)
{
    struct aic_btusb_context *context = &g_aic_btusb;
    rt_err_t result;

    if (context->registered)
    {
        return RT_EOK;
    }
    result = rt_mutex_init(&context->state_mutex, "aic-bt",
                           RT_IPC_FLAG_PRIO);
    if (result != RT_EOK)
    {
        return result;
    }
    result = rt_bt_hci_register_auto(
        &context->hci, &g_aic_btusb_hci_ops, context->hci_rx_buffer,
        sizeof(context->hci_rx_buffer), context);
    if (result != RT_EOK)
    {
        rt_mutex_detach(&context->state_mutex);
        return result;
    }
    context->registered = RT_TRUE;
    LOG_I("Bluetooth HCI registered as /dev/%s",
          context->hci.parent.parent.name);
    return RT_EOK;
}
