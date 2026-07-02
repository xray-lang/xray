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
 *   - Json instance methods cover the runtime protocol declared by
 *     stdlib/types/json.xr: iterator helpers plus keys/values/entries,
 *     toString, has, and the Json value predicates.
 *   - Bodies live as `static` inside xjson_methods.c. The bound-
 *     method system does not need them by name.
 */

#ifndef XJSON_METHODS_H
#define XJSON_METHODS_H

#include "../../base/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct XrVMRuntime;
XR_FUNC void xr_json_register_instance_methods(struct XrVMRuntime *isolate);

#ifdef __cplusplus
}
#endif

#endif /* XJSON_METHODS_H */
