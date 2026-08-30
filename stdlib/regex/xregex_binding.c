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
 *   The regex engine itself lives in stdlib/regex/regex.xr. Nothing here
 *   parses, compiles or matches. This file only allocates the Regex handle
 *   and forwards three questions that belong to other owners: the one
 *   flag-string mask used by regex.compile and the VM literal path, plus two
 *   unicode property lookups owned by src/base/xunicode.c.
 *
 *   Regex stays a native class solely because the literal syntax /pat/flags
 *   lowers to XI_REGEX_COMPILE with its result type pinned to type_regex
 *   (src/ir/xi_lower_expr.c:11349). The handle carries three ordinary,
 *   GC-visible fields: pattern, flags and the compiled program image, which
 *   regex.xr fills in on first use.
 */

#include "xregex_binding.h"
#include "../common.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/value/xvalue_format.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xclass_builder.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/runtime/mem/xcoro_heap.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/shared/xr_semantic_owner_ids_gen.h"
#include "../../src/shared/xr_regex_core.h"
#include "../../src/base/xunicode.h"

/* Field slots on the Regex handle, in declaration order
 * (stdlib/defs/core.def, module regex). */
enum {
    XR_REGEX_FIELD_PATTERN = 0,
    XR_REGEX_FIELD_FLAGS = 1,
    XR_REGEX_FIELD_PROG = 2,
};

/* RegexMatch field slots, in declaration order. */
enum {
    XR_REGEX_MATCH_FIELD_START = 0,
    XR_REGEX_MATCH_FIELD_END = 1,
    XR_REGEX_MATCH_FIELD_TEXT = 2,
    XR_REGEX_MATCH_FIELD_GROUPS = 3,
};

static const char *value_to_cstring(XrValue v, int *len) {
    if (!XR_IS_STRING(v))
        return NULL;
    XrString *s = XR_TO_STRING(v);
    if (!s)
        return NULL;
    if (len)
        *len = (int) s->length;
    return s->data;
}

/* Allocate a Regex handle carrying its pattern and flags. The empty program
 * image is filled by regex.xr on first use. */
static XrValue make_regex(XrVMRuntime *isolate, XrValue pattern, int64_t flags) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core && core->regexClass, "make_regex: regexClass not registered");

    /* xr_instance_new takes XrVMRuntime*, not XrCoroutine*: passing a coro here
     * was type confusion that made a module-level regex.compile dereference
     * garbage during multi-module preload, before any coroutine runs. */
    XrInstance *inst = xr_instance_new(isolate, core->regexClass);
    if (!inst)
        return xr_null();
    xr_rc_retain_value(pattern);
    xr_instance_set_field_fast(inst, XR_REGEX_FIELD_PATTERN, pattern);
    xr_instance_set_field_fast(inst, XR_REGEX_FIELD_FLAGS, xr_int((int) flags));
    /* The program image starts as an empty array rather than null: regex.xr
     * fills it by pushing, because a class-typed parameter there is a read
     * capability and cannot have its fields assigned. */
    XrArray *prog = xr_array_new(xr_current_coro(isolate));
    xr_instance_set_field_fast(inst, XR_REGEX_FIELD_PROG, xr_value_from_array(prog));
    return XR_FROM_PTR(inst);
}

/* __regexMatchNew(start, end, text, groups) - allocation only; every value is
 * computed by regex.xr. */
static XrValue regex_match_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    if (argc < 4)
        return xr_null();
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core && core->regexMatchClass, "regex_match_new: regexMatchClass not registered");
    XrInstance *inst = xr_instance_new(isolate, core->regexMatchClass);
    if (!inst)
        return xr_null();
    xr_rc_retain_value(args[2]);
    xr_rc_retain_value(args[3]);
    xr_instance_set_field_fast(inst, XR_REGEX_MATCH_FIELD_START, args[0]);
    xr_instance_set_field_fast(inst, XR_REGEX_MATCH_FIELD_END, args[1]);
    xr_instance_set_field_fast(inst, XR_REGEX_MATCH_FIELD_TEXT, args[2]);
    xr_instance_set_field_fast(inst, XR_REGEX_MATCH_FIELD_GROUPS, args[3]);
    return XR_FROM_PTR(inst);
}

/* __regexNew(pattern, flags) */
static XrValue regex_compile(XrVMRuntime *isolate, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();
    int64_t flags = 0;
    if (argc >= 2 && XR_IS_INT(args[1]))
        flags = (int64_t) XR_TO_INT(args[1]);
    return make_regex(isolate, args[0], flags);
}

/* __regexParseFlags(flags) - forwards regex.compile to the flag-mask authority
 * that the VM literal path also uses. Unknown flag characters are ignored,
 * which is deliberate and long-standing. */
static XrValue regex_parse_flags(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1)
        return xr_int(0);
    int len = 0;
    const char *s = value_to_cstring(args[0], &len);
    if (!s)
        return xr_int(0);
    return xr_int((int) xr_regex_core_parse_flags(s, (size_t) len));
}

/* __unicodePropId(name) - forwards to the unicode property table, which is the
 * one owner of what \p{...} names mean. */
static XrValue regex_unicode_prop_id(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1)
        return xr_int(-1);
    int len = 0;
    const char *s = value_to_cstring(args[0], &len);
    if (!s)
        return xr_int(-1);
    XrUnicodeProperty prop = xr_unicode_property_lookup(s, len);
    if (prop == XR_UP_INVALID)
        return xr_int(-1);
    return xr_int((int) prop);
}

/* __unicodeHasProp(cp, propId) */
static XrValue regex_unicode_has_prop(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_bool(false);
    int64_t cp = (int64_t) XR_TO_INT(args[0]);
    int64_t prop = (int64_t) XR_TO_INT(args[1]);
    if (cp < 0 || prop <= 0)
        return xr_bool(false);
    return xr_bool(xr_unicode_is_property((uint32_t) cp, (XrUnicodeProperty) prop));
}

/* Public API: build a Regex for a regex literal (OP_REGEX_COMPILE bytecode
 * helper). Like __regexNew it only records the pattern and the flags; the
 * literal path cannot compile, because the compiler is Xray now. */
XrValue xr_regex_compile_literal(XrVMRuntime *isolate, XrValue pattern_val, XrValue flags_val) {
    XrString *pattern_str = xr_value_to_string(isolate, pattern_val);
    XrString *flags_str = xr_value_to_string(isolate, flags_val);
    if (!pattern_str || !flags_str)
        return xr_null();

    if (!xr_semantic_owner_has_consumer(XR_SEM_OWNER_ID_STDLIB_REGEX_COMPILE_MATCH_HI,
                                        XR_SEM_OWNER_ID_STDLIB_REGEX_COMPILE_MATCH_LO,
                                        XR_SEM_CONSUMER_RUNTIME))
        return xr_null();
    int64_t flags =
        (int64_t) xr_regex_core_parse_flags(flags_str->data, (size_t) flags_str->length);

    return make_regex(isolate, xr_string_value(pattern_str), flags);
}

/* ========================================================================
 * Class and module registration
 * ======================================================================== */

#define XR_STDLIB_VM_BIND_CLASS_REGEX 1
#define XR_STDLIB_VM_BIND_CLASS_REGEX_MATCH 1
#include "../../src/stdlib/xstdlib_class_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_CLASS_REGEX_MATCH
#undef XR_STDLIB_VM_BIND_CLASS_REGEX

void xr_regex_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_regex_class_generated(isolate);
    xr_stdlib_vm_register_regex_match_class_generated(isolate);
}

#define XR_STDLIB_VM_BIND_MODULE_REGEX 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_REGEX
