#ifndef USBH_HID_REPORT_PARSER_H
#define USBH_HID_REPORT_PARSER_H

#include <rtdef.h>

#define HID_REPORT_MAX_FIELDS 64
#define HID_REPORT_MAX_BUTTONS 8

#define HID_FIELD_FLAG_CONSTANT 0x01
#define HID_FIELD_FLAG_RELATIVE 0x02
#define HID_FIELD_FLAG_SIGNED   0x04

struct hid_report_field
{
    rt_uint8_t report_id;
    rt_uint16_t usage_page;
    rt_uint16_t usage;
    rt_int32_t logical_min;
    rt_int32_t logical_max;
    rt_uint16_t bit_offset;
    rt_uint8_t report_size;
    rt_uint8_t flags;
};

struct hid_report_map
{
    struct hid_report_field fields[HID_REPORT_MAX_FIELDS];
    rt_uint8_t field_count;
    rt_uint16_t report_bits;
    rt_bool_t has_report_id;
    rt_uint8_t report_id;
    rt_bool_t pointer_has_report_id;
    rt_uint8_t pointer_report_id;
    rt_bool_t is_absolute;
    rt_int8_t x_index;
    rt_int8_t y_index;
    rt_int8_t wheel_index;
    rt_int8_t tip_index;
    rt_int8_t pressure_index;
    rt_uint8_t button_indices[HID_REPORT_MAX_BUTTONS];
    rt_uint8_t button_count;
};

int hid_parse_report_descriptor(const rt_uint8_t *desc, int len, struct hid_report_map *map);
rt_int32_t hid_extract_field(const rt_uint8_t *report, const struct hid_report_field *field);

#endif