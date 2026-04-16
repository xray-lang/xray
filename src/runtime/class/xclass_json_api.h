/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_json_api.h - Json utility class (static methods only)
 *
 * KEY CONCEPT:
 *   Json objects have no instance methods to avoid name conflicts
 *   with user-defined fields. All operations go through Json.xxx()
 *   static methods (e.g. Json.keys(obj), Json.has(obj, "key")).
 */

#ifndef XCLASS_JSON_API_H
#define XCLASS_JSON_API_H

#include "../xisolate_api.h"
#include "../value/xvalue.h"
#include "../base/xdefs.h"

XR_FUNC void xr_json_api_init(XrayIsolate *X);

#endif // XCLASS_JSON_API_H
