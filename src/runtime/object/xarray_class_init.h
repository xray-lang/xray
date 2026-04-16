/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray_class_init.h - Array class initialization
 *
 * KEY CONCEPT:
 *   Create Array class with constructor using XrClassBuilder.
 */

#ifndef XARRAY_CLASS_INIT_H
#define XARRAY_CLASS_INIT_H

#include "../../base/xforward_decl.h"
#include "../base/xdefs.h"

// Create Array class with all methods (using XrClassBuilder)
XR_FUNC XrClass* xr_array_create_class(XrayIsolate *X, XrClass *objectClass);

#endif // XARRAY_CLASS_INIT_H
