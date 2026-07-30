/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_hosted_proto.h"

#include <string.h>

#define EH_PB_WIRE_VARINT 0
#define EH_PB_WIRE_64BIT  1
#define EH_PB_WIRE_BYTES  2
#define EH_PB_WIRE_32BIT  5

static int eh_pb_write_raw_varint(struct eh_pb_writer *writer, uint64_t value)
{
    do
    {
        uint8_t byte = value & 0x7f;

        value >>= 7;
        if (value)
        {
            byte |= 0x80;
        }
        if (writer->length >= writer->capacity)
        {
            writer->error = -1;
            return -1;
        }
        writer->data[writer->length++] = byte;
    } while (value);

    return 0;
}

static int eh_pb_read_raw_varint(struct eh_pb_reader *reader, uint64_t *value)
{
    uint64_t result = 0;
    unsigned int shift;

    for (shift = 0; shift < 64; shift += 7)
    {
        uint8_t byte;

        if (reader->offset >= reader->length)
        {
            return -1;
        }
        byte = reader->data[reader->offset++];
        result |= ((uint64_t)(byte & 0x7f)) << shift;
        if (!(byte & 0x80))
        {
            *value = result;
            return 0;
        }
    }

    return -1;
}

void eh_pb_writer_init(struct eh_pb_writer *writer, uint8_t *data, size_t capacity)
{
    writer->data = data;
    writer->length = 0;
    writer->capacity = capacity;
    writer->error = 0;
}

int eh_pb_put_varint(struct eh_pb_writer *writer, uint32_t field, uint64_t value)
{
    if (!field || writer->error)
    {
        return -1;
    }
    if (eh_pb_write_raw_varint(writer, ((uint64_t)field << 3) | EH_PB_WIRE_VARINT))
    {
        return -1;
    }
    return eh_pb_write_raw_varint(writer, value);
}

int eh_pb_put_int32(struct eh_pb_writer *writer, uint32_t field, int32_t value)
{
    return eh_pb_put_varint(writer, field, (uint64_t)(int64_t)value);
}

int eh_pb_put_bytes(struct eh_pb_writer *writer, uint32_t field, const void *data, size_t length)
{
    if (!field || writer->error || (length && !data))
    {
        return -1;
    }
    if (eh_pb_write_raw_varint(writer, ((uint64_t)field << 3) | EH_PB_WIRE_BYTES) ||
        eh_pb_write_raw_varint(writer, length))
    {
        return -1;
    }
    if (length > writer->capacity - writer->length)
    {
        writer->error = -1;
        return -1;
    }
    if (length)
    {
        memcpy(writer->data + writer->length, data, length);
        writer->length += length;
    }
    return 0;
}

void eh_pb_reader_init(struct eh_pb_reader *reader, const void *data, size_t length)
{
    reader->data = data;
    reader->length = length;
    reader->offset = 0;
}

int eh_pb_next(struct eh_pb_reader *reader, struct eh_pb_field *field)
{
    uint64_t key;
    uint64_t length;
    size_t skip;

    if (reader->offset == reader->length)
    {
        return 0;
    }
    if (reader->offset > reader->length || eh_pb_read_raw_varint(reader, &key))
    {
        return -1;
    }

    field->number = key >> 3;
    field->wire_type = key & 0x07;
    field->value = 0;
    field->data = NULL;
    field->length = 0;
    if (!field->number)
    {
        return -1;
    }

    switch (field->wire_type)
    {
    case EH_PB_WIRE_VARINT:
        return eh_pb_read_raw_varint(reader, &field->value) ? -1 : 1;
    case EH_PB_WIRE_64BIT:
        skip = 8;
        break;
    case EH_PB_WIRE_BYTES:
        if (eh_pb_read_raw_varint(reader, &length) || length > reader->length - reader->offset)
        {
            return -1;
        }
        field->data = reader->data + reader->offset;
        field->length = (size_t)length;
        reader->offset += field->length;
        return 1;
    case EH_PB_WIRE_32BIT:
        skip = 4;
        break;
    default:
        return -1;
    }

    if (skip > reader->length - reader->offset)
    {
        return -1;
    }
    field->data = reader->data + reader->offset;
    field->length = skip;
    reader->offset += skip;
    return 1;
}

int eh_pb_find(const void *data, size_t length, uint32_t number, struct eh_pb_field *field)
{
    struct eh_pb_reader reader;
    int result;

    eh_pb_reader_init(&reader, data, length);
    while ((result = eh_pb_next(&reader, field)) > 0)
    {
        if (field->number == number)
        {
            return 1;
        }
    }
    return result;
}

int32_t eh_pb_field_int32(const struct eh_pb_field *field)
{
    return (int32_t)(uint32_t)field->value;
}
