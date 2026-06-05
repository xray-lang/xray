/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xslot_ref.h - Backend-neutral suspended result slots
 */

#ifndef XSLOT_REF_H
#define XSLOT_REF_H

#include <stdint.h>

#ifdef XRT_VALUE_H
#ifndef XR_REP_I64
#define XR_REP_I64 0
#define XR_REP_F64 1
#define XR_REP_PTR 2
#define XR_REP_TAGGED 3
#define XR_REP_VOID 4
#define XR_REP_STR 5
#endif
#else
#include "../runtime/value/xtype.h"
#endif

typedef enum {
    XR_SLOT_NONE = 0,
    XR_SLOT_XVALUE_PTR,
    XR_SLOT_AOT_FRAME_OFFSET,
    XR_SLOT_JIT_SUSPEND
} XrSlotKind;

typedef struct {
    XrSlotKind kind;
    void *base;
    uint32_t offset;
    uint16_t type_id;
} XrSlotRef;

static inline XrSlotRef xr_slot_none(void) {
    XrSlotRef slot = {XR_SLOT_NONE, NULL, 0, 0};
    return slot;
}

static inline XrSlotRef xr_slot_xvalue_ptr(XrValue *ptr) {
    XrSlotRef slot = {XR_SLOT_XVALUE_PTR, ptr, 0, XR_REP_TAGGED};
    return slot;
}

static inline XrSlotRef xr_slot_aot_frame_offset(void *base, uint32_t offset, uint16_t type_id) {
    XrSlotRef slot = {XR_SLOT_AOT_FRAME_OFFSET, base, offset, type_id};
    return slot;
}

static inline XrSlotRef xr_slot_jit_suspend(void *base, uint32_t offset, uint16_t type_id) {
    XrSlotRef slot = {XR_SLOT_JIT_SUSPEND, base, offset, type_id};
    return slot;
}

#endif  // XSLOT_REF_H
