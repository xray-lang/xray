/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_serial.c - Binary serialization for XrValue
 *
 * KEY CONCEPT:
 *   Compact binary encoding for all serializable XrValue types.
 *   Uses varint for integers, IEEE754 for floats, length-prefixed
 *   for strings/bytes, and recursive encoding for containers.
 *
 * WHY THIS DESIGN:
 *   - Varint saves bandwidth for small integers (common case)
 *   - Version byte enables future protocol evolution
 *   - Depth limit prevents stack overflow on cyclic-like deep nesting
 */

#include "cluster_serial.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xset.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xshape.h"
#include "../../src/runtime/gc/xgc_header.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/symbol/xsymbol_table.h"

#include "../compress/compress.h"

#include <stdlib.h>
#include <string.h>

/* ========== Encode Buffer ========== */

#define SERIAL_BUF_INIT_CAP 256

void xr_serial_buf_init(XrSerialBuf *buf) {
    buf->data = (uint8_t *)xr_malloc(SERIAL_BUF_INIT_CAP);
    buf->len = 0;
    buf->cap = SERIAL_BUF_INIT_CAP;
    buf->error = (buf->data == NULL);
}

void xr_serial_buf_free(XrSerialBuf *buf) {
    if (buf->data) {
        xr_free(buf->data);
        buf->data = NULL;
    }
    buf->len = 0;
    buf->cap = 0;
}

static void buf_ensure(XrSerialBuf *buf, size_t need) {
    if (buf->error || buf->len + need <= buf->cap) return;
    size_t new_cap = buf->cap * 2;
    while (new_cap < buf->len + need) new_cap *= 2;
    uint8_t *new_data = (uint8_t *)xr_realloc(buf->data, new_cap);
    if (!new_data) {
        buf->error = true;
        return;
    }
    buf->data = new_data;
    buf->cap = new_cap;
}

static inline void buf_put_u8(XrSerialBuf *buf, uint8_t v) {
    buf_ensure(buf, 1);
    if (buf->error) return;
    buf->data[buf->len++] = v;
}

static inline void buf_put_bytes(XrSerialBuf *buf, const void *data, size_t len) {
    buf_ensure(buf, len);
    if (buf->error) return;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

/* ========== Varint Encoding (LEB128 / ZigZag) ========== */

// ZigZag encode: maps signed to unsigned (0→0, -1→1, 1→2, -2→3, ...)
static inline uint64_t zigzag_encode(int64_t v) {
    return (uint64_t)((v << 1) ^ (v >> 63));
}

static inline int64_t zigzag_decode(uint64_t v) {
    return (int64_t)((v >> 1) ^ -(int64_t)(v & 1));
}

// Write unsigned LEB128
static void buf_put_varint(XrSerialBuf *buf, uint64_t v) {
    buf_ensure(buf, 10);
    if (buf->error) return;
    while (v >= 0x80) {
        buf->data[buf->len++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    buf->data[buf->len++] = (uint8_t)v;
}

/* ========== Internal Encode (recursive) ========== */

static int encode_value(XrayIsolate *X, XrValue value, XrSerialBuf *buf, int depth) {
    if (depth > XR_SERIAL_MAX_DEPTH || buf->error) return -1;

    switch (value.tag) {
    case XR_TAG_NULL:
        buf_put_u8(buf, XR_STAG_NULL);
        return 0;

    case XR_TAG_BOOL:
        buf_put_u8(buf, XR_STAG_BOOL);
        buf_put_u8(buf, (uint8_t)value.i);
        return 0;

    case XR_TAG_I64: {
        buf_put_u8(buf, XR_STAG_INT);
        buf_put_varint(buf, zigzag_encode(value.i));
        return 0;
    }

    case XR_TAG_F64: {
        buf_put_u8(buf, XR_STAG_FLOAT);
        double fv = value.f;
        buf_ensure(buf, 8);
        memcpy(buf->data + buf->len, &fv, 8);
        buf->len += 8;
        return 0;
    }

    case XR_TAG_PTR: {
        if (!value.ptr) {
            buf_put_u8(buf, XR_STAG_NULL);
            return 0;
        }
        int heap_type = XR_GC_GET_TYPE((XrGCHeader *)value.ptr);

        switch (heap_type) {
        case XR_TSTRING: {
            XrString *str = XR_TO_STRING(value);
            buf_put_u8(buf, XR_STAG_STRING);
            buf_put_varint(buf, (uint64_t)str->length);
            buf_put_bytes(buf, str->data, str->length);
            return 0;
        }

        case XR_TARRAY:
        case XR_TARRAY_SLICE: {
            XrArray *arr = XR_TO_ARRAY(value);
            int32_t count = arr->length;

            // Typed arrays: batch memcpy for compact wire format
            switch (arr->elem_type) {
            case XR_ELEM_U8:
                buf_put_u8(buf, XR_STAG_BYTES);
                buf_put_varint(buf, (uint64_t)count);
                buf_put_bytes(buf, arr->data, (size_t)count);
                return 0;
            case XR_ELEM_I32:
            case XR_ELEM_U32:
                buf_put_u8(buf, XR_STAG_ARRAY_I32);
                buf_put_varint(buf, (uint64_t)count);
                buf_put_bytes(buf, arr->data, (size_t)count * 4);
                return 0;
            case XR_ELEM_I64:
            case XR_ELEM_U64:
                buf_put_u8(buf, XR_STAG_ARRAY_I64);
                buf_put_varint(buf, (uint64_t)count);
                buf_put_bytes(buf, arr->data, (size_t)count * 8);
                return 0;
            case XR_ELEM_F32:
                buf_put_u8(buf, XR_STAG_ARRAY_F32);
                buf_put_varint(buf, (uint64_t)count);
                buf_put_bytes(buf, arr->data, (size_t)count * 4);
                return 0;
            case XR_ELEM_F64:
                buf_put_u8(buf, XR_STAG_ARRAY_F64);
                buf_put_varint(buf, (uint64_t)count);
                buf_put_bytes(buf, arr->data, (size_t)count * 8);
                return 0;
            default:
                break;
            }

            // Generic array: per-element recursive encoding
            buf_put_u8(buf, XR_STAG_ARRAY);
            buf_put_varint(buf, (uint64_t)count);
            for (int32_t i = 0; i < count; i++) {
                XrValue elem = xr_array_get(arr, i);
                if (encode_value(X, elem, buf, depth + 1) != 0) return -1;
            }
            return 0;
        }

        case XR_TMAP: {
            XrMap *map = XR_TO_MAP(value);
            uint32_t count = map->count;
            buf_put_u8(buf, XR_STAG_MAP);
            buf_put_varint(buf, (uint64_t)count);

            // Iterate all non-empty nodes
            uint32_t total_nodes = xr_map_sizenode(map);
            uint32_t written = 0;
            for (uint32_t i = 0; i < total_nodes && written < count; i++) {
                XrMapNode *node = xr_map_node(map, i);
                if (XR_MAP_NODE_EMPTY(node)) continue;
                if (encode_value(X, node->key, buf, depth + 1) != 0) return -1;
                if (encode_value(X, node->value, buf, depth + 1) != 0) return -1;
                written++;
            }
            return 0;
        }

        case XR_TSET: {
            XrSet *set = (XrSet *)value.ptr;
            uint32_t count = set->count;
            buf_put_u8(buf, XR_STAG_SET);
            buf_put_varint(buf, (uint64_t)count);

            uint32_t written = 0;
            for (uint32_t i = 0; i < set->capacity && written < count; i++) {
                if (set->entries[i].state & XR_SET_VALID) {
                    if (encode_value(X, set->entries[i].value, buf, depth + 1) != 0)
                        return -1;
                    written++;
                }
            }
            return 0;
        }

        case XR_TJSON: {
            // Direct binary encoding: [count 4B] [key_len 2B, key, value] ...
            XrJson *json = (XrJson *)value.ptr;
            buf_put_u8(buf, XR_STAG_JSON);

            {
                XrShape *shape = xr_json_shape(json);
                uint16_t count = shape->field_count;
                buf_put_varint(buf, (uint64_t)count);
                for (uint16_t i = 0; i < count; i++) {
                    SymbolId sym = shape->field_symbols[i];
                    const char *fname = xr_symbol_get_name_in_table(
                        X->symbol_table, sym);
                    if (!fname) fname = "";
                    size_t flen = strlen(fname);
                    buf_put_varint(buf, (uint64_t)flen);
                    buf_put_bytes(buf, fname, flen);
                    XrValue fval = xr_json_get_field_any(json, i);
                    if (encode_value(X, fval, buf, depth + 1) != 0) return -1;
                }
            }
            return 0;
        }

        default:
            // Non-serializable: Channel, Closure, Instance, etc.
            return -1;
        }
    }

    default:
        return -1;
    }
}

/* ========== Public Encode API ========== */

int xr_cluster_encode(XrayIsolate *X, XrValue value, XrSerialBuf *buf) {
    buf_put_u8(buf, XR_SERIAL_VERSION);
    int rc = encode_value(X, value, buf, 0);
    if (buf->error) return -1;
    if (rc != 0) return rc;

    // Append CRC32 of entire payload (including version byte)
    uint32_t crc = xr_crc32(buf->data, buf->len);
    buf_ensure(buf, 4);
    if (buf->error) return -1;
    buf->data[buf->len++] = (uint8_t)(crc & 0xFF);
    buf->data[buf->len++] = (uint8_t)((crc >> 8) & 0xFF);
    buf->data[buf->len++] = (uint8_t)((crc >> 16) & 0xFF);
    buf->data[buf->len++] = (uint8_t)((crc >> 24) & 0xFF);
    return 0;
}

/* ========== Decode Helpers ========== */

void xr_serial_reader_init(XrSerialReader *r, XrayIsolate *X,
                            const uint8_t *data, size_t len) {
    r->data = data;
    r->len = len;
    r->pos = 0;
    r->depth = 0;
    r->X = X;
}

static inline int reader_u8(XrSerialReader *r, uint8_t *out) {
    if (r->pos >= r->len) return -1;
    *out = r->data[r->pos++];
    return 0;
}

static inline int reader_bytes(XrSerialReader *r, size_t n, const uint8_t **out) {
    if (r->pos + n > r->len) return -1;
    *out = r->data + r->pos;
    r->pos += n;
    return 0;
}

static int reader_varint(XrSerialReader *r, uint64_t *out) {
    uint64_t result = 0;
    int shift = 0;
    for (;;) {
        if (r->pos >= r->len) return -1;
        uint8_t b = r->data[r->pos++];
        result |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift >= 64) return -1;
    }
    *out = result;
    return 0;
}

// Read varint and truncate to uint32_t
static inline int reader_varint32(XrSerialReader *r, uint32_t *out) {
    uint64_t v;
    if (reader_varint(r, &v) != 0) return -1;
    *out = (uint32_t)v;
    return 0;
}

/* ========== Internal Decode (recursive) ========== */

static int decode_value(XrSerialReader *r, XrValue *out) {
    if (r->depth > XR_SERIAL_MAX_DEPTH) return -1;

    uint8_t tag;
    if (reader_u8(r, &tag) != 0) return -1;

    switch (tag) {
    case XR_STAG_NULL:
        *out = xr_null();
        return 0;

    case XR_STAG_BOOL: {
        uint8_t bv;
        if (reader_u8(r, &bv) != 0) return -1;
        *out = bv ? XR_TRUE_VAL : XR_FALSE_VAL;
        return 0;
    }

    case XR_STAG_INT: {
        uint64_t zv;
        if (reader_varint(r, &zv) != 0) return -1;
        int64_t iv = zigzag_decode(zv);
        *out = xr_int(iv);
        return 0;
    }

    case XR_STAG_FLOAT: {
        const uint8_t *fp;
        if (reader_bytes(r, 8, &fp) != 0) return -1;
        double fv;
        memcpy(&fv, fp, 8);
        *out = xr_float(fv);
        return 0;
    }

    case XR_STAG_STRING: {
        uint32_t slen;
        if (reader_varint32(r, &slen) != 0) return -1;
        const uint8_t *sdata;
        if (reader_bytes(r, slen, &sdata) != 0) return -1;
        XrString *str = xr_string_intern(r->X, (const char *)sdata, slen, 0);
        if (!str) return -1;
        *out = xr_string_value(str);
        return 0;
    }

    case XR_STAG_BYTES: {
        uint32_t blen;
        if (reader_varint32(r, &blen) != 0) return -1;
        const uint8_t *bdata;
        if (reader_bytes(r, blen, &bdata) != 0) return -1;
        // Create a typed Array<uint8> (XR_ELEM_U8)
        XrArray *arr = xr_array_with_capacity_typed(NULL, (int)blen, XR_ELEM_U8);
        if (!arr) return -1;
        memcpy(arr->data, bdata, blen);
        arr->length = (int32_t)blen;
        *out = xr_value_from_array(arr);
        return 0;
    }

    case XR_STAG_ARRAY: {
        uint32_t count;
        if (reader_varint32(r, &count) != 0) return -1;
        XrArray *arr = xr_array_with_capacity(NULL, (int)count);
        if (!arr) return -1;
        r->depth++;
        for (uint32_t i = 0; i < count; i++) {
            XrValue elem;
            if (decode_value(r, &elem) != 0) { r->depth--; return -1; }
            xr_array_push(arr, elem);
        }
        r->depth--;
        *out = xr_value_from_array(arr);
        return 0;
    }

    case XR_STAG_MAP: {
        uint32_t count;
        if (reader_varint32(r, &count) != 0) return -1;
        XrMap *map = xr_map_with_capacity(NULL, count);
        if (!map) return -1;
        r->depth++;
        for (uint32_t i = 0; i < count; i++) {
            XrValue key, val;
            if (decode_value(r, &key) != 0) { r->depth--; return -1; }
            if (decode_value(r, &val) != 0) { r->depth--; return -1; }
            xr_map_set(map, key, val);
        }
        r->depth--;
        *out = xr_value_from_map(map);
        return 0;
    }

    case XR_STAG_SET: {
        uint32_t count;
        if (reader_varint32(r, &count) != 0) return -1;
        XrSet *set = xr_set_new_with_capacity(NULL, count);
        if (!set) return -1;
        r->depth++;
        for (uint32_t i = 0; i < count; i++) {
            XrValue elem;
            if (decode_value(r, &elem) != 0) { r->depth--; return -1; }
            xr_set_add(set, elem);
        }
        r->depth--;
        *out = xr_value_from_set(set);
        return 0;
    }

    case XR_STAG_ARRAY_I32: {
        uint32_t count;
        if (reader_varint32(r, &count) != 0) return -1;
        size_t byte_len = (size_t)count * 4;
        const uint8_t *raw;
        if (reader_bytes(r, byte_len, &raw) != 0) return -1;
        XrArray *arr = xr_array_with_capacity_typed(NULL, (int)count, XR_ELEM_I32);
        if (!arr) return -1;
        memcpy(arr->data, raw, byte_len);
        arr->length = (int32_t)count;
        *out = xr_value_from_array(arr);
        return 0;
    }

    case XR_STAG_ARRAY_I64: {
        uint32_t count;
        if (reader_varint32(r, &count) != 0) return -1;
        size_t byte_len = (size_t)count * 8;
        const uint8_t *raw;
        if (reader_bytes(r, byte_len, &raw) != 0) return -1;
        XrArray *arr = xr_array_with_capacity_typed(NULL, (int)count, XR_ELEM_I64);
        if (!arr) return -1;
        memcpy(arr->data, raw, byte_len);
        arr->length = (int32_t)count;
        *out = xr_value_from_array(arr);
        return 0;
    }

    case XR_STAG_ARRAY_F32: {
        uint32_t count;
        if (reader_varint32(r, &count) != 0) return -1;
        size_t byte_len = (size_t)count * 4;
        const uint8_t *raw;
        if (reader_bytes(r, byte_len, &raw) != 0) return -1;
        XrArray *arr = xr_array_with_capacity_typed(NULL, (int)count, XR_ELEM_F32);
        if (!arr) return -1;
        memcpy(arr->data, raw, byte_len);
        arr->length = (int32_t)count;
        *out = xr_value_from_array(arr);
        return 0;
    }

    case XR_STAG_ARRAY_F64: {
        uint32_t count;
        if (reader_varint32(r, &count) != 0) return -1;
        size_t byte_len = (size_t)count * 8;
        const uint8_t *raw;
        if (reader_bytes(r, byte_len, &raw) != 0) return -1;
        XrArray *arr = xr_array_with_capacity_typed(NULL, (int)count, XR_ELEM_F64);
        if (!arr) return -1;
        memcpy(arr->data, raw, byte_len);
        arr->length = (int32_t)count;
        *out = xr_value_from_array(arr);
        return 0;
    }

    case XR_STAG_JSON: {
        // Direct binary decoding: [count varint] [key_len varint, key, value] ...
        uint32_t count;
        if (reader_varint32(r, &count) != 0) return -1;
        XrJson *json = xr_json_new(NULL, (uint16_t)(count < 65535 ? count : 32));
        if (!json) return -1;
        r->depth++;
        for (uint32_t i = 0; i < count; i++) {
            // Read key length (varint)
            uint32_t klen;
            if (reader_varint32(r, &klen) != 0) { r->depth--; return -1; }
            const uint8_t *kdata;
            if (reader_bytes(r, klen, &kdata) != 0) { r->depth--; return -1; }
            // Read value
            XrValue val;
            if (decode_value(r, &val) != 0) { r->depth--; return -1; }
            // Set field by key name (heap alloc for long keys)
            char stack_key[256];
            char *key_buf = (klen < sizeof(stack_key))
                ? stack_key : (char *)xr_malloc(klen + 1);
            if (!key_buf) { r->depth--; return -1; }
            memcpy(key_buf, kdata, klen);
            key_buf[klen] = '\0';
            xr_json_set_by_key(r->X, json, key_buf, val);
            if (key_buf != stack_key) xr_free(key_buf);
        }
        r->depth--;
        *out = xr_json_value(json);
        return 0;
    }

    default:
        return -1;
    }
}

/* ========== Public Decode API ========== */

int xr_cluster_decode(XrSerialReader *r, XrValue *out) {
    // Need at least version(1) + tag(1) + crc(4)
    if (r->len < 6) return -1;

    // Verify CRC32 trailer: last 4 bytes are CRC of everything before
    size_t payload_len = r->len - 4;
    uint32_t expected_crc = (uint32_t)r->data[payload_len]
                          | ((uint32_t)r->data[payload_len + 1] << 8)
                          | ((uint32_t)r->data[payload_len + 2] << 16)
                          | ((uint32_t)r->data[payload_len + 3] << 24);
    uint32_t actual_crc = xr_crc32(r->data, payload_len);
    if (actual_crc != expected_crc) return -1;

    // Adjust reader length to exclude CRC trailer
    r->len = payload_len;

    uint8_t version;
    if (reader_u8(r, &version) != 0) return -1;
    if (version != XR_SERIAL_VERSION) return -1;
    return decode_value(r, out);
}
