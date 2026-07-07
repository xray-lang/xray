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
XrValue xr_enum_get_name(XrVMRuntime *isolate, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_null();
    XrValue self = args[0];
    if (!XR_IS_PTR(self))
        return xr_null();

    if (!XR_IS_ENUM_CTOR(self))
        return xr_null();

    XrEnumCtor *enum_val = (XrEnumCtor *) XR_TO_PTR(self);

    const char *name = xr_enum_ctor_name(enum_val);
    size_t len = strlen(name);
    XrString *str = xr_string_intern(isolate, name, len, 0);
    return xr_string_value(str);
}

/* ========== Enum.ordinal ========== */
XrValue xr_enum_get_ordinal(XrVMRuntime *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1)
        return xr_null();
    XrValue self = args[0];
    if (!XR_IS_PTR(self))
        return xr_null();

    if (!XR_IS_ENUM_CTOR(self))
        return xr_null();

    XrEnumCtor *enum_val = (XrEnumCtor *) XR_TO_PTR(self);

    return xr_int(enum_val->member_index);
}

/* ========== Enum.toString ========== */
XrValue xr_enum_toString(XrVMRuntime *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "enum_toString: NULL isolate");
    if (nargs < 1)
        return xr_null();
    XrValue self = args[0];
    if (!XR_IS_PTR(self))
        return xr_null();

    if (!XR_IS_ENUM_CTOR(self))
        return xr_null();

    XrEnumCtor *enum_val = (XrEnumCtor *) XR_TO_PTR(self);

    // Format: EnumName.MemberName
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s.%s", enum_val->enum_name, xr_enum_ctor_name(enum_val));

    size_t len = strlen(buffer);
    XrString *str = xr_string_intern(isolate, buffer, len, 0);
    return xr_string_value(str);
}
