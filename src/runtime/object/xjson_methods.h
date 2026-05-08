/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_methods.h - Json builtin method dispatch table.
 *
 * KEY POINTS:
 *   - Json instance methods are intentionally tiny: entriesIterator
 *     (powers `for (k, v in obj)`) and toString. All other Json
 *     facilities are exposed as the static `Json.xxx(obj)` API
 *     elsewhere; they do not go through this table.
 *   - Bodies live as `static` inside xjson_methods.c. The bound-
 *     method system does not need them by name.
 */

#ifndef XJSON_METHODS_H
#define XJSON_METHODS_H

#include "../value/xmethod_table.h"
#include "../symbol/xsymbol_table.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const XrMethodSlot xr_json_method_table[SYMBOL_BUILTIN_COUNT];

struct XrayIsolate;
XR_FUNC void xr_json_register_native_type(struct XrayIsolate *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XJSON_METHODS_H */
