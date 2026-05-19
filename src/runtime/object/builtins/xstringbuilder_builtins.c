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
#include "xisolate_api.h"
#include "xclass_builder.h"
#include "xclass_system.h"
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
XrValue xr_builtin_stringbuilder_new(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    (void) self;
    XR_DCHECK(isolate != NULL, "stringbuilder_new: NULL isolate");
    XrStringBuilder *sb = xr_stringbuilder_new(xr_current_coro(isolate));
    if (!sb)
        return xr_null();

    if (argc > 0) {
        append_value(sb, isolate, args[0]);
    }

    return xr_stringbuilder_value(sb);
}

/* ========== Instance Methods ========== */

// sb.append(value) - supports chaining
XrValue xr_builtin_stringbuilder_append(XrayIsolate *isolate, XrValue self, XrValue *args,
                                        int argc) {
    XrStringBuilder *sb = xr_to_stringbuilder(self);
    if (!sb)
        return xr_null();

    for (int i = 0; i < argc; i++) {
        append_value(sb, isolate, args[i]);
    }

    return self;
}

// sb.toString()
XrValue xr_builtin_stringbuilder_toString(XrayIsolate *isolate, XrValue self, XrValue *args,
                                          int argc) {
    (void) args;
    (void) argc;
    XrStringBuilder *sb = xr_to_stringbuilder(self);
    if (!sb)
        return xr_null();

    XrString *str = xr_stringbuilder_to_string(sb);
    if (!str) {
        return xr_string_value(xr_string_intern(isolate, "", 0, 0));
    }

    return xr_string_value(str);
}

// sb.clear()
XrValue xr_builtin_stringbuilder_clear(XrayIsolate *isolate, XrValue self, XrValue *args,
                                       int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    XrStringBuilder *sb = xr_to_stringbuilder(self);
    if (!sb)
        return xr_null();

    xr_stringbuilder_clear(sb);
    return self;
}

// sb.length
XrValue xr_builtin_stringbuilder_length(XrayIsolate *isolate, XrValue self, XrValue *args,
                                        int argc) {
    (void) isolate;
    (void) args;
    (void) argc;
    XrStringBuilder *sb = xr_to_stringbuilder(self);
    if (!sb)
        return xr_int(0);

    return xr_int((int64_t) xr_stringbuilder_length(sb));
}

/* ========== Class Registration ========== */

void xr_stringbuilder_register_class(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "stringbuilder_register_class: NULL isolate");
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    XR_DCHECK(core != NULL, "stringbuilder_register_class: no core classes");

    XrClassBuilder *b = xr_class_builder_new(X, "StringBuilder", core->objectClass);
    if (!b)
        return;
    xr_class_builder_set_native_body(b, xr_stringbuilder_native_body_desc());
    xr_class_builder_add_static_method(b, "constructor", xr_builtin_stringbuilder_new, 0, 0);
    xr_class_builder_add_method(b, "append", xr_builtin_stringbuilder_append, 1, 0);
    xr_class_builder_add_method(b, "toString", xr_builtin_stringbuilder_toString, 0, 0);
    xr_class_builder_add_method(b, "clear", xr_builtin_stringbuilder_clear, 0, 0);
    xr_class_builder_add_method(b, "length", xr_builtin_stringbuilder_length, 0, 0);
    XrClass *cls = xr_class_builder_finalize(b);
    if (!cls)
        return;
    cls->flags |= XR_CLASS_BUILTIN | XR_CLASS_STRINGBUILDER;
    core->stringBuilderClass = cls;
}
