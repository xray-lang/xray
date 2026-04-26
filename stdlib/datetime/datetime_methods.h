/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * datetime_methods.h - DateTime instance method dispatch table.
 *
 * KEY POINTS:
 *   - Owning the DateTime method table inside stdlib/datetime/ is what
 *     finally lets src/vm/xvm.c stop reverse-#including
 *     stdlib/datetime/datetime.h. The VM dispatcher reaches each
 *     method through xr_method_table_lookup(XR_TID_DATETIME, ...),
 *     so it never sees DateTime's data structures directly.
 *   - The method bodies are `static` inside datetime_methods.c
 *     because nothing outside that translation unit needs to call
 *     them by name.
 */

#ifndef XRAY_DATETIME_METHODS_H
#define XRAY_DATETIME_METHODS_H

#include "../../src/runtime/value/xmethod_table.h"
#include "../../src/runtime/symbol/xsymbol_table.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const XrMethodSlot xr_datetime_method_table[SYMBOL_BUILTIN_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* XRAY_DATETIME_METHODS_H */
