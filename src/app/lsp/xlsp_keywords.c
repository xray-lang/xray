/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_keywords.c - Keyword and builtin array definitions
 */

#include "xlsp_keywords.h"
#include <stddef.h>

// Xray reserved words. Keep this array mechanically tied to the lexer table so
// completion and rename cannot drift from the parser-visible token set.
const char *xr_keywords[] = {
#define XR_KW(spelling, length, token) spelling,
#include "xkeywords.def"
#undef XR_KW
#define XR_SCALAR_TYPE(source_id, spelling, length, lexer_token, scalar_rep, type_family, role,    \
                       canonical_display, public_type_id, range_class)                             \
    spelling,
#include "../../../shared/xr_scalar_type.def"
#undef XR_SCALAR_TYPE
    NULL};

// Builtin and prelude symbols offered by completion and protected from rename.
// Prelude names are tied to the same registry used by the analyzer.
const char *xr_builtins[] = {
#define XR_PRELUDE_TYPE(name, native_type, kind) name,
#include "../../../stdlib/prelude/prelude_types.def"
#undef XR_PRELUDE_TYPE
    "Coro",   "CoroPool",  "WeakMap",      "WeakSet",   "__dir__",       "__file__",    "process",
    "assert", "assert_eq", "assert_false", "assert_ne", "assert_throws", "assert_true", "bool",
    "chr",    "copy",      "dump",         "float",     "int",           "len",         "likely",
    "print",  "rune",      "string",       "typeName",  "typeOf",        "unlikely",    NULL};
