/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_serial.h - Binary serialization for XrValue
 *
 * KEY CONCEPT:
 *   Compact binary encoding for transmitting XrValue across cluster nodes.
 *   Supports all serializable types: null, bool, int, float, string,
 *   Array, Map, Set, Json. Non-serializable types (Channel, Closure,
 *   Instance) return an error.
 *
 * WIRE FORMAT:
 *   [Ver 1B] [Tag 1B] [Length 0-4B] [Payload ...]
 *   Version is currently 0x01.
 */

#ifndef XR_CLUSTER_SERIAL_H
#define XR_CLUSTER_SERIAL_H

#include "../../src/runtime/value/xvalue.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Protocol version
#define XR_SERIAL_VERSION  0x03

// Wire tags
#define XR_STAG_NULL       0x01
#define XR_STAG_BOOL       0x02
#define XR_STAG_INT        0x03
#define XR_STAG_FLOAT      0x04
#define XR_STAG_STRING     0x05
#define XR_STAG_BYTES      0x06
#define XR_STAG_ARRAY      0x07
#define XR_STAG_MAP        0x08
#define XR_STAG_SET        0x09
#define XR_STAG_JSON       0x0A
#define XR_STAG_ARRAY_I32  0x0B // Typed array int32, batch memcpy
#define XR_STAG_ARRAY_I64  0x0C // Typed array int64, batch memcpy
#define XR_STAG_ARRAY_F32  0x0D // Typed array float32, batch memcpy
#define XR_STAG_ARRAY_F64  0x0E // Typed array float64, batch memcpy

// Max serialization depth to prevent stack overflow on recursive structures
#define XR_SERIAL_MAX_DEPTH 128

// Forward declarations
struct XrayIsolate;

/* ========== Encode Buffer ========== */

/*
 * Growable byte buffer for encoding.
 * Caller should call xr_serial_buf_free() after use.
 */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    bool     error;  // set on allocation failure
} XrSerialBuf;

void xr_serial_buf_init(XrSerialBuf *buf);
void xr_serial_buf_free(XrSerialBuf *buf);

/* ========== Encode API ========== */

/*
 * Encode a single XrValue into binary format.
 *
 * Returns: 0 on success, -1 on error (non-serializable type, depth overflow)
 *
 * The isolate is needed for Json → JSON string conversion.
 */
int xr_cluster_encode(struct XrayIsolate *X, XrValue value, XrSerialBuf *buf);

/* ========== Decode API ========== */

/*
 * Decode context for reading from a byte buffer.
 */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    int            depth;
    struct XrayIsolate *X;
} XrSerialReader;

void xr_serial_reader_init(XrSerialReader *r, struct XrayIsolate *X,
                            const uint8_t *data, size_t len);

/*
 * Decode a single XrValue from binary data.
 *
 * Returns: 0 on success, -1 on error (corrupt data, depth overflow)
 * Decoded value is written to *out.
 */
int xr_cluster_decode(XrSerialReader *r, XrValue *out);

/* ========== Convenience Wrappers ========== */

/*
 * Encode value to a newly allocated buffer.
 * Caller must call xr_serial_buf_free() on the result.
 */
static inline int xr_cluster_encode_value(struct XrayIsolate *X, XrValue value,
                                           uint8_t **out_data, size_t *out_len) {
    XrSerialBuf buf;
    xr_serial_buf_init(&buf);
    int rc = xr_cluster_encode(X, value, &buf);
    if (rc == 0) {
        *out_data = buf.data;
        *out_len = buf.len;
    } else {
        xr_serial_buf_free(&buf);
        *out_data = NULL;
        *out_len = 0;
    }
    return rc;
}

/*
 * Decode value from a buffer.
 */
static inline int xr_cluster_decode_value(struct XrayIsolate *X,
                                           const uint8_t *data, size_t len,
                                           XrValue *out) {
    XrSerialReader r;
    xr_serial_reader_init(&r, X, data, len);
    return xr_cluster_decode(&r, out);
}

#endif
