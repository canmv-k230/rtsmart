#include <rthw.h>
#include <rtthread.h>

#include "rt_input_event.h"
#include "usbh_core.h"
#include "usbh_hid.h"
#include "usbh_hid_report_parser.h"

#define HID_POINTER_BUTTON_MASK 0x1f

enum hid_device_type
{
    HID_DEV_KEYBOARD = 0,
    HID_DEV_MOUSE,
    HID_DEV_TOUCH,
};

static const unsigned char usb_kbd_keycode[256] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
    115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
    122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
    150,158,159,128,136,177,178,176,142,152,173,140
};

struct hid_device
{
    struct rt_input_dev input;
    struct usbh_hid *hid_class;
    enum hid_device_type type;
    rt_bool_t use_boot_protocol;
    rt_bool_t registered;
    rt_uint8_t last_report[8];
    rt_uint8_t last_buttons;
    struct hid_report_map report_map;
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_kbd_buffer[128];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t hid_pointer_buffer[128];

static struct hid_device hid_kbd_dev;
static struct hid_device hid_pointer_dev;

static void usbh_hid_kbd_callback(void *arg, int nbytes);
static void usbh_hid_pointer_callback(void *arg, int nbytes);

static void hid_prepare_device(struct hid_device *hid)
{
    if (!hid->registered) {
        rt_memset(hid, 0, sizeof(*hid));
        hid->input.id = -1;
        return;
    }

    hid->hid_class = RT_NULL;
    hid->last_buttons = 0;
    rt_memset(hid->last_report, 0, sizeof(hid->last_report));
    rt_memset(&hid->report_map, 0, sizeof(hid->report_map));
    rt_memset(&hid->input.info, 0, sizeof(hid->input.info));
}

static rt_uint16_t hid_button_code(rt_uint8_t index)
{
    switch (index) {
    case 0:
        return BTN_LEFT;
    case 1:
        return BTN_RIGHT;
    case 2:
        return BTN_MIDDLE;
    case 3:
        return BTN_SIDE;
    case 4:
        return BTN_EXTRA;
    case 5:
        return BTN_FORWARD;
    case 6:
        return BTN_BACK;
    default:
        return BTN_EXTRA;
    }
}

static void hid_report_pointer_buttons(struct hid_device *hid, rt_uint8_t current_buttons)
{
    rt_uint8_t changed;
    rt_uint8_t index;

    changed = hid->last_buttons ^ current_buttons;
    if (changed == 0) {
        hid->last_buttons = current_buttons;
        return;
    }

    for (index = 0; index < 7; index++) {
        rt_uint8_t mask = 1u << index;

        if (changed & mask)
            rt_input_report(&hid->input, EV_KEY, hid_button_code(index), (current_buttons & mask) ? KEY_PRESSED : KEY_RELEASED);
    }

    hid->last_buttons = current_buttons;
}

/* Process HID keyboard data - Generate key events (Linux-style) */
static void hid_process_keyboard(struct hid_device *hid, const rt_uint8_t *data, int nbytes)
{
    struct usb_hid_kbd_report *report = (struct usb_hid_kbd_report *)data;
    int i;
    rt_bool_t changed = RT_FALSE;

    if (nbytes < sizeof(struct usb_hid_kbd_report))
        return;

    /* Process modifier keys (bits 0-7 in modifier byte)
     * HID Usage 224-231 maps to usb_kbd_keycode[224-231]
     * This handles Ctrl, Shift, Alt, Meta keys
     */
    for (i = 0; i < 8; i++) {
        unsigned char keycode = usb_kbd_keycode[i + 224];
        if (keycode) {
            /* Check if this modifier bit changed */
            int old_state = (hid->last_report[0] >> i) & 1;
            int new_state = (report->modifier >> i) & 1;

            if (old_state != new_state) {
                rt_input_report(&hid->input, EV_KEY, keycode, new_state ? KEY_PRESSED : KEY_RELEASED);
                changed = RT_TRUE;
            }
        }
    }

    /* Process released keys (in old[2-7] but not in new[2-7])
     * old[i] contains HID Usage ID, need to check if it's still in new report
     */
    for (i = 2; i < 8; i++) {
        if (hid->last_report[i] > 3) {
            /* Check if this key is still pressed (exists in new report) */
            int j;
            rt_bool_t found = RT_FALSE;

            for (j = 0; j < 6; j++) {
                if (report->key[j] == hid->last_report[i]) {
                    found = RT_TRUE;
                    break;
                }
            }

            if (!found) {
                /* Key was released */
                unsigned char keycode = usb_kbd_keycode[hid->last_report[i]];
                if (keycode) {
                    rt_input_report(&hid->input, EV_KEY, keycode, KEY_RELEASED);
                    changed = RT_TRUE;
                }
            }
        }
    }

    /* Process pressed keys (in new[2-7] but not in old[2-7]) */
    for (i = 0; i < 6; i++) {
        if (report->key[i] > 3) {
            /* Check if this is a new key press (not in old report) */
            int j;
            rt_bool_t found = RT_FALSE;

            for (j = 2; j < 8; j++) {
                if (hid->last_report[j] == report->key[i]) {
                    found = RT_TRUE;
                    break;
                }
            }

            if (!found) {
                /* New key pressed */
                unsigned char keycode = usb_kbd_keycode[report->key[i]];
                if (keycode) {
                    rt_input_report(&hid->input, EV_KEY, keycode, KEY_PRESSED);
                    changed = RT_TRUE;
                }
            }
        }
    }

    if (changed)
        rt_input_sync(&hid->input);

    /* Save current state for next comparison */
    rt_memcpy(hid->last_report, data, 8);
}

static void hid_process_mouse_boot(struct hid_device *hid, const rt_uint8_t *data, int nbytes)
{
    rt_uint8_t buttons;
    rt_int8_t dx;
    rt_int8_t dy;
    rt_bool_t changed = RT_FALSE;

    if (nbytes < 3)
        return;

    buttons = data[0] & HID_POINTER_BUTTON_MASK;
    dx = (rt_int8_t)data[1];
    dy = (rt_int8_t)data[2];

    if (hid->last_buttons != buttons)
        changed = RT_TRUE;
    hid_report_pointer_buttons(hid, buttons);

    if (dx != 0) {
        rt_input_report(&hid->input, EV_REL, REL_X, dx);
        changed = RT_TRUE;
    }
    if (dy != 0) {
        rt_input_report(&hid->input, EV_REL, REL_Y, dy);
        changed = RT_TRUE;
    }
    if (nbytes >= 4 && (rt_int8_t)data[3] != 0) {
        rt_input_report(&hid->input, EV_REL, REL_WHEEL, (rt_int8_t)data[3]);
        changed = RT_TRUE;
    }

    if (changed)
        rt_input_sync(&hid->input);
}

static void hid_process_pointer_report(struct hid_device *hid, const rt_uint8_t *data, int nbytes)
{
    const rt_uint8_t *report = data;
    rt_uint8_t current_buttons = 0;
    rt_int32_t value;
    rt_uint8_t index;
    rt_bool_t changed = RT_FALSE;

    if (hid->report_map.pointer_has_report_id) {
        if (nbytes <= 0 || data[0] != hid->report_map.pointer_report_id)
            return;
        report = data;
    }

    if (hid->type == HID_DEV_TOUCH) {
        if (hid->report_map.x_index >= 0) {
            value = hid_extract_field(report, &hid->report_map.fields[hid->report_map.x_index]);
            rt_input_report(&hid->input, EV_ABS, ABS_X, value);
            changed = RT_TRUE;
        }

        if (hid->report_map.y_index >= 0) {
            value = hid_extract_field(report, &hid->report_map.fields[hid->report_map.y_index]);
            rt_input_report(&hid->input, EV_ABS, ABS_Y, value);
            changed = RT_TRUE;
        }

        if (hid->report_map.pressure_index >= 0) {
            value = hid_extract_field(report, &hid->report_map.fields[hid->report_map.pressure_index]);
            rt_input_report(&hid->input, EV_ABS, ABS_PRESSURE, value);
            changed = RT_TRUE;
        }

        if (hid->report_map.tip_index >= 0) {
            current_buttons = hid_extract_field(report, &hid->report_map.fields[hid->report_map.tip_index]) ? 1 : 0;
        } else if (hid->report_map.button_count > 0) {
            current_buttons = hid_extract_field(report, &hid->report_map.fields[hid->report_map.button_indices[0]]) ? 1 : 0;
        }

        if ((hid->last_buttons ^ current_buttons) & 0x01) {
            rt_input_report(&hid->input, EV_KEY, BTN_TOUCH, current_buttons ? KEY_PRESSED : KEY_RELEASED);
            hid->last_buttons = current_buttons;
            changed = RT_TRUE;
        }
    } else {
        for (index = 0; index < hid->report_map.button_count; index++) {
            const struct hid_report_field *field;

            field = &hid->report_map.fields[hid->report_map.button_indices[index]];
            if (hid_extract_field(report, field))
                current_buttons |= 1u << index;
        }
        if (hid->last_buttons != current_buttons)
            changed = RT_TRUE;
        hid_report_pointer_buttons(hid, current_buttons);

        if (hid->report_map.x_index >= 0) {
            value = hid_extract_field(report, &hid->report_map.fields[hid->report_map.x_index]);
            if (value != 0) {
                rt_input_report(&hid->input, EV_REL, REL_X, value);
                changed = RT_TRUE;
            }
        }

        if (hid->report_map.y_index >= 0) {
            value = hid_extract_field(report, &hid->report_map.fields[hid->report_map.y_index]);
            if (value != 0) {
                rt_input_report(&hid->input, EV_REL, REL_Y, value);
                changed = RT_TRUE;
            }
        }

        if (hid->report_map.wheel_index >= 0) {
            value = hid_extract_field(report, &hid->report_map.fields[hid->report_map.wheel_index]);
            if (value != 0) {
                rt_input_report(&hid->input, EV_REL, REL_WHEEL, value);
                changed = RT_TRUE;
            }
        }
    }

    if (changed)
        rt_input_sync(&hid->input);
}

static void usbh_hid_kbd_callback(void *arg, int nbytes)
{
    struct hid_device *hid = (struct hid_device *)arg;
    struct usbh_hid *hid_class = hid->hid_class;

    if (hid_class == RT_NULL || !hid->input.connected)
        return;

    if (nbytes > 0) {
        hid_process_keyboard(hid, hid_kbd_buffer, nbytes);
        usbh_submit_urb(&hid_class->intin_urb);
    } else if (nbytes == -USB_ERR_NAK) {
        usbh_submit_urb(&hid_class->intin_urb);
    }
}

static void usbh_hid_pointer_callback(void *arg, int nbytes)
{
    struct hid_device *hid = (struct hid_device *)arg;
    struct usbh_hid *hid_class = hid->hid_class;

    if (hid_class == RT_NULL || !hid->input.connected)
        return;

    if (nbytes > 0) {
        if (hid->use_boot_protocol)
            hid_process_mouse_boot(hid, hid_pointer_buffer, nbytes);
        else
            hid_process_pointer_report(hid, hid_pointer_buffer, nbytes);

        usbh_submit_urb(&hid_class->intin_urb);
    } else if (nbytes == -USB_ERR_NAK) {
        usbh_submit_urb(&hid_class->intin_urb);
    }
}

static void hid_cleanup_device(struct hid_device *hid)
{
    if (!hid->registered)
        return;

    rt_input_dev_disconnect(&hid->input);
    hid->registered = (hid->input.id >= 0);
    hid->hid_class = RT_NULL;
    hid->last_buttons = 0;
    rt_memset(hid->last_report, 0, sizeof(hid->last_report));
    rt_memset(&hid->report_map, 0, sizeof(hid->report_map));
}

static int hid_start_device(struct hid_device *hid, rt_uint8_t *buffer, usbh_complete_callback_t callback)
{
    int ret;

    rt_memset(&hid->input.info, 0, sizeof(hid->input.info));

    if (hid->type == HID_DEV_KEYBOARD) {
        rt_input_set_name(&hid->input, "usb-hid-keyboard");
        rt_input_set_kind(&hid->input, RT_INPUT_KIND_KEYBOARD);
        rt_input_set_capability(&hid->input, EV_KEY, KEY_A);
        rt_input_set_capability(&hid->input, EV_KEY, KEY_ENTER);
    } else if (hid->type == HID_DEV_TOUCH) {
        rt_input_set_name(&hid->input, "usb-hid-touch");
        rt_input_set_kind(&hid->input, RT_INPUT_KIND_TOUCH);
        rt_input_set_capability(&hid->input, EV_KEY, BTN_TOUCH);
        rt_input_set_capability(&hid->input, EV_ABS, ABS_X);
        rt_input_set_capability(&hid->input, EV_ABS, ABS_Y);
        rt_input_set_capability(&hid->input, EV_ABS, ABS_PRESSURE);
    } else {
        rt_input_set_name(&hid->input, "usb-hid-mouse");
        rt_input_set_kind(&hid->input, RT_INPUT_KIND_MOUSE);
        rt_input_set_capability(&hid->input, EV_KEY, BTN_LEFT);
        rt_input_set_capability(&hid->input, EV_REL, REL_X);
        rt_input_set_capability(&hid->input, EV_REL, REL_Y);
        rt_input_set_capability(&hid->input, EV_REL, REL_WHEEL);
    }

    if (hid->registered) {
        ret = rt_input_dev_reconnect(&hid->input);
    } else {
        ret = rt_input_dev_register(&hid->input);
        if (ret == RT_EOK)
            hid->registered = RT_TRUE;
    }

    if (ret != RT_EOK)
        return ret;

    usbh_int_urb_fill(&hid->hid_class->intin_urb,
                      hid->hid_class->hport,
                      hid->hid_class->intin,
                      buffer,
                      hid->hid_class->intin->wMaxPacketSize,
                      0,
                      callback,
                      hid);

    ret = usbh_submit_urb(&hid->hid_class->intin_urb);
    if (ret < 0) {
        hid_cleanup_device(hid);
        return ret;
    }

    return RT_EOK;
}

/* Start HID device */
void usbh_hid_run(struct usbh_hid *hid_class)
{
    int ret;
    rt_bool_t prefer_report_protocol = RT_FALSE;
    struct usb_interface_descriptor *intf_desc;

    /* Get interface descriptor to check protocol */
    intf_desc = &hid_class->hport->config.intf[hid_class->intf].altsetting[0].intf_desc;

    if ((NULL == hid_class->intin) || (0x1 != intf_desc->bNumEndpoints)) {
        USB_LOG_INFO("HID device protocol 0x%02x subclass 0x%02x have %d ep num unsupported\n",
                     intf_desc->bInterfaceProtocol, intf_desc->bInterfaceSubClass, intf_desc->bNumEndpoints);
        return;
    }

    if ((intf_desc->bInterfaceProtocol == HID_PROTOCOL_KEYBOARD) &&
        (intf_desc->bInterfaceSubClass == HID_SUBCLASS_BOOTIF)) {
        if (hid_kbd_dev.registered) {
            if (!hid_kbd_dev.input.connected) {
                hid_prepare_device(&hid_kbd_dev);
            } else {
                USB_LOG_WRN("Only one HID keyboard device is supported at a time\n");
                return;
            }
        } else {
            hid_prepare_device(&hid_kbd_dev);
        }

        hid_kbd_dev.hid_class = hid_class;
        hid_kbd_dev.type = HID_DEV_KEYBOARD;
        hid_kbd_dev.use_boot_protocol = RT_TRUE;

        ret = hid_start_device(&hid_kbd_dev, hid_kbd_buffer, usbh_hid_kbd_callback);
        if (ret != RT_EOK) {
            USB_LOG_ERR("Failed to start HID keyboard: %d\n", ret);
            return;
        }

        rt_kprintf("HID keyboard started on /dev/%s\n", hid_kbd_dev.input.parent.parent.name);
        return;
    }

    if (hid_pointer_dev.registered) {
        if (!hid_pointer_dev.input.connected) {
            hid_prepare_device(&hid_pointer_dev);
        } else {
            USB_LOG_WRN("Only one HID pointer device is supported at a time\n");
            return;
        }
    } else {
        hid_prepare_device(&hid_pointer_dev);
    }

    hid_pointer_dev.hid_class = hid_class;

    if ((intf_desc->bInterfaceProtocol == HID_PROTOCOL_MOUSE) &&
        (intf_desc->bInterfaceSubClass == HID_SUBCLASS_BOOTIF)) {
        prefer_report_protocol = RT_TRUE;
    }

    if ((intf_desc->bInterfaceProtocol == HID_PROTOCOL_MOUSE) ||
        (intf_desc->bInterfaceProtocol == HID_PROTOCOL_NONE)) {
        if (hid_class->report_desc_len == 0) {
            if (!prefer_report_protocol) {
                USB_LOG_WRN("HID report descriptor is empty, skipping pointer device\n");
                return;
            }
        }

        if (prefer_report_protocol) {
            ret = usbh_hid_set_protocol(hid_class, 0x1);
            if (ret < 0) {
                USB_LOG_WRN("Failed to switch mouse to report protocol, fallback to boot protocol\n");
            }
        }

        if (hid_class->report_desc_len > 0) {
            ret = hid_parse_report_descriptor(hid_class->report_desc, hid_class->report_desc_len, &hid_pointer_dev.report_map);
            if (ret >= 0) {
                hid_pointer_dev.type = hid_pointer_dev.report_map.is_absolute ? HID_DEV_TOUCH : HID_DEV_MOUSE;
                hid_pointer_dev.use_boot_protocol = RT_FALSE;
            } else if (prefer_report_protocol) {
                hid_pointer_dev.type = HID_DEV_MOUSE;
                hid_pointer_dev.use_boot_protocol = RT_TRUE;
            } else {
                USB_LOG_WRN("Unsupported HID report descriptor on intf %u, skipping (ret=%d len=%u)\n",
                            hid_class->intf, ret, hid_class->report_desc_len);
                USB_LOG_WRN("Report descriptor hex dump:\n");
                for (uint32_t i = 0; i < hid_class->report_desc_len; i += 16) {
                    uint32_t j, end = i + 16;
                    if (end > hid_class->report_desc_len) end = hid_class->report_desc_len;
                    rt_kprintf("  %04x: ", i);
                    for (j = i; j < end; j++)
                        rt_kprintf("%02x ", hid_class->report_desc[j]);
                    rt_kprintf("\n");
                }
                return;
            }
        } else if (prefer_report_protocol) {
            hid_pointer_dev.type = HID_DEV_MOUSE;
            hid_pointer_dev.use_boot_protocol = RT_TRUE;
        } else {
            USB_LOG_WRN("HID report descriptor is empty, skipping pointer device\n");
            return;
        }
    } else {
        USB_LOG_INFO("HID device protocol 0x%02x subclass 0x%02x is not handled\n",
                     intf_desc->bInterfaceProtocol, intf_desc->bInterfaceSubClass);
        return;
    }

    ret = hid_start_device(&hid_pointer_dev, hid_pointer_buffer, usbh_hid_pointer_callback);
    if (ret != RT_EOK) {
        USB_LOG_ERR("Failed to start HID pointer device: %d\n", ret);
        return;
    }

    rt_kprintf("HID %s started on /dev/%s\n",
               hid_pointer_dev.type == HID_DEV_TOUCH ? "touch" : "mouse",
               hid_pointer_dev.input.parent.parent.name);
    rt_kprintf("  report_id=%u has_id=%d x=%d y=%d buttons=%d wheel=%d absolute=%d\n",
               hid_pointer_dev.report_map.pointer_report_id,
               hid_pointer_dev.report_map.pointer_has_report_id,
               hid_pointer_dev.report_map.x_index,
               hid_pointer_dev.report_map.y_index,
               hid_pointer_dev.report_map.button_count,
               hid_pointer_dev.report_map.wheel_index,
               hid_pointer_dev.report_map.is_absolute);
}

/* Stop HID device */
void usbh_hid_stop(struct usbh_hid *hid_class)
{
    if (hid_kbd_dev.registered && hid_kbd_dev.hid_class == hid_class)
        hid_cleanup_device(&hid_kbd_dev);

    if (hid_pointer_dev.registered && hid_pointer_dev.hid_class == hid_class)
        hid_cleanup_device(&hid_pointer_dev);
}
