/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_names.h - Runtime-side view of the public type name vocabulary.
 *
 * KEY CONCEPT:
 *   Avoids hardcoded type name strings.
 *   Primitives (int/float/string/bool/null) lowercase.
 *   Object types (Array/Map/BigInt/DateTime etc.) PascalCase.
 *
 * The TYPE_NAME_* spellings, the XrTypeId enum and the id -> name mapping all
 * come from the shared kernel xr_type_names_core.h; this header adds only what
 * needs the runtime's scalar representation vocabulary.
 */

#ifndef XTYPE_NAMES_H
#define XTYPE_NAMES_H

#include <stdint.h>
#include "../../base/xdefs.h"
#include "../../shared/xr_scalar_type.h"
#include "../../shared/xr_type_names_core.h"

/* The dynamic tag carries only the i64/f64 family; the static width lives in
 * the scalar representation. Fixed-width type tests need the way back from a
 * public type id to that representation. Returns XR_SCALAR_REP_NONE for ids
 * that name no scalar. */
static inline uint8_t xr_typeid_scalar_rep(XrTypeId tid) {
    switch (tid) {
        case XR_TID_I8:
            return XR_NATIVE_I8;
        case XR_TID_U8:
            return XR_NATIVE_U8;
        case XR_TID_I16:
            return XR_NATIVE_I16;
        case XR_TID_U16:
            return XR_NATIVE_U16;
        case XR_TID_I32:
            return XR_NATIVE_I32;
        case XR_TID_U32:
            return XR_NATIVE_U32;
        case XR_TID_INT:
            return XR_NATIVE_I64;
        case XR_TID_U64:
            return XR_NATIVE_U64;
        case XR_TID_F32:
            return XR_NATIVE_F32;
        case XR_TID_FLOAT:
            return XR_NATIVE_F64;
        default:
            return XR_SCALAR_REP_NONE;
    }
}

/* Inverse of xr_typeid_scalar_rep. isize/usize have no public id of their own
 * and collapse onto the widest id of their signedness, which is exact on 64-bit
 * targets and the closest available answer elsewhere. */
static inline XrTypeId xr_scalar_rep_typeid(uint8_t scalar_rep) {
    switch ((XrNativeType) scalar_rep) {
        case XR_NATIVE_I8:
            return XR_TID_I8;
        case XR_NATIVE_U8:
            return XR_TID_U8;
        case XR_NATIVE_I16:
            return XR_TID_I16;
        case XR_NATIVE_U16:
            return XR_TID_U16;
        case XR_NATIVE_I32:
            return XR_TID_I32;
        case XR_NATIVE_U32:
            return XR_TID_U32;
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
            return XR_TID_U64;
        case XR_NATIVE_F32:
            return XR_TID_F32;
        case XR_NATIVE_F64:
            return XR_TID_FLOAT;
        case XR_NATIVE_I64:
        case XR_NATIVE_ISIZE:
        default:
            return XR_TID_INT;
    }
}

/* ========== Utility Functions ========== */

XR_FUNC int xr_type_from_name(const char *type_name);
XR_FUNC int xr_is_valid_type_name(const char *type_name);

// Type ID → name string (defined in xvalue.c)
XR_FUNC const char *xr_typeid_name(XrTypeId tid);

// XrType kind → XrTypeId (for reified generics, defined in xvalue.c)
struct XrType;
XR_FUNC uint8_t xr_type_to_tid(const struct XrType *type);

#endif  // XTYPE_NAMES_H
