/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_jit_internal.h - Shared inline helpers for xir_jit.c / xir_jit_runtime.c
 *
 * KEY CONCEPT:
 *   Value reconstruction utilities used by both the JIT call/deopt path
 *   and the C-bridge runtime helpers.  Kept in a single header to avoid
 *   duplicating 200+ lines of tag-dispatch logic.
 */

#ifndef XIR_JIT_INTERNAL_H
#define XIR_JIT_INTERNAL_H

#include "xir_jit_runtime.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/gc/xgc.h"
#include <string.h>

/* ========== JIT function pointer type ========== */

#ifdef _WIN32
/* Win64: JIT code returns payload in RAX only; tag is written to
 * jit_ctx->call_result_tag by the JIT epilogue. */
typedef int64_t (*XirJitFn)(intptr_t, int64_t *);
#else
/* System V: JIT code returns XrJitResult in RAX(payload) + RDX(tag). */
typedef XrJitResult (*XirJitFn)(intptr_t, int64_t *);
#endif

/* ========== Convenience alias ========== */

#define XR_JIT_RESULT(val) XR_JIT_VAL(val)

/* ========== Tag conversion ========== */

static inline uint8_t slot_type_to_xr_tag(uint8_t slot_type) {
    switch (slot_type) {
        case XR_SLOT_I64:
            return XR_TAG_I64;
        case XR_SLOT_F64:
            return XR_TAG_F64;
        case XR_SLOT_PTR:
            return XR_TAG_PTR;
        case XR_SLOT_BOOL:
            return XR_TAG_BOOL;
        default:
            return XR_RTAG_UNKNOWN;
    }
}

/* ========== Value reconstruction from raw + tag ========== */

// Reconstruct XrValue from raw payload + xr_tag.
// When xr_tag is known (0-15), reconstruction is exact with zero heuristics.
// UNKNOWN (0xFF) defaults to I64 — no address-range guessing.
static inline XrValue jit_value_from_tag(int64_t raw, uint8_t xr_tag) {
    XrValue v;
    v.descriptor = 0;
    switch (xr_tag) {
        case XR_TAG_F64:
            memcpy(&v.f, &raw, sizeof(double));
            v.tag = XR_TAG_F64;
            break;
        case XR_TAG_NULL:
            v.tag = XR_TAG_NULL;
            v.i = 0;
            break;
        case XR_TAG_BOOL:
            v.i = raw;
            v.tag = XR_TAG_BOOL;
            break;
        case XR_TAG_PTR:
            if (raw == 0) {
                v.tag = XR_TAG_NULL;
                v.i = 0;
            } else if ((uint64_t) raw >= 0x1000 && (raw & 0x7) == 0) {
                v.ptr = (void *) raw;
                v.tag = XR_TAG_PTR;
                v.heap_type = (uint16_t) ((XrGCHeader *) v.ptr)->type;
            } else {
                v.i = raw;
                v.tag = XR_TAG_I64;
            }
            break;
        case XR_RTAG_NUMERIC:
        case XR_RTAG_UNKNOWN:
            v.i = raw;
            v.tag = XR_TAG_I64;
            break;
        default:
            v.i = raw;
            v.tag = xr_tag;
            break;
    }
    return v;
}

/* ========== Deopt value reconstruction ========== */

// Reconstruct XrValue from deopt slot: raw payload + XIR rep + xr_tag hint.
// When xr_tag is known (0-15), reconstruction is exact — no address heuristic.
// Callers should resolve xr_tag from slot_runtime_tags before calling this.
static inline XrValue deopt_reconstruct(int64_t raw, uint8_t xir_type, uint8_t xr_tag) {
    XrValue v;
    v.descriptor = 0;

    // Treat NUMERIC the same as UNKNOWN (no precise tag)
    if (xr_tag == XR_RTAG_NUMERIC) {
        xr_tag = XR_RTAG_UNKNOWN;
    }

    // Known tag: trust unconditionally, no address-range heuristic
    if (xr_tag != XR_RTAG_UNKNOWN) {
        if (xr_tag == XR_TAG_F64) {
            memcpy(&v.f, &raw, sizeof(double));
            v.tag = XR_TAG_F64;
        } else if (xr_tag == XR_TAG_PTR) {
            v.i = raw;
            if (raw == 0) {
                v.tag = XR_TAG_NULL;
            } else {
                v.tag = XR_TAG_PTR;
                if ((raw & 0x7) == 0) {
                    XrGCHeader *gc = (XrGCHeader *) (intptr_t) raw;
                    v.heap_type = (uint16_t) gc->type;
                }
            }
        } else {
            v.i = raw;
            v.tag = xr_tag;
        }
        return v;
    }

    // Unknown tag: infer from XIR representation type
    switch (xir_type) {
        case XR_REP_F64:
            memcpy(&v.f, &raw, sizeof(double));
            v.tag = XR_TAG_F64;
            break;
        case XR_REP_PTR:
            v.i = raw;
            if (raw == 0) {
                v.tag = XR_TAG_NULL;
            } else if ((raw & 0x7) == 0) {
                v.tag = XR_TAG_PTR;
                XrGCHeader *gc = (XrGCHeader *) (intptr_t) raw;
                v.heap_type = (uint16_t) gc->type;
            } else {
                v.tag = XR_TAG_I64;
            }
            break;
        case XR_REP_TAGGED:
        default:
            // Unknown rep + unknown tag: safe default to I64.
            // Never guess pointer from address range — an integer that happens
            // to be aligned > 0x10000 would SIGSEGV when dereferencing GC header.
            // The interpreter will do proper type checking after deopt.
            v.i = raw;
            v.tag = (raw == 0) ? XR_TAG_NULL : XR_TAG_I64;
            break;
    }
    return v;
}

/* ========== Tag bitmap decode ========== */

// Decode xr_tag from a 64-bit tag bitmap stored in call_args[15].
// Each slot occupies 4 bits: slot i is at bits[(i+1)*4-1 : i*4].
static inline uint8_t jit_bitmap_tag(int64_t bitmap, int slot) {
    uint8_t nibble = (uint8_t) ((bitmap >> (slot * 4)) & 0xF);
    if (nibble <= 7)
        return nibble;
    return XR_RTAG_UNKNOWN;
}

#endif  // XIR_JIT_INTERNAL_H
