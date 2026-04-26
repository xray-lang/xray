/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_methods.h - String instance method dispatch table.
 *
 * KEY POINTS:
 *   - Owns the per-type XrMethodSlot table for strings (XR_TID_STRING).
 *     OP_INVOKE_BUILTIN, OP_INVOKE invoke_string, the JIT string
 *     hint, and the cold-path go-statement helper all reach methods
 *     through xr_method_table_lookup(XR_TID_STRING, ...).
 *   - The bound-method bridge (xr_string_get_handler in
 *     xvm_builtins.c) pulls from the same table.
 *   - SYMBOL_MATCH is wired through stdlib/regex/. The reverse
 *     include used to live in src/vm/xvm_builtins.c (vm -> stdlib);
 *     it now lives in xstring_methods.c (runtime/object -> stdlib).
 *     Same direction; the structural fix is to move XrRegex itself
 *     into src/runtime/object/ — out of scope for the method table
 *     migration.
 *   - Method bodies are `static` inside xstring_methods.c.
 */

#ifndef XSTRING_METHODS_H
#define XSTRING_METHODS_H

#include "../value/xmethod_table.h"
#include "../symbol/xsymbol_table.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const XrMethodSlot xr_string_method_table[SYMBOL_BUILTIN_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* XSTRING_METHODS_H */
