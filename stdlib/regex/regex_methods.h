/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * regex_methods.h - Regex instance method dispatch table.
 *
 * KEY POINTS:
 *   - Owning the Regex method table inside stdlib/regex/ keeps regex
 *     method implementations colocated with the engine so src/vm
 *     never needs to reverse-include stdlib/regex/*. The VM dispatcher
 *     reaches each method via xr_method_table_lookup(XR_TID_REGEX, ...),
 *     so it never sees XrRegex internals directly.
 *   - The method bodies are static inside regex_methods.c. The slot
 *     table itself is the only public surface published here.
 *   - Mirrors stdlib/datetime/datetime_methods.{c,h}; both sit under
 *     xr_builtin_method_tables[] in src/runtime/value/xmethod_table.c.
 */

#ifndef XRAY_REGEX_METHODS_H
#define XRAY_REGEX_METHODS_H

#include "../../src/runtime/value/xmethod_table.h"
#include "../../src/runtime/symbol/xsymbol_table.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const XrMethodSlot xr_regex_method_table[SYMBOL_BUILTIN_COUNT];

#ifdef __cplusplus
}
#endif

#endif // XRAY_REGEX_METHODS_H
