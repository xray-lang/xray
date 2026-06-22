/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xblock_slot.c - Coroutine resume slot helpers.
 */

#include "xblock.h"

#include <stdint.h>

#include "../runtime/value/xvalue.h"
#include "xcoroutine.h"

XrValue *xr_slot_value_address(XrSlotRef slot) {
    switch (slot.kind) {
        case XR_SLOT_NONE:
            return NULL;
        case XR_SLOT_XVALUE_PTR:
            return (XrValue *) slot.base;
        case XR_SLOT_NATIVE_PTR:
            if (slot.type_id != XR_REP_TAGGED)
                return NULL;
            return (XrValue *) slot.base;
        case XR_SLOT_AOT_FRAME_OFFSET:
            if (slot.type_id != XR_REP_TAGGED)
                return NULL;
            if (!slot.base)
                return NULL;
            return (XrValue *) ((uint8_t *) slot.base + slot.offset);
        default:
            return NULL;
    }
}

bool xr_slot_store_value(XrSlotRef slot, XrValue value) {
    switch (slot.kind) {
        case XR_SLOT_NONE:
            return true;
        case XR_SLOT_XVALUE_PTR:
            if (!slot.base)
                return false;
            *(XrValue *) slot.base = value;
            return true;
        case XR_SLOT_NATIVE_PTR: {
            if (!slot.base)
                return false;
            if (slot.type_id == XR_REP_I64) {
                *(int64_t *) slot.base = XR_TO_INT(value);
                return true;
            }
            if (slot.type_id == XR_REP_F64) {
                *(double *) slot.base = XR_TO_FLOAT(value);
                return true;
            }
            *(XrValue *) slot.base = value;
            return true;
        }
        case XR_SLOT_AOT_FRAME_OFFSET: {
            if (!slot.base)
                return false;
            void *addr = (uint8_t *) slot.base + slot.offset;
            if (slot.type_id == XR_REP_I64) {
                *(int64_t *) addr = XR_TO_INT(value);
                return true;
            }
            if (slot.type_id == XR_REP_F64) {
                *(double *) addr = XR_TO_FLOAT(value);
                return true;
            }
            *(XrValue *) addr = value;
            return true;
        }
        default:
            return false;
    }
}

bool xr_slot_load_value(XrSlotRef slot, XrValue *out_value) {
    if (!out_value)
        return false;
    switch (slot.kind) {
        case XR_SLOT_NONE:
            *out_value = xr_null();
            return true;
        case XR_SLOT_XVALUE_PTR:
            if (!slot.base)
                return false;
            *out_value = *(XrValue *) slot.base;
            return true;
        case XR_SLOT_NATIVE_PTR:
            if (!slot.base)
                return false;
            if (slot.type_id == XR_REP_I64) {
                *out_value = xr_int(*(int64_t *) slot.base);
                return true;
            }
            if (slot.type_id == XR_REP_F64) {
                *out_value = xr_float(*(double *) slot.base);
                return true;
            }
            *out_value = *(XrValue *) slot.base;
            return true;
        case XR_SLOT_AOT_FRAME_OFFSET: {
            if (!slot.base)
                return false;
            void *addr = (uint8_t *) slot.base + slot.offset;
            if (slot.type_id == XR_REP_I64) {
                *out_value = xr_int(*(int64_t *) addr);
                return true;
            }
            if (slot.type_id == XR_REP_F64) {
                *out_value = xr_float(*(double *) addr);
                return true;
            }
            *out_value = *(XrValue *) addr;
            return true;
        }
        default:
            return false;
    }
}

bool xr_coro_store_recv_value(XrCoroutine *coro, XrValue value) {
    if (!coro)
        return false;
    XrCoroExt *ext = coro->ext;
    if (!ext)
        return false;
    if (ext->recv_slot_ref.kind != XR_SLOT_NONE)
        return xr_slot_store_value(ext->recv_slot_ref, value);
    if (ext->recv_slot) {
        *ext->recv_slot = value;
        return true;
    }
    return false;
}
