/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xsm_io.h - Checked little-endian XSM byte primitives
 */

#ifndef XR_XSM_IO_H
#define XR_XSM_IO_H

#include "../../base/xmalloc.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct XrXsmWriter {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
    size_t limit;
} XrXsmWriter;

typedef struct XrXsmReader {
    const uint8_t *data;
    size_t size;
    size_t offset;
    bool failed;
    size_t string_bytes;
    size_t allocation_bytes;
} XrXsmReader;

static inline bool xr_xsm_writer_reserve(XrXsmWriter *writer, size_t extra) {
    if (writer->failed || extra > SIZE_MAX - writer->size ||
        (writer->limit != 0 &&
         (writer->size > writer->limit || extra > writer->limit - writer->size))) {
        writer->failed = true;
        return false;
    }
    size_t required = writer->size + extra;
    if (required <= writer->capacity)
        return true;
    size_t capacity = writer->capacity ? writer->capacity : 1024;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            writer->failed = true;
            return false;
        }
        capacity *= 2;
    }
    uint8_t *grown = (uint8_t *) xr_realloc(writer->data, capacity);
    if (!grown) {
        writer->failed = true;
        return false;
    }
    writer->data = grown;
    writer->capacity = capacity;
    return true;
}

static inline void xr_xsm_put_bytes(XrXsmWriter *writer, const void *bytes, size_t size) {
    if (!xr_xsm_writer_reserve(writer, size))
        return;
    memcpy(writer->data + writer->size, bytes, size);
    writer->size += size;
}

static inline void xr_xsm_put_u8(XrXsmWriter *writer, uint8_t value) {
    xr_xsm_put_bytes(writer, &value, 1);
}

static inline void xr_xsm_put_u16(XrXsmWriter *writer, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t) value, (uint8_t) (value >> 8)};
    xr_xsm_put_bytes(writer, bytes, sizeof(bytes));
}

static inline void xr_xsm_put_u32(XrXsmWriter *writer, uint32_t value) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_xsm_put_bytes(writer, bytes, sizeof(bytes));
}

static inline void xr_xsm_put_u64(XrXsmWriter *writer, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_xsm_put_bytes(writer, bytes, sizeof(bytes));
}

static inline void xr_xsm_put_string(XrXsmWriter *writer, const char *text) {
    const char *value = text ? text : "";
    size_t length = strlen(value);
    if (length > UINT32_MAX) {
        writer->failed = true;
        return;
    }
    xr_xsm_put_u32(writer, (uint32_t) length);
    xr_xsm_put_bytes(writer, value, length);
}

static inline bool xr_xsm_take_bytes(XrXsmReader *reader, void *out, size_t size) {
    if (reader->failed || reader->offset > reader->size || size > reader->size - reader->offset) {
        reader->failed = true;
        return false;
    }
    if (out)
        memcpy(out, reader->data + reader->offset, size);
    reader->offset += size;
    return true;
}

static inline uint8_t xr_xsm_take_u8(XrXsmReader *reader) {
    uint8_t value = 0;
    xr_xsm_take_bytes(reader, &value, 1);
    return value;
}

static inline uint16_t xr_xsm_take_u16(XrXsmReader *reader) {
    uint8_t bytes[2] = {0};
    xr_xsm_take_bytes(reader, bytes, sizeof(bytes));
    return (uint16_t) bytes[0] | ((uint16_t) bytes[1] << 8);
}

static inline uint32_t xr_xsm_take_u32(XrXsmReader *reader) {
    uint8_t bytes[4] = {0};
    xr_xsm_take_bytes(reader, bytes, sizeof(bytes));
    uint32_t value = 0;
    for (unsigned i = 0; i < sizeof(bytes); i++)
        value |= (uint32_t) bytes[i] << (i * 8);
    return value;
}

static inline uint64_t xr_xsm_take_u64(XrXsmReader *reader) {
    uint8_t bytes[8] = {0};
    xr_xsm_take_bytes(reader, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < sizeof(bytes); i++)
        value |= (uint64_t) bytes[i] << (i * 8);
    return value;
}

#endif  // XR_XSM_IO_H
