#include "usbd_desc.h"

#include "rtdef.h"
#include "rtthread.h"

bool g_usb_device_connected = false;

static void event_handler(uint8_t busid, uint8_t event)
{
    if (event == USBD_EVENT_RESET) {
        g_usb_device_connected = false;

        rt_kprintf("usb disconnect\n");
    } else if (event == USBD_EVENT_CONFIGURED) {
        g_usb_device_connected = true;

#if defined(CHERRY_USB_DEVICE_FUNC_CDC) || defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP)
        canmv_usb_device_cdc_on_connected();
#endif

#if defined(CHERRY_USB_DEVICE_FUNC_UVC)
        canmv_usb_device_uvc_on_connected();
#endif
    }
}

RT_WEAK int mtp_fs_db_valid(void) { return 0; }

/*****************************************************************************/
void board_usb_device_init(void* usb_base)
{
    usbd_desc_register(USB_DEVICE_BUS_ID, canmv_usb_descriptor);

#if defined(CHERRY_USB_DEVICE_FUNC_CDC) || defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP)
    canmv_usb_device_cdc_init();
#endif // CHERRY_USB_DEVICE_FUNC_CDC

#if defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP)
    canmv_usb_device_mtp_init();
#endif // CHERRY_USB_DEVICE_FUNC_CDC_MTP

#if defined(CHERRY_USB_DEVICE_FUNC_UVC)
    canmv_usb_device_uvc_init();
#endif

    usbd_initialize(USB_DEVICE_BUS_ID, (uint32_t)(uint64_t)usb_base, event_handler);
}
