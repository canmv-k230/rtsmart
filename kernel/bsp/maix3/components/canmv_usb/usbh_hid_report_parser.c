#include <rtthread.h>

#include "usbh_hid_report_parser.h"

#define HID_ITEM_TYPE_MAIN      0
#define HID_ITEM_TYPE_GLOBAL    1
#define HID_ITEM_TYPE_LOCAL     2

#define HID_MAIN_ITEM_INPUT     8
#define HID_MAIN_ITEM_COLLECTION 10

#define HID_GLOBAL_USAGE_PAGE   0
#define HID_GLOBAL_LOGICAL_MIN  1
#define HID_GLOBAL_LOGICAL_MAX  2
#define HID_GLOBAL_REPORT_SIZE  7
#define HID_GLOBAL_REPORT_ID    8
#define HID_GLOBAL_REPORT_COUNT 9

#define HID_LOCAL_USAGE         0
#define HID_LOCAL_USAGE_MIN     1
#define HID_LOCAL_USAGE_MAX     2

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_BUTTON          0x09
#define HID_USAGE_PAGE_DIGITIZER       0x0d

#define HID_USAGE_X                    0x30
#define HID_USAGE_Y                    0x31
#define HID_USAGE_WHEEL                0x38
#define HID_USAGE_TIP_SWITCH           0x42
#define HID_USAGE_PRESSURE             0x30

struct hid_parser_global
{
    rt_uint8_t report_id;
    rt_uint16_t usage_page;
    rt_int32_t logical_min;
    rt_int32_t logical_max;
    rt_uint8_t report_size;
    rt_uint8_t report_count;
};

static rt_int32_t hid_item_data_to_s32(rt_uint32_t value, rt_uint8_t size)
{
    if (size == 1)
        return (rt_int8_t)value;
    if (size == 2)
        return (rt_int16_t)value;
    return (rt_int32_t)value;
}

static int hid_add_field(struct hid_report_map *map,
                         const struct hid_parser_global *global,
                         rt_uint16_t usage,
                         rt_uint16_t bit_offset,
                         rt_uint8_t flags)
{
    struct hid_report_field *field;

    if (map->field_count >= HID_REPORT_MAX_FIELDS)
        return -1;

    field = &map->fields[map->field_count];
    field->report_id = global->report_id;
    field->usage_page = global->usage_page;
    field->usage = usage;
    field->logical_min = global->logical_min;
    field->logical_max = global->logical_max;
    field->bit_offset = bit_offset;
    field->report_size = global->report_size;
    field->flags = flags;

    if (global->logical_min < 0)
        field->flags |= HID_FIELD_FLAG_SIGNED;

    map->field_count++;
    return 0;
}

static void hid_select_pointer_report(struct hid_report_map *map, const rt_uint16_t *report_bit_offsets)
{
    rt_uint8_t candidate_report_id = 0;
    rt_int8_t best_x = -1;
    rt_int8_t best_y = -1;
    rt_int8_t best_wheel = -1;
    rt_int8_t best_tip = -1;
    rt_int8_t best_pressure = -1;
    rt_uint8_t best_buttons[HID_REPORT_MAX_BUTTONS];
    rt_uint8_t best_button_count = 0;
    rt_bool_t best_is_absolute = RT_FALSE;
    int report_id;

    rt_memset(best_buttons, 0, sizeof(best_buttons));
    map->x_index = -1;
    map->y_index = -1;
    map->wheel_index = -1;
    map->tip_index = -1;
    map->pressure_index = -1;
    map->button_count = 0;
    map->is_absolute = RT_FALSE;

    for (report_id = 0; report_id < 256; report_id++) {
        rt_int8_t x_index = -1;
        rt_int8_t y_index = -1;
        rt_int8_t wheel_index = -1;
        rt_int8_t tip_index = -1;
        rt_int8_t pressure_index = -1;
        rt_uint8_t button_indices[HID_REPORT_MAX_BUTTONS];
        rt_uint8_t button_count = 0;
        rt_bool_t is_absolute = RT_FALSE;
        rt_uint8_t field_index;

        rt_memset(button_indices, 0, sizeof(button_indices));

        for (field_index = 0; field_index < map->field_count; field_index++) {
            const struct hid_report_field *field = &map->fields[field_index];

            if (field->report_id != report_id || (field->flags & HID_FIELD_FLAG_CONSTANT))
                continue;

            if (field->usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
                if (field->usage == HID_USAGE_X)
                    x_index = field_index;
                else if (field->usage == HID_USAGE_Y)
                    y_index = field_index;
                else if (field->usage == HID_USAGE_WHEEL)
                    wheel_index = field_index;

                if (!(field->flags & HID_FIELD_FLAG_RELATIVE))
                    is_absolute = RT_TRUE;
            } else if (field->usage_page == HID_USAGE_PAGE_BUTTON) {
                if (button_count < HID_REPORT_MAX_BUTTONS)
                    button_indices[button_count++] = field_index;
            } else if (field->usage_page == HID_USAGE_PAGE_DIGITIZER) {
                if (field->usage == HID_USAGE_TIP_SWITCH)
                    tip_index = field_index;
                else if (field->usage == HID_USAGE_PRESSURE)
                    pressure_index = field_index;

                is_absolute = RT_TRUE;
            }
        }

        if (x_index < 0 || y_index < 0)
            continue;

        candidate_report_id = (rt_uint8_t)report_id;
        best_x = x_index;
        best_y = y_index;
        best_wheel = wheel_index;
        best_tip = tip_index;
        best_pressure = pressure_index;
        best_button_count = button_count;
        best_is_absolute = is_absolute;
        rt_memcpy(best_buttons, button_indices, sizeof(best_buttons));

        if (button_count > 0 || wheel_index >= 0 || tip_index >= 0)
            break;
    }

    map->x_index = best_x;
    map->y_index = best_y;
    map->wheel_index = best_wheel;
    map->tip_index = best_tip;
    map->pressure_index = best_pressure;
    map->button_count = best_button_count;
    map->is_absolute = best_is_absolute;
    rt_memcpy(map->button_indices, best_buttons, sizeof(map->button_indices));

    if (best_x >= 0 && best_y >= 0) {
        map->pointer_has_report_id = candidate_report_id != 0;
        map->pointer_report_id = candidate_report_id;
        map->report_bits = report_bit_offsets[candidate_report_id];
    }
}

int hid_parse_report_descriptor(const rt_uint8_t *desc, int len, struct hid_report_map *map)
{
    struct hid_parser_global global;
    rt_uint16_t report_bit_offsets[256];
    rt_uint16_t usages[HID_REPORT_MAX_FIELDS];
    rt_uint8_t usage_count = 0;
    rt_uint16_t usage_min = 0;
    rt_uint16_t usage_max = 0;
    rt_bool_t have_usage_range = RT_FALSE;
    int index = 0;

    if (desc == RT_NULL || map == RT_NULL || len <= 0)
        return -RT_EINVAL;

    rt_memset(&global, 0, sizeof(global));
    rt_memset(map, 0, sizeof(*map));
    rt_memset(report_bit_offsets, 0, sizeof(report_bit_offsets));
    map->x_index = -1;
    map->y_index = -1;
    map->wheel_index = -1;
    map->tip_index = -1;
    map->pressure_index = -1;

    while (index < len) {
        rt_uint8_t prefix = desc[index++];
        rt_uint8_t size_code;
        rt_uint8_t size;
        rt_uint8_t type;
        rt_uint8_t tag;
        rt_uint32_t value = 0;
        rt_uint8_t item_flags = 0;
        int count;

        if (prefix == 0xfe) {
            if (index + 1 >= len)
                break;
            size = desc[index];
            index += 2 + size;
            continue;
        }

        size_code = prefix & 0x03;
        size = (size_code == 3) ? 4 : size_code;
        type = (prefix >> 2) & 0x03;
        tag = (prefix >> 4) & 0x0f;

        if (index + size > len)
            break;

        for (count = 0; count < size; count++)
            value |= ((rt_uint32_t)desc[index + count]) << (count * 8);
        index += size;

        switch (type) {
        case HID_ITEM_TYPE_GLOBAL:
            switch (tag) {
            case HID_GLOBAL_USAGE_PAGE:
                global.usage_page = (rt_uint16_t)value;
                break;
            case HID_GLOBAL_LOGICAL_MIN:
                global.logical_min = hid_item_data_to_s32(value, size);
                break;
            case HID_GLOBAL_LOGICAL_MAX:
                global.logical_max = hid_item_data_to_s32(value, size);
                break;
            case HID_GLOBAL_REPORT_SIZE:
                global.report_size = (rt_uint8_t)value;
                break;
            case HID_GLOBAL_REPORT_ID:
                map->has_report_id = RT_TRUE;
                global.report_id = (rt_uint8_t)value;
                if (report_bit_offsets[global.report_id] == 0)
                    report_bit_offsets[global.report_id] = 8;
                break;
            case HID_GLOBAL_REPORT_COUNT:
                global.report_count = (rt_uint8_t)value;
                break;
            default:
                break;
            }
            break;

        case HID_ITEM_TYPE_LOCAL:
            switch (tag) {
            case HID_LOCAL_USAGE:
                if (usage_count < HID_REPORT_MAX_FIELDS)
                    usages[usage_count++] = (rt_uint16_t)value;
                break;
            case HID_LOCAL_USAGE_MIN:
                usage_min = (rt_uint16_t)value;
                have_usage_range = RT_TRUE;
                break;
            case HID_LOCAL_USAGE_MAX:
                usage_max = (rt_uint16_t)value;
                have_usage_range = RT_TRUE;
                break;
            default:
                break;
            }
            break;

        case HID_ITEM_TYPE_MAIN:
            if (tag == HID_MAIN_ITEM_COLLECTION) {
                usage_count = 0;
                have_usage_range = RT_FALSE;
                break;
            }

            if (tag != HID_MAIN_ITEM_INPUT)
                break;

            if (value & 0x01)
                item_flags |= HID_FIELD_FLAG_CONSTANT;
            if (value & 0x04)
                item_flags |= HID_FIELD_FLAG_RELATIVE;

            for (count = 0; count < global.report_count; count++) {
                rt_uint16_t usage = 0;
                rt_uint16_t bit_offset = report_bit_offsets[global.report_id];

                if (usage_count > 0) {
                    if (count < usage_count)
                        usage = usages[count];
                    else
                        usage = usages[usage_count - 1];
                } else if (have_usage_range && usage_max >= usage_min) {
                    usage = usage_min + count;
                }

                if (hid_add_field(map, &global, usage, bit_offset, item_flags) < 0)
                    return -RT_ENOMEM;

                report_bit_offsets[global.report_id] = bit_offset + global.report_size;
            }

            usage_count = 0;
            have_usage_range = RT_FALSE;
            break;

        default:
            break;
        }
    }

    hid_select_pointer_report(map, report_bit_offsets);

    if (map->x_index < 0 || map->y_index < 0)
        return -RT_ERROR;

    map->report_id = map->pointer_report_id;
    return 0;
}

rt_int32_t hid_extract_field(const rt_uint8_t *report, const struct hid_report_field *field)
{
    rt_uint32_t value = 0;
    rt_uint16_t bit;

    if (report == RT_NULL || field == RT_NULL || field->report_size == 0 || field->report_size > 32)
        return 0;

    for (bit = 0; bit < field->report_size; bit++) {
        rt_uint16_t absolute_bit = field->bit_offset + bit;
        rt_uint8_t byte = report[absolute_bit / 8];

        if (byte & (1u << (absolute_bit % 8)))
            value |= 1u << bit;
    }

    if ((field->flags & HID_FIELD_FLAG_SIGNED) && field->report_size < 32) {
        rt_uint32_t sign_bit = 1u << (field->report_size - 1);
        if (value & sign_bit)
            value |= ~((1u << field->report_size) - 1u);
    }

    return (rt_int32_t)value;
}