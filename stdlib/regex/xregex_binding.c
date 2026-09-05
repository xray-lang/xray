/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex_binding.c - VM binding for the regex module
 *
 * KEY CONCEPT:
 *   Regex objects, flags, compilation, caching and matching live entirely in
 *   stdlib/regex/regex.xr. This file contains only the VM ABI adapters for the
 *   runtime's canonical Unicode property tables.
 */

#include "../common.h"

#include <stdbool.h>

#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/base/xunicode.h"

/* __unicodePropId(name) - forwards to the unicode property table, which is the
 * one owner of what \p{...} names mean. */
static XrValue regex_unicode_prop_id(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc != 1 || !XR_IS_STRING(args[0]))
        return xr_int(-1);
    XrString *name = XR_TO_STRING(args[0]);
    if (!name)
        return xr_int(-1);
    XrUnicodeProperty prop = xr_unicode_property_lookup(name->data, (int) name->length);
    if (prop == XR_UP_INVALID)
        return xr_int(-1);
    return xr_int((int) prop);
}

/* __unicodeHasProp(cp, propId) */
static XrValue regex_unicode_has_prop(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc != 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_bool(false);
    int64_t cp = (int64_t) XR_TO_INT(args[0]);
    int64_t prop = (int64_t) XR_TO_INT(args[1]);
    if (cp < 0 || prop <= 0)
        return xr_bool(false);
    return xr_bool(xr_unicode_is_property((uint32_t) cp, (XrUnicodeProperty) prop));
}

#define XR_STDLIB_VM_BIND_MODULE_REGEX 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_REGEX
