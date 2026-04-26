/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstringbuilder_builtins.c - StringBuilder builtin methods
 *
 * KEY CONCEPT:
 *   Efficient string concatenation with mutable buffer.
 *   Supports chained calls: sb.append("a").append("b")
 */

#include "xchecks.h"
#include "xgc.h"
#include "xstringbuilder_builtins.h"
#include "xstringbuilder.h"
#include "xstring.h"
#include "xvalue.h"
#include "xclass.h"
#include "xclass_builder.h"
#include "xisolate_api.h"
#include "xclass_system.h"
#include <stdio.h>
#include <string.h>

/* ========== Helpers ========== */

// Convert XrValue to string and append to StringBuilder
static void append_value(XrStringBuilder *sb, XrayIsolate *iso, XrValue value) {
    (void) iso;
    if (XR_IS_STRING(value)) {
        xr_stringbuilder_append_str(sb, XR_TO_STRING(value));
    } else if (XR_IS_INT(value)) {
        xr_stringbuilder_append_int(sb, XR_TO_INT(value));
    } else if (XR_IS_FLOAT(value)) {
        xr_stringbuilder_append_float(sb, XR_TO_FLOAT(value));
    } else if (XR_IS_BOOL(value)) {
        const char *s = XR_TO_BOOL(value) ? "true" : "false";
        xr_stringbuilder_append_cstr(sb, s, strlen(s));
    } else if (XR_IS_NULL(value)) {
        xr_stringbuilder_append_cstr(sb, "null", 4);
    } else {
        xr_stringbuilder_append_cstr(sb, "<object>", 8);
    }
}

/* ========== Constructor ========== */

// StringBuilder() or StringBuilder(initValue)
XrValue xr_builtin_stringbuilder_new(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "stringbuilder_new: NULL isolate");
    XrStringBuilder *sb = xr_stringbuilder_new(xr_current_coro(isolate));
    if (!sb)
        return xr_null();

    if (nargs > 0) {
        append_value(sb, isolate, args[0]);
    }

    return xr_stringbuilder_value(sb);
}

/* ========== Instance Methods ========== */

// sb.append(value) - supports chaining
XrValue xr_builtin_stringbuilder_append(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_null();

    XrValue receiver = args[0];
    XrStringBuilder *sb = xr_to_stringbuilder(receiver);
    if (!sb)
        return xr_null();

    for (int i = 1; i < nargs; i++) {
        append_value(sb, isolate, args[i]);
    }

    return receiver;
}

// sb.toString()
XrValue xr_builtin_stringbuilder_toString(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs < 1)
        return xr_null();

    XrStringBuilder *sb = xr_to_stringbuilder(args[0]);
    if (!sb)
        return xr_null();

    XrString *str = xr_stringbuilder_to_string(sb);
    if (!str) {
        return xr_string_value(xr_string_intern(isolate, "", 0, 0));
    }

    return xr_string_value(str);
}

// sb.clear()
XrValue xr_builtin_stringbuilder_clear(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1)
        return xr_null();

    XrStringBuilder *sb = xr_to_stringbuilder(args[0]);
    if (!sb)
        return xr_null();

    xr_stringbuilder_clear(sb);
    return args[0];
}

// sb.length
XrValue xr_builtin_stringbuilder_length(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1)
        return xr_int(0);

    XrStringBuilder *sb = xr_to_stringbuilder(args[0]);
    if (!sb)
        return xr_int(0);

    return xr_int((int64_t) xr_stringbuilder_length(sb));
}

/* ========== Class Initialization ========== */

// Initialize StringBuilder class
// Create StringBuilder class with all methods using XrClassBuilder
XrClass *xr_stringbuilder_create_class(XrayIsolate *X, XrClass *objectClass) {
    XR_DCHECK(X != NULL, "stringbuilder_create_class: NULL isolate");
    XrClassBuilder *builder = xr_class_builder_new(X, "StringBuilder", objectClass);
    if (!builder) {
        fprintf(stderr, "[StringBuilder] ERROR: Failed to create class builder\n");
        return NULL;
    }

    // Static constructor
    xr_class_builder_add_static_method(builder, XR_KEYWORD_CONSTRUCTOR,
                                       (XrCFunctionPtr) xr_builtin_stringbuilder_new, 0, 0);

    // Instance methods
    xr_class_builder_add_method(builder, "append", (XrCFunctionPtr) xr_builtin_stringbuilder_append,
                                1, 0);
    xr_class_builder_add_method(builder, "toString",
                                (XrCFunctionPtr) xr_builtin_stringbuilder_toString, 0, 0);
    xr_class_builder_add_method(builder, "clear", (XrCFunctionPtr) xr_builtin_stringbuilder_clear,
                                0, 0);
    xr_class_builder_add_method(builder, "length", (XrCFunctionPtr) xr_builtin_stringbuilder_length,
                                0, 0);

    return xr_class_builder_finalize(builder);
}
