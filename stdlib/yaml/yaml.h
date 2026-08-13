/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * yaml.h - YAML standard library module loader
 *
 * Public YAML semantics live in stdlib/yaml/yaml.xr. This header exposes only
 * the native loader anchor used by the stdlib resolver.
 */

#ifndef XR_STDLIB_YAML_H
#define XR_STDLIB_YAML_H

#include "../../src/base/xdefs.h"
struct XrVMRuntime;
struct XrModule;

// Load yaml module
XR_FUNC struct XrModule *xr_native_module_create_yaml(XrVMRuntime *isolate);

#endif  // XR_STDLIB_YAML_H
