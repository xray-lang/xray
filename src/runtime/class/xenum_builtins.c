/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xenum_builtins.c - Enum builtin methods
 */

#include "xenum_builtins.h"
#include "../../base/xchecks.h"
#include "../xisolate_api.h"
#include "../value/xvalue.h"
#include "../object/xstring.h"
#include "xenum.h"
#include <string.h>
#include <stdio.h>

/* ========== Enum.name ========== */
XrValue xr_enum_get_name(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_null();
    XrValue self = args[0];
    if (!XR_IS_PTR(self))
        return xr_null();

    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(self);
    if (XR_GC_GET_TYPE(gc) != XR_TENUM_VALUE)
        return xr_null();

    XrEnumValue *enum_val = (XrEnumValue *) gc;

    size_t len = strlen(enum_val->member_name);
    XrString *str = xr_string_intern(isolate, enum_val->member_name, len, 0);
    return xr_string_value(str);
}

/* ========== Enum.value ========== */
XrValue xr_enum_get_value(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1)
        return xr_null();
    XrValue self = args[0];
    if (!XR_IS_PTR(self))
        return xr_null();

    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(self);
    if (XR_GC_GET_TYPE(gc) != XR_TENUM_VALUE)
        return xr_null();

    XrEnumValue *enum_val = (XrEnumValue *) gc;

    return enum_val->raw_value;
}

/* ========== Enum.ordinal ========== */
XrValue xr_enum_get_ordinal(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1)
        return xr_null();
    XrValue self = args[0];
    if (!XR_IS_PTR(self))
        return xr_null();

    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(self);
    if (XR_GC_GET_TYPE(gc) != XR_TENUM_VALUE)
        return xr_null();

    XrEnumValue *enum_val = (XrEnumValue *) gc;

    return xr_int(enum_val->member_index);
}

/* ========== Enum.toString ========== */
XrValue xr_enum_toString(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "enum_toString: NULL isolate");
    if (nargs < 1)
        return xr_null();
    XrValue self = args[0];
    if (!XR_IS_PTR(self))
        return xr_null();

    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(self);
    if (XR_GC_GET_TYPE(gc) != XR_TENUM_VALUE)
        return xr_null();

    XrEnumValue *enum_val = (XrEnumValue *) gc;

    // Format: EnumName.MemberName
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s.%s", enum_val->enum_name, enum_val->member_name);

    size_t len = strlen(buffer);
    XrString *str = xr_string_intern(isolate, buffer, len, 0);
    return xr_string_value(str);
}

/* ========== EnumType.memberCount ========== */
XrValue xr_enum_type_get_member_count(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1)
        return xr_null();
    XrValue self = args[0];
    if (!XR_IS_PTR(self))
        return xr_null();

    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(self);
    if (XR_GC_GET_TYPE(gc) != XR_TENUM_TYPE)
        return xr_null();

    XrEnumType *enum_type = (XrEnumType *) gc;

    return xr_int(enum_type->member_count);
}

/* ========== EnumType.getMember ========== */
XrValue xr_enum_type_get_member(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2)
        return xr_null();
    XrValue self = args[0];
    XrValue index_val = args[1];
    if (!XR_IS_PTR(self))
        return xr_null();
    if (!XR_IS_INT(index_val))
        return xr_null();

    XrGCHeader *gc = (XrGCHeader *) XR_TO_PTR(self);
    if (XR_GC_GET_TYPE(gc) != XR_TENUM_TYPE)
        return xr_null();

    XrEnumType *enum_type = (XrEnumType *) gc;
    int index = XR_TO_INT(index_val);

    if (index < 0 || index >= (int) enum_type->member_count) {
        return xr_null();
    }

    XrEnumValue *enum_val = enum_type->members[index].instance;
    return XR_FROM_PTR(enum_val);
}
