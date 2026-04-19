/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_builtins.h - Json utility class (static methods only)
 *
 * KEY CONCEPT:
 *   Json objects have no instance methods to avoid name conflicts
 *   with user-defined fields. All operations go through Json.xxx()
 *   static methods (e.g. Json.keys(obj), Json.has(obj, "key")).
 */

#ifndef XJSON_BUILTINS_H
#define XJSON_BUILTINS_H

#include "xisolate_api.h"
#include "xvalue.h"
#include "xdefs.h"

XR_FUNC void xr_json_api_init(XrayIsolate *X);

#endif // XJSON_BUILTINS_H
