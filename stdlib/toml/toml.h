/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * toml.h - TOML standard library module loader
 *
 * KEY CONCEPT:
 *   TOML's public API is implemented in stdlib/toml/toml.xr. This header only
 *   exposes the native loader that anchors `import toml` in the stdlib registry.
 */

#ifndef XR_STDLIB_TOML_H
#define XR_STDLIB_TOML_H

#include "../../src/base/xdefs.h"

struct XrModule;
struct XrVMRuntime;

XR_FUNC struct XrModule *xr_load_module_toml(struct XrVMRuntime *isolate);

#endif  // XR_STDLIB_TOML_H
