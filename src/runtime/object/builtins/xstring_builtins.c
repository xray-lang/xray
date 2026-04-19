/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_builtins.c - String builtin functions
 *
 * KEY CONCEPT:
 *   Global String constructor.
 */

#include "xstring_builtins.h"
#include "xchecks.h"
#include "xisolate_api.h"
#include "xstring.h"
#include "xarray.h"
#include "xvalue.h"
#include <stdio.h>
#include <string.h>

// Helper: convert any value to string
static XrString* value_to_string(XrayIsolate *isolate, XrValue value) {
    if (XR_IS_STRING(value)) {
        return XR_TO_STRING(value);
    }
    
    if (XR_IS_INT(value)) {
        return xr_string_from_int(isolate, XR_TO_INT(value));
    }
    
    if (XR_IS_FLOAT(value)) {
        return xr_string_from_float(isolate, XR_TO_FLOAT(value));
    }
    
    if (XR_IS_BOOL(value)) {
        const char *s = XR_TO_BOOL(value) ? "true" : "false";
        return xr_string_intern(isolate, s, strlen(s), 0);
    }
    
    if (XR_IS_NULL(value)) {
        return xr_string_intern(isolate, "null", 4, 0);
    }
    
    // Other types: simplified representation
    if (XR_IS_ARRAY(value)) {
        return xr_string_intern(isolate, "[Array]", 7, 0);
    }
    
    if (XR_IS_SET(value)) {
        return xr_string_intern(isolate, "#{Set}", 6, 0);
    }
    
    if (XR_IS_MAP(value)) {
        return xr_string_intern(isolate, "{Map}", 5, 0);
    }
    
    return xr_string_intern(isolate, "<object>", 8, 0);
}

// String() or String(value)
XrValue xr_builtin_string_construct(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "string_construct: NULL isolate");
    if (nargs == 0) {
        // String() - empty string
        XrString *str = xr_string_intern(isolate, "", 0, 0);
        return xr_string_value(str);
    }
    else if (nargs == 1) {
        // String(value) - toString
        XrString *str = value_to_string(isolate, args[0]);
        return xr_string_value(str);
    }
    else {
        xr_runtime_error(isolate, "String() expects 0 or 1 argument\n");
        return xr_null();
    }
}

