/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_lookup.h - Fast class lookup independent of reflection
 *
 * KEY CONCEPT:
 *   Direct lookup without triggering reflection registration.
 *   O(1) for core classes, O(n) for user classes.
 */

#ifndef XCLASS_LOOKUP_H
#define XCLASS_LOOKUP_H

#include "../../base/xforward_decl.h"
#include "../../base/xdefs.h"

XR_FUNC XrClass* xr_class_lookup_by_name(XrayIsolate *X, const char *class_name);

#endif // XCLASS_LOOKUP_H
