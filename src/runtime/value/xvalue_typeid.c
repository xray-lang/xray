/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_typeid.c - XrValue/XrType to public type id mapping.
 */

#include "xvalue.h"
#include "xtype.h"
#include "xtype_names.h"
#include "../../base/xchecks.h"
#include "../class/xinstance.h"

static const XrTypeId tag_to_typeid[8] = {
    [XR_TAG_NULL] = XR_TID_NULL,       [XR_TAG_BOOL] = XR_TID_BOOL,     [XR_TAG_CHAR] = XR_TID_CHAR,
    [XR_TAG_I64] = XR_TID_INT,         [XR_TAG_F64] = XR_TID_FLOAT,     [XR_TAG_PTR] = XR_TID_NULL,
    [XR_TAG_STRUCT_REF] = XR_TID_NULL, [XR_TAG_NOTFOUND] = XR_TID_NULL,
};

static const XrTypeId gctype_to_typeid[XR_TRESULTGROUP + 1] = {
    [XR_TNULL] = XR_TID_NULL,
    [XR_TBOOL] = XR_TID_BOOL,
    [XR_TINT] = XR_TID_INT,
    [XR_TFLOAT] = XR_TID_FLOAT,
    [XR_TSTRING] = XR_TID_STRING,
    [XR_TFUNCTION] = XR_TID_FUNCTION,
    [XR_TCFUNCTION] = XR_TID_FUNCTION,
    [XR_TARRAY] = XR_TID_ARRAY,
    [XR_TSET] = XR_TID_SET,
    [XR_TMAP] = XR_TID_MAP,
    [XR_TCLASS] = XR_TID_FUNCTION,
    [XR_TINSTANCE] = XR_TID_INSTANCE,
    [XR_TBOUND_METHOD] = XR_TID_BOUND_METHOD,
    [XR_TERROR] = XR_TID_PANIC_INFO,
    [XR_TMODULE] = XR_TID_MODULE,
    [XR_TCOROUTINE] = XR_TID_COROUTINE,
    [XR_TCHANNEL] = XR_TID_CHANNEL,
    [XR_TCOROPOOL] = XR_TID_NULL,
    [XR_TTASK] = XR_TID_TASK,
    [XR_TATOMIC] = XR_TID_ATOMIC,
    [XR_TWORKQUEUE] = XR_TID_WORKQUEUE,
    [XR_TRESULTGROUP] = XR_TID_RESULTGROUP,
};

XrTypeId xr_value_typeid(XrValue v) {
    XR_DCHECK(v.tag <= XR_TAG_NOTFOUND, "value_typeid: invalid tag");
    if (v.tag <= XR_TAG_F64)
        return tag_to_typeid[v.tag];
    if (v.tag == XR_TAG_PTR && v.ptr) {
        uint8_t gctype = XR_OBJ_GET_TYPE((XrObjHeader *) v.ptr);
        if (gctype < sizeof(gctype_to_typeid) / sizeof(gctype_to_typeid[0])) {
            XrTypeId tid = gctype_to_typeid[gctype];
            if (tid == XR_TID_INSTANCE) {
                XrInstance *inst = (XrInstance *) v.ptr;
                if (inst->klass) {
                    switch (inst->klass->builtin_kind) {
                        case XR_BK_JSON:
                            return XR_TID_JSON;
                        case XR_BK_RECORD:
                            return XR_TID_RECORD;
                        case XR_BK_STRINGBUILDER:
                            return XR_TID_STRINGBUILDER;
                        case XR_BK_ENUM_VALUE:
                            return XR_TID_ENUM_VALUE;
                        case XR_BK_ENUM_TYPE:
                            return XR_TID_ENUM_TYPE;
                        case XR_BK_ITERATOR:
                            return XR_TID_ITERATOR;
                        case XR_BK_REGEX:
                            return XR_TID_REGEX;
                        case XR_BK_NETCONN:
                            return XR_TID_NETCONN;
                        case XR_BK_NETLISTENER:
                            return XR_TID_NETLISTENER;
                        case XR_BK_BIGINT:
                            return XR_TID_BIGINT;
                        case XR_BK_PANIC_INFO:
                            return XR_TID_PANIC_INFO;
                        case XR_BK_RANGE:
                            return XR_TID_RANGE;
                        case XR_BK_DATETIME:
                            return XR_TID_DATETIME;
                        default:
                            break;
                    }
                }
            }
            return tid;
        }
    }
    return XR_TID_NULL;
}

XR_DATADEF const char *typeid_names[XR_TID_COUNT] = {
    [XR_TID_NULL] = TYPE_NAME_NULL,
    [XR_TID_BOOL] = TYPE_NAME_BOOL,
    [XR_TID_INT8] = TYPE_NAME_INT8,
    [XR_TID_UINT8] = TYPE_NAME_UINT8,
    [XR_TID_INT16] = TYPE_NAME_INT16,
    [XR_TID_UINT16] = TYPE_NAME_UINT16,
    [XR_TID_INT32] = TYPE_NAME_INT32,
    [XR_TID_UINT32] = TYPE_NAME_UINT32,
    [XR_TID_INT] = TYPE_NAME_INT,
    [XR_TID_UINT64] = TYPE_NAME_UINT64,
    [XR_TID_FLOAT32] = TYPE_NAME_FLOAT32,
    [XR_TID_FLOAT] = TYPE_NAME_FLOAT,
    [XR_TID_STRING] = TYPE_NAME_STRING,
    [XR_TID_CHAR] = TYPE_NAME_CHAR,
    [XR_TID_RECORD] = TYPE_NAME_RECORD,
    [XR_TID_FUNCTION] = TYPE_NAME_FUNCTION,
    [XR_TID_ARRAY] = TYPE_NAME_ARRAY,
    [XR_TID_SET] = TYPE_NAME_SET,
    [XR_TID_MAP] = TYPE_NAME_MAP,
    [XR_TID_INSTANCE] = TYPE_NAME_INSTANCE,
    [XR_TID_JSON] = TYPE_NAME_JSON,
    [XR_TID_BIGINT] = TYPE_NAME_BIGINT,
    [XR_TID_STRINGBUILDER] = TYPE_NAME_STRINGBUILDER,
    [XR_TID_CHANNEL] = TYPE_NAME_CHANNEL,
    [XR_TID_REGEX] = TYPE_NAME_REGEX,
    [XR_TID_DATETIME] = TYPE_NAME_DATETIME,
    [XR_TID_PANIC_INFO] = TYPE_NAME_PANIC_INFO,
    [XR_TID_ENUM_VALUE] = TYPE_NAME_ENUM_VALUE,
    [XR_TID_ENUM_TYPE] = TYPE_NAME_ENUM_TYPE,
    [XR_TID_BOUND_METHOD] = TYPE_NAME_FUNCTION,
    [XR_TID_ITERATOR] = TYPE_NAME_ITERATOR,
    [XR_TID_MODULE] = TYPE_NAME_MODULE,
    [XR_TID_COROUTINE] = TYPE_NAME_COROUTINE,
    [XR_TID_RANGE] = TYPE_NAME_RANGE,
    [XR_TID_TASK] = TYPE_NAME_TASK,
    [XR_TID_NETCONN] = TYPE_NAME_NETCONN,
    [XR_TID_NETLISTENER] = TYPE_NAME_NETLISTENER,
    [XR_TID_ATOMIC] = TYPE_NAME_ATOMIC,
    [XR_TID_WORKQUEUE] = TYPE_NAME_WORKQUEUE,
    [XR_TID_RESULTGROUP] = TYPE_NAME_RESULTGROUP,
};

const char *xr_typeid_name(XrTypeId tid) {
    if (tid >= 0 && tid < XR_TID_COUNT && typeid_names[tid])
        return typeid_names[tid];
    return TYPE_NAME_UNKNOWN;
}

uint8_t xr_type_to_tid(const XrType *type) {
    if (!type)
        return 0;
    XR_DCHECK(type->kind < XR_KIND_COUNT, "type_to_tid: invalid kind");
    switch (type->kind) {
        case XR_KIND_UNKNOWN:
            return 0;
        case XR_KIND_INT:
            switch (type->native_width) {
                case XR_NATIVE_I8:
                    return XR_TID_INT8;
                case XR_NATIVE_U8:
                    return XR_TID_UINT8;
                case XR_NATIVE_I16:
                    return XR_TID_INT16;
                case XR_NATIVE_U16:
                    return XR_TID_UINT16;
                case XR_NATIVE_I32:
                    return XR_TID_INT32;
                case XR_NATIVE_U32:
                    return XR_TID_UINT32;
                case XR_NATIVE_U64:
                    return XR_TID_UINT64;
                default:
                    break;
            }
            return XR_TID_INT;
        case XR_KIND_FLOAT:
            if (type->native_width == XR_NATIVE_F32)
                return XR_TID_FLOAT32;
            return XR_TID_FLOAT;
        case XR_KIND_STRING:
            return XR_TID_STRING;
        case XR_KIND_BOOL:
            return XR_TID_BOOL;
        case XR_KIND_CHAR:
            return XR_TID_CHAR;
        case XR_KIND_ARRAY:
            return XR_TID_ARRAY;
        case XR_KIND_MAP:
            return XR_TID_MAP;
        case XR_KIND_SET:
            return XR_TID_SET;
        case XR_KIND_CHANNEL:
            return XR_TID_CHANNEL;
        case XR_KIND_JSON:
            return XR_TID_JSON;
        case XR_KIND_RECORD:
            return XR_TID_RECORD;
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
