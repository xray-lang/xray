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

#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/object/xstring.h"

XrModule* xr_load_module_path(XrayIsolate *isolate);

#endif
