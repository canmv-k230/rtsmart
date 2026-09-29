#include "usbd_desc.h"

#include "rtdef.h"
#include "rtthread.h"

#ifdef RT_USING_PUFS
#include "drv_pufs.h"
#endif

#define CANMV_USB_UID_SIZE                  32U
#define CANMV_USB_SERIAL_HEX_CHARS          (CANMV_USB_UID_SIZE * 2U)
#define CANMV_USB_SERIAL_DESCRIPTOR_SIZE    (2U + CANMV_USB_SERIAL_HEX_CHARS * 2U)
#define CANMV_USB_DEVICE_SERIAL_INDEX_OFFSET 16U

bool g_usb_device_connected = false;
static bool g_usb_device_registered = false;
static uint8_t g_canmv_usb_descriptor[sizeof(canmv_usb_descriptor) + CANMV_USB_SERIAL_DESCRIPTOR_SIZE];

static const uint8_t* find_string_descriptor(const uint8_t* descriptor, size_t size, uint8_t index)
{
    const uint8_t* end = descriptor + size;
    uint8_t        string_index = 0;

    while ((descriptor + 2U) <= end && descriptor[0] != 0U) {
        uint8_t length = descriptor[0];

        if (length < 2U || (descriptor + length) > end)
            return RT_NULL;
        if (descriptor[1] == USB_DESCRIPTOR_TYPE_STRING) {
            if (string_index == index)
                return descriptor;
            string_index++;
        }
        descriptor += length;
    }

    return RT_NULL;
}

static bool read_chip_uid(uint8_t uid[CANMV_USB_UID_SIZE])
{
#ifdef RT_USING_PUFS
    rt_device_t    pufs = rt_device_find("pufs");
    pufs_uid_get_t request;
    bool           nonzero = false;

    if (pufs == RT_NULL)
        return false;

    rt_memset(uid, 0, CANMV_USB_UID_SIZE);
    request.slot     = 0;
    request.uid_phys = (uint64_t)(uintptr_t)uid;
    if (rt_device_control(pufs, PUFS_UID_GET, &request) != RT_EOK)
        return false;

    for (size_t i = 0; i < CANMV_USB_UID_SIZE; i++)
        nonzero |= uid[i] != 0U;
    return nonzero;
#else
    (void)uid;
    return false;
#endif
}

static void prepare_usb_descriptor(void)
{
    static const char hex[] = "0123456789ABCDEF";
    const uint8_t* serial;
    uint8_t        uid[CANMV_USB_UID_SIZE];
    size_t         prefix_size;
    size_t         suffix_offset;

    serial = find_string_descriptor(canmv_usb_descriptor, sizeof(canmv_usb_descriptor), USB_STRING_SERIAL_INDEX);
    if (serial == RT_NULL) {
        rt_memcpy(g_canmv_usb_descriptor, canmv_usb_descriptor, sizeof(canmv_usb_descriptor));
        g_canmv_usb_descriptor[CANMV_USB_DEVICE_SERIAL_INDEX_OFFSET] = 0U;
        rt_kprintf("USB serial descriptor not found; serial disabled\n");
        return;
    }

    prefix_size  = (size_t)(serial - canmv_usb_descriptor);
    suffix_offset = prefix_size + serial[0];
    rt_memcpy(g_canmv_usb_descriptor, canmv_usb_descriptor, prefix_size);

    if (!read_chip_uid(uid)) {
        rt_memcpy(g_canmv_usb_descriptor + prefix_size, canmv_usb_descriptor + prefix_size,
                  sizeof(canmv_usb_descriptor) - prefix_size);
        g_canmv_usb_descriptor[CANMV_USB_DEVICE_SERIAL_INDEX_OFFSET] = 0U;
        rt_kprintf("PUFS UID unavailable; USB serial disabled\n");
        return;
    }

    g_canmv_usb_descriptor[prefix_size]     = CANMV_USB_SERIAL_DESCRIPTOR_SIZE;
    g_canmv_usb_descriptor[prefix_size + 1] = USB_DESCRIPTOR_TYPE_STRING;
    for (size_t i = 0; i < CANMV_USB_UID_SIZE; i++) {
        size_t output = prefix_size + 2U + i * 4U;

        g_canmv_usb_descriptor[output]     = (uint8_t)hex[uid[i] >> 4];
        g_canmv_usb_descriptor[output + 1] = 0U;
        g_canmv_usb_descriptor[output + 2] = (uint8_t)hex[uid[i] & 0x0fU];
        g_canmv_usb_descriptor[output + 3] = 0U;
    }
    rt_memcpy(g_canmv_usb_descriptor + prefix_size + CANMV_USB_SERIAL_DESCRIPTOR_SIZE,
              canmv_usb_descriptor + suffix_offset, sizeof(canmv_usb_descriptor) - suffix_offset);
    rt_kprintf("USB serial initialized from PUFS UID\n");
}

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
#if defined(CHERRY_USB_DEVICE_FUNC_CDC) || defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP) || defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP) || defined (CHERRY_USB_DEVICE_FUNC_CDC_ADB) || defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP_ADB)
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

    prepare_usb_descriptor();
    usbd_desc_register(USB_DEVICE_BUS_ID, g_canmv_usb_descriptor);

#if defined(CHERRY_USB_DEVICE_FUNC_CDC) || defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP) || defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP) || defined (CHERRY_USB_DEVICE_FUNC_CDC_ADB) || defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP_ADB)
    canmv_usb_device_cdc_init();
#endif // CHERRY_USB_DEVICE_FUNC_CDC

#if defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP) || defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP) || defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP_ADB)
    canmv_usb_device_mtp_init();
#endif // CHERRY_USB_DEVICE_FUNC_CDC_MTP

#if defined(CHERRY_USB_DEVICE_FUNC_HID) || defined(CHERRY_USB_DEVICE_FUNC_HID_CDC_MTP)
    canmv_usb_device_hid_init();
#endif

#if defined(CHERRY_USB_DEVICE_FUNC_UVC)
    canmv_usb_device_uvc_init();
#endif

#if defined (CHERRY_USB_DEVICE_FUNC_ADB) || defined(CHERRY_USB_DEVICE_FUNC_CDC_ADB) || defined(CHERRY_USB_DEVICE_FUNC_CDC_MTP_ADB)
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
