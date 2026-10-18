/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_tid.c - Semantic type to public value tag mapping
 */
#include "xtype.h"
#include "xtype_names.h"
#include "../../base/xchecks.h"

XR_FUNC uint8_t xr_type_to_tid(const XrType *type) {
    if (!type)
        return 0;
    XR_DCHECK(type->kind < XR_KIND_COUNT, "type_to_tid: invalid kind");
    switch (type->kind) {
        case XR_KIND_UNKNOWN:
        case XR_KIND_ERROR:
            return 0;
        case XR_KIND_INT:
            switch (type->scalar_rep) {
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
                    return XR_TID_U64;
                case XR_NATIVE_ISIZE:
                    return XR_TID_ISIZE;
                case XR_NATIVE_USIZE:
                    return XR_TID_USIZE;
                default:
                    break;
            }
            return XR_TID_I64;
        case XR_KIND_FLOAT:
            if (type->scalar_rep == XR_NATIVE_F32)
                return XR_TID_F32;
            return XR_TID_F64;
        case XR_KIND_STRING:
            return XR_TID_STRING;
        case XR_KIND_BOOL:
            return XR_TID_BOOL;
        case XR_KIND_RUNE:
            return XR_TID_RUNE;
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
            return XR_TID_ARRAY;
        case XR_KIND_MAP:
            return XR_TID_MAP;
        case XR_KIND_SET:
            return XR_TID_SET;
        case XR_KIND_CHANNEL:
            return XR_TID_CHANNEL;
        case XR_KIND_JSON:
            /* Not XR_TID_OBJECT. A Json value is any member of the domain, and
             * the object form is only one of seven; answering with the object
             * id turns every membership question into "is it an object", which
             * a string- or number-valued Json fails. */
            return XR_TID_JSON;
        case XR_KIND_STRUCT_OBJECT:
            return XR_TID_OBJECT;
        case XR_KIND_INSTANCE:
            return XR_TID_INSTANCE;
        case XR_KIND_FUNCTION:
            return XR_TID_FUNCTION;
        case XR_KIND_ENUM:
            return XR_TID_ENUM_VALUE;
        default:
            return 0;
    }
}
