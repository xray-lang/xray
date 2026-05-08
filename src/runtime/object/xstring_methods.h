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
 *   - Dispatch is via native_type_classes[XR_TSTRING], registered
 *     during isolate init by xr_string_register_native_type().
 *   - SYMBOL_MATCH is wired through stdlib/regex/.
 *   - Method bodies are `static` inside xstring_methods.c.
 */

#ifndef XSTRING_METHODS_H
#define XSTRING_METHODS_H

#include "../../base/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct XrayIsolate;
XR_FUNC void xr_string_register_native_type(struct XrayIsolate *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XSTRING_METHODS_H */
