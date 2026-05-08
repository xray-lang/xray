/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_native_types.h - Load builtin type definitions from embedded .xr sources
 *
 * KEY CONCEPT:
 *   Native type declarations are written in Xray syntax (@native class)
 *   and embedded at compile time. This module parses the embedded strings
 *   at startup to populate the builtin type tables used by the analyzer.
 */

#ifndef XANALYZER_NATIVE_TYPES_H
#define XANALYZER_NATIVE_TYPES_H

#include "xanalyzer_builtins.h"
#include "../../base/xdefs.h"

/* Initialize builtin type member tables from embedded .xr declarations.
 * Must be called once before any xa_builtin_* query.  Populates the
 * internal builtin_types[] array. */
XR_FUNC void xa_native_types_init(void);

/* Check whether native types have been initialized. */
XR_FUNC bool xa_native_types_ready(void);

/* Get the populated builtin type table (indexed by XrTypeId). */
XR_FUNC const XaBuiltinType *xa_native_get_builtin_types(void);

/* Debug-only: verify that C-registered native methods match .xr declarations.
 * Logs warnings for any mismatches.  Returns number of mismatches (0 = OK). */
XR_FUNC int xa_native_verify_protocol(struct XrayIsolate *X);

#endif  // XANALYZER_NATIVE_TYPES_H
