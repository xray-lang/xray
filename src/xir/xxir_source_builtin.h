/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_builtin.h - Source-loader-owned executable builtin bindings
 *
 * KEY CONCEPT:
 *   Numerical bindings are published only by the actual Core loader. Query
 *   names and native identity observations cannot manufacture these facts.
 */
#ifndef XXIR_SOURCE_BUILTIN_H
#define XXIR_SOURCE_BUILTIN_H
#include "xxir_source_query.h"

typedef struct XrXirSourceBuiltinBinding {
    uint32_t builtin, function, declaration;
} XrXirSourceBuiltinBinding;

/* The immutable snapshot owns these scalars after every Source producer dies.
 * Absent bindings preserve output and never confer an executable role. */
XR_FUNC bool xr_xir_compile_source_builtin_binding(const XrXirSourceSnapshot *,
    uint32_t source_function, XrXirSourceBuiltinBinding *output);
#endif // XXIR_SOURCE_BUILTIN_H
