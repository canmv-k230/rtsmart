/*
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ESP_HOSTED_PROTO_H
#define ESP_HOSTED_PROTO_H

#include <stddef.h>
#include <stdint.h>

struct eh_pb_writer
{
    uint8_t *data;
    size_t length;
    size_t capacity;
    int error;
};

struct eh_pb_reader
{
    const uint8_t *data;
    size_t length;
    size_t offset;
};

struct eh_pb_field
{
    uint32_t number;
    uint8_t wire_type;
    uint64_t value;
    const uint8_t *data;
    size_t length;
};

void eh_pb_writer_init(struct eh_pb_writer *writer, uint8_t *data, size_t capacity);
int eh_pb_put_varint(struct eh_pb_writer *writer, uint32_t field, uint64_t value);
int eh_pb_put_int32(struct eh_pb_writer *writer, uint32_t field, int32_t value);
int eh_pb_put_bytes(struct eh_pb_writer *writer, uint32_t field, const void *data, size_t length);

void eh_pb_reader_init(struct eh_pb_reader *reader, const void *data, size_t length);
int eh_pb_next(struct eh_pb_reader *reader, struct eh_pb_field *field);
int eh_pb_find(const void *data, size_t length, uint32_t number, struct eh_pb_field *field);
int32_t eh_pb_field_int32(const struct eh_pb_field *field);

#endif
