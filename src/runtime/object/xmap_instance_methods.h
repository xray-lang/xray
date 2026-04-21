/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap_instance_methods.h - Map instance method declarations
 */

#ifndef XMAP_INSTANCE_METHODS_H
#define XMAP_INSTANCE_METHODS_H

#include "../value/xvalue.h"
#include "../../base/xdefs.h"

// Map instance methods
XR_FUNC XrValue xr_map_method_set(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_map_method_get(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_map_method_has(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_map_method_delete(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_map_method_clear(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_map_method_increment(XrayIsolate *isolate, XrValue *args, int nargs);

// Note: Methods are registered via XrClassBuilder in xr_map_create_class()

#endif // XMAP_INSTANCE_METHODS_H
