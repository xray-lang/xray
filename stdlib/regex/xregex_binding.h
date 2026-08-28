/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex_binding.h - VM binding for the regex module
 *
 * KEY CONCEPT:
 *   The engine is stdlib/regex/regex.xr. This header declares only what the
 *   runtime still needs from C: module registration, class registration, and
 *   the bytecode helper that builds a Regex for a /pat/flags literal.
 *
 *   The public surface (compile, test, count, find, fullFind, findAll,
 *   findText, findGroup, replace, replaceAll, split, escape, isValid) is
 *   exported from regex.xr and documented there.
 */

#ifndef XREGEX_BINDING_H
#define XREGEX_BINDING_H

#include "../../src/base/xdefs.h"
#include "../../src/runtime/value/xvalue.h"

struct XrModule;

/* Create the native `regex` module: the four private leaves regex.xr forwards
 * to (__regexNew, __regexParseFlags, __unicodePropId, __unicodeHasProp). */
XR_FUNC struct XrModule *xr_native_module_create_regex(XrVMRuntime *isolate);

/*
 * Register the Regex and RegexMatch classes. Regex stays a native class
 * because the literal syntax /pat/flags lowers to XI_REGEX_COMPILE with its
 * result type pinned to type_regex (src/ir/xi_lower_expr.c:11349); it carries
 * three ordinary GC-visible fields (pattern, flags, prog) and no native body.
 * Called from xr_prelude_register_all_native_types during isolate init.
 */
XR_FUNC void xr_regex_register_class(XrVMRuntime *isolate);

/*
 * Build a Regex for a regex literal (OP_REGEX_COMPILE bytecode helper).
 * It records the pattern and the parsed flag mask; compilation happens in
 * regex.xr on first use.
 */
XR_FUNC XrValue xr_regex_compile_literal(XrVMRuntime *isolate, XrValue pattern_val,
                                         XrValue flags_val);

#endif  // XREGEX_BINDING_H
