#include "usbd_desc.h"

#include "rtdef.h"
#include "rtthread.h"

bool g_usb_device_connected = false;
static bool g_usb_device_registered = false;

void board_usb_device_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;

    switch (event) {
        case USBD_EVENT_RESET:
            rt_kprintf("usb disconnect\n");
            /* fall through */
        case USBD_EVENT_DEINIT:
            g_usb_device_connected = false;
            break;
        case USBD_EVENT_CONFIGURED:
            g_usb_device_connected = true;
            break;
        default:
            return;
    }
#if defined(CHERRY_USB_DEVICE_FUNC_CDC) || defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP) || defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP) || defined (CHERRY_USB_DEVICE_FUNC_CDC_ADB)
    if (g_usb_device_connected) {
        canmv_usb_device_cdc_on_connected();
    } else {
        canmv_usb_device_cdc_on_disconnected();
    }
#endif

#if defined(CHERRY_USB_DEVICE_FUNC_HID) || defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP)
    if (g_usb_device_connected) {
        canmv_usb_device_hid_on_connected();
    } else {
        canmv_usb_device_hid_on_disconnected();
    }
#endif

#if defined(CHERRY_USB_DEVICE_FUNC_UVC)
    if (g_usb_device_connected) {
        canmv_usb_device_uvc_on_connected();
    } else {
        canmv_usb_device_uvc_on_disconnected();
    }
#endif
}

RT_WEAK int mtp_fs_db_valid(void) { return 0; }

void board_usb_device_register(void)
{
    if (g_usb_device_registered) {
        return;
    }

    usbd_desc_register(USB_DEVICE_BUS_ID, canmv_usb_descriptor);

#if defined(CHERRY_USB_DEVICE_FUNC_CDC) || defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP) || defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP) || defined (CHERRY_USB_DEVICE_FUNC_CDC_ADB)
    canmv_usb_device_cdc_init();
#endif // CHERRY_USB_DEVICE_FUNC_CDC

#if defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP) || defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP)
    canmv_usb_device_mtp_init();
#endif // CHERRY_USB_DEVICE_FUNC_CDC_MTP

#if defined(CHERRY_USB_DEVICE_FUNC_HID) || defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP)
    canmv_usb_device_hid_init();
#endif

#if defined(CHERRY_USB_DEVICE_FUNC_UVC)
    canmv_usb_device_uvc_init();
#endif

#if defined (CHERRY_USB_DEVICE_FUNC_ADB) || defined(CHERRY_USB_DEVICE_FUNC_CDC_ADB)
    canmv_usb_device_adb_init();
#endif

    g_usb_device_registered = true;
}

/*****************************************************************************/
void board_usb_device_init(void* usb_base)
{
    board_usb_device_register();
    usbd_initialize(USB_DEVICE_BUS_ID, (uint32_t)(uint64_t)usb_base, board_usb_device_event_handler);
}
