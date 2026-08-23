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
#define XR_EXACT_SCALAR(id, stable_id, source_name, native_type, family, range_class, flags)       \
    source_name,
#include "../../../shared/xr_exact_scalar_registry.def"
#undef XR_EXACT_SCALAR
    NULL};

// Builtin and prelude symbols offered by completion and protected from rename.
// Prelude names are tied to the same registry used by the analyzer.
const char *xr_builtins[] = {
#define XR_BUILTIN_PRELUDE_TYPE(name, arity, native_type, prelude_kind) name,
#define XR_BUILTIN_TYPE(name, arity) name,
#define XR_BUILTIN_ENUM(name, arity, vm_slot, variants) name,
#define XR_BUILTIN_IFACE(name, arity) name,
#include "../../../stdlib/prelude/builtin_symbols.def"
#define XR_CORE_INTRINSIC(id, stable_id, source_name, category, call_form, parameter_shape,       \
                          min_arity, max_arity, result_shape, effect_kind, flow_rule,               \
                          expected_failure_channel, semantic_op, target_applicability,              \
                          diagnostic_name)                                                          \
    source_name,
#include "../../../shared/xr_core_intrinsic.def"
#undef XR_CORE_INTRINSIC
    "Coro", "CoroPool", "__dir__", "__file__", "process", "bool", "chr", "copy", "dump",
    "len", "rune",
    "string",       "typeName",  "typeOf",        NULL};
