/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * path.h - Path manipulation module
 *
 * KEY CONCEPT:
 *   Cross-platform path string operations (join, dirname, basename, etc).
 *
 * EXPORTS:
 *   Functions:
 *     - join(...)           Join path segments
 *     - dirname(path)       Get directory part
 *     - basename(path)      Get filename part
 *     - extname(path)       Get extension (with dot)
 *     - normalize(path)     Normalize path (resolve . and ..)
 *     - isAbsolute(path)    Check if path is absolute
 *     - resolve(...)        Resolve to absolute path
 *     - relative(from, to)  Compute relative path
 *     - parse(path)         Parse path into components (returns Map)
 *     - format(obj)         Build path from components
 *   Constants:
 *     - sep                 Path separator
 *     - delimiter           PATH environment variable delimiter
 */

#ifndef XR_STDLIB_PATH_H
#define XR_STDLIB_PATH_H

#include "../../src/base/xdefs.h"
#include "../../src/runtime/value/xvalue.h"

struct XrayIsolate;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_path(struct XrayIsolate *isolate);

/* Runtime-archive AOT shims kept for the runtime-backed stdlib build. The
 * native AOT backend now emits freestanding xrt_path_* inline calls for the
 * supported pure path subset, so those programs do not link xray_core. */
XR_FUNC XrValue xr_aot_path_isAbsolute(const char *path, int64_t len);
XR_FUNC const char *xr_aot_path_dirname(const char *path, int64_t len, int64_t *out_len);
XR_FUNC const char *xr_aot_path_basename(const char *path, int64_t len, int64_t *out_len);
XR_FUNC const char *xr_aot_path_extname(const char *path, int64_t len, int64_t *out_len);

#endif  // XR_STDLIB_PATH_H
