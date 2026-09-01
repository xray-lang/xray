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
#include "../../shared/xr_numeric_conversion_core.h"
#include "../../shared/xr_type_identity_core.h"
#include "../../base/xchecks.h"
#include "../class/xenum.h"
#include "../class/xinstance.h"
#include "../object/xarray.h" /* elem_tid carries a Json array's domain */

#define XR_TYPE_IDENTITY_ASSERT_PUBLIC_ID(core_id, public_id, numeric_id)                          \
    _Static_assert((unsigned) (core_id) == (numeric_id), "type identity core id drifted");         \
    _Static_assert((unsigned) (public_id) == (numeric_id), "public type id drifted")

XR_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_NULL, XR_TID_NULL, 0u);
XR_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_BOOL, XR_TID_BOOL, 1u);
XR_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_I64, XR_TID_I64, 8u);
XR_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_F64, XR_TID_F64, 11u);
XR_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_BUFFER, XR_TID_BUFFER, 42u);
XR_TYPE_IDENTITY_ASSERT_PUBLIC_ID(XR_TYPE_IDENTITY_CORE_RUNE, XR_TID_RUNE, 43u);

#undef XR_TYPE_IDENTITY_ASSERT_PUBLIC_ID

static const XrTypeIdentityCoreKind tag_to_type_identity[8] = {
    [XR_TAG_NULL] = XR_TYPE_IDENTITY_CORE_NULL,    [XR_TAG_BOOL] = XR_TYPE_IDENTITY_CORE_BOOL,
    [XR_TAG_RUNE] = XR_TYPE_IDENTITY_CORE_RUNE,    [XR_TAG_I64] = XR_TYPE_IDENTITY_CORE_I64,
    [XR_TAG_F64] = XR_TYPE_IDENTITY_CORE_F64,    [XR_TAG_PTR] = XR_TYPE_IDENTITY_CORE_NULL,
    [XR_TAG_AGG_REF] = XR_TYPE_IDENTITY_CORE_NULL, [XR_TAG_NOTFOUND] = XR_TYPE_IDENTITY_CORE_NULL,
};

/* Designated initializers cannot be pinned complete by the array bound alone —
 * a builtin appended to XrObjType would just widen the table with a zeroed
 * (XR_TYPE_IDENTITY_CORE_NULL) slot and read as deliberately unmapped.
 * Re-anchor this count by hand after adding the new type's entry below. */
_Static_assert(XR_OBJ_TYPE_BUILTIN_COUNT == 35,
               "XrObjType gained a member: give it a kind in gctype_to_type_identity "
               "(XR_TYPE_IDENTITY_CORE_NULL if it is internal and never surfaces as a "
               "value), then re-anchor this count");

static const XrTypeIdentityCoreKind gctype_to_type_identity[XR_OBJ_TYPE_BUILTIN_COUNT] = {
    [XR_TNULL] = XR_TYPE_IDENTITY_CORE_NULL,
    [XR_TBOOL] = XR_TYPE_IDENTITY_CORE_BOOL,
    [XR_TINT] = XR_TYPE_IDENTITY_CORE_I64,
    [XR_TFLOAT] = XR_TYPE_IDENTITY_CORE_F64,
    [XR_TSTRING] = XR_TYPE_IDENTITY_CORE_STRING,
    [XR_TFUNCTION] = XR_TYPE_IDENTITY_CORE_FUNCTION,
    [XR_TCFUNCTION] = XR_TYPE_IDENTITY_CORE_FUNCTION,
    [XR_TARRAY] = XR_TYPE_IDENTITY_CORE_ARRAY,
    [XR_TSET] = XR_TYPE_IDENTITY_CORE_SET,
    [XR_TMAP] = XR_TYPE_IDENTITY_CORE_MAP,
    [XR_TCLASS] = XR_TYPE_IDENTITY_CORE_FUNCTION,
    [XR_TINSTANCE] = XR_TYPE_IDENTITY_CORE_INSTANCE,
    [XR_TBOUND_METHOD] = XR_TYPE_IDENTITY_CORE_BOUND_METHOD,
    [XR_TERROR] = XR_TYPE_IDENTITY_CORE_PANIC_INFO,
    [XR_TMODULE] = XR_TYPE_IDENTITY_CORE_MODULE,
    [XR_TCOROUTINE] = XR_TYPE_IDENTITY_CORE_COROUTINE,
    [XR_TCHANNEL] = XR_TYPE_IDENTITY_CORE_CHANNEL,
    [XR_TCOROPOOL] = XR_TYPE_IDENTITY_CORE_NULL,
    [XR_TTASK] = XR_TYPE_IDENTITY_CORE_TASK,
    [XR_TATOMIC] = XR_TYPE_IDENTITY_CORE_ATOMIC,
    [XR_TWORKQUEUE] = XR_TYPE_IDENTITY_CORE_WORKQUEUE,
    [XR_TRESULTGROUP] = XR_TYPE_IDENTITY_CORE_RESULTGROUP,
    [XR_TBOOLMAP] = XR_TYPE_IDENTITY_CORE_MAP,
    [XR_TCOUNTDOWNLATCH] = XR_TYPE_IDENTITY_CORE_COUNTDOWNLATCH,
    [XR_TSEMAPHORE] = XR_TYPE_IDENTITY_CORE_SEMAPHORE,
    [XR_TEVENTCOUNT] = XR_TYPE_IDENTITY_CORE_EVENTCOUNT,
    [XR_TTHREAD] = XR_TYPE_IDENTITY_CORE_THREAD,
    [XR_TENUM_TYPE] = XR_TYPE_IDENTITY_CORE_ENUM_TYPE,
    [XR_TENUM_CTOR] = XR_TYPE_IDENTITY_CORE_FUNCTION,
    [XR_TENUM_DESCRIPTOR] = XR_TYPE_IDENTITY_CORE_INSTANCE,
    [XR_TENUM_SCALAR_LAYOUT] = XR_TYPE_IDENTITY_CORE_ENUM_VALUE,
    /* Internal storage, like XR_TCOROPOOL above: a weak field holds the handle,
     * but every user-visible read goes through xr_weak_field_load, which hands
     * back the target or null. Seeing the raw handle here means someone typed
     * the slot instead of the load, and null is the honest answer for both a
     * cleared handle and one whose target was never asked for. */
    [XR_TWEAK_HANDLE] = XR_TYPE_IDENTITY_CORE_NULL,
    /* A payload-carrying enum member that crossed a tagged boundary. Same
     * answer as the other two enum-member forms, XR_TENUM_SCALAR_LAYOUT above
     * and XR_BK_ADT_ENUM below — the boxing is a representation choice, not a
     * different type to the program. */
    [XR_TENUM_BOX] = XR_TYPE_IDENTITY_CORE_ENUM_VALUE,
};

static XrTypeIdentityCoreKind xr_value_type_identity_kind(XrValue v) {
    XR_DCHECK(v.tag <= XR_TAG_NOTFOUND, "value_typeid: invalid tag");
    if (v.tag <= XR_TAG_F64)
        return tag_to_type_identity[v.tag];
    if (v.tag == XR_TAG_PTR && v.ptr) {
        /* Canonical String objects carry the heap type in XrValue; consulting
         * that representation field is adapter work, not a semantic fallback. */
        if (v.heap_type == XR_TSTRING)
            return XR_TYPE_IDENTITY_CORE_STRING;
        uint8_t gctype = XR_OBJ_GET_TYPE((XrObjHeader *) v.ptr);
        if (gctype < sizeof(gctype_to_type_identity) / sizeof(gctype_to_type_identity[0])) {
            XrTypeIdentityCoreKind kind = gctype_to_type_identity[gctype];
            if (kind == XR_TYPE_IDENTITY_CORE_INSTANCE) {
                XrObjectInstance *inst = (XrObjectInstance *) v.ptr;
                if (inst->klass) {
                    switch (inst->klass->builtin_kind) {
                        case XR_BK_STRUCT_OBJECT:
                            return XR_TYPE_IDENTITY_CORE_OBJECT;
                        case XR_BK_STRINGBUILDER:
                            return XR_TYPE_IDENTITY_CORE_STRINGBUILDER;
                        case XR_BK_ADT_ENUM:
                            return XR_TYPE_IDENTITY_CORE_ENUM_VALUE;
                        case XR_BK_ITERATOR:
                            return XR_TYPE_IDENTITY_CORE_ITERATOR;
                        case XR_BK_REGEX:
                            return XR_TYPE_IDENTITY_CORE_REGEX;
                        case XR_BK_NETCONN:
                            return XR_TYPE_IDENTITY_CORE_NETCONN;
                        case XR_BK_NETLISTENER:
                            return XR_TYPE_IDENTITY_CORE_NETLISTENER;
                        case XR_BK_BIGINT:
                            return XR_TYPE_IDENTITY_CORE_BIGINT;
                        case XR_BK_PANIC_INFO:
                            return XR_TYPE_IDENTITY_CORE_PANIC_INFO;
                        case XR_BK_RANGE:
                            return XR_TYPE_IDENTITY_CORE_RANGE;
                        case XR_BK_BUFFER:
                            return XR_TYPE_IDENTITY_CORE_BUFFER;
                        default:
                            break;
                    }
                }
            }
            return kind;
        }
    }
    return XR_TYPE_IDENTITY_CORE_NULL;
}

XrTypeId xr_value_typeid(XrValue v) {
    return (XrTypeId) xr_type_identity_core_eval(
        XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI, XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO,
        XR_SEM_CONSUMER_RUNTIME, xr_value_type_identity_kind(v));
}

XrTypeId xr_value_typeid_vm(XrValue v) {
    return (XrTypeId) xr_type_identity_core_eval(
        XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_HI, XR_SEM_OWNER_ID_PRIMITIVE_TYPE_IDENTITY_LO,
        XR_SEM_CONSUMER_VM, xr_value_type_identity_kind(v));
}

/* Membership in JSON.Value is a constant-time tag/provenance read. Objects use
 * the JSON.Object Map representation; arrays carry JSON element provenance. */
bool xr_value_in_json_domain(XrValue v) {
    XrTypeId tid = xr_value_typeid(v);
    switch (tid) {
        case XR_TID_NULL:
        case XR_TID_BOOL:
        case XR_TID_STRING:
            return true;
        case XR_TID_MAP:
            return true;
        case XR_TID_ARRAY: {
            const XrArray *arr = (v.tag == XR_TAG_PTR) ? (const XrArray *) v.ptr : NULL;
            return arr && arr->elem_tid == XR_TID_JSON;
        }
        default:
            return XR_TID_IS_NUMBER(tid);
    }
}

bool xr_value_is_type_id(XrValue v, XrTypeId tid) {
    /* Json names a domain, not a tag, so membership cannot be a tag compare.
     * Asking it that way is what made a string-valued Json answer `false` to
     * `is Json`. */
    if (tid == XR_TID_JSON)
        return xr_value_in_json_domain(v);
    uint8_t rep = xr_typeid_scalar_rep(tid);
    if (rep != XR_SCALAR_REP_NONE) {
        if (xr_scalar_rep_is_integer(rep))
            return XR_IS_INT(v) && xr_scalar_rep_holds_i64(rep, XR_TO_INT(v));
        return XR_IS_FLOAT(v) && xr_scalar_rep_holds_f64(rep, XR_TO_FLOAT(v));
    }
    return xr_value_typeid(v) == tid;
}

/* Only the reverse lookup (xr_type_from_name) needs the names in array form.
 * Both this and the forward mapping expand xr_type_names.def, so the array and
 * xr_type_name_from_tid cannot disagree about what an id is called. */
XR_DATADEF const char *typeid_names[XR_TID_COUNT] = {
#define XR_TYPE_NAME(suffix, id, display) [XR_TID_##suffix] = display,
#include "../../shared/xr_type_names.def"
#undef XR_TYPE_NAME
};

const char *xr_typeid_name(XrTypeId tid) {
    if (tid < 0 || tid >= XR_TID_COUNT)
        return TYPE_NAME_UNKNOWN;
    return xr_type_name_from_tid(tid);
}

uint8_t xr_type_to_tid(const XrType *type) {
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
