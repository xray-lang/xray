/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset_class_init.h - Set class initialization
 *
 * KEY CONCEPT:
 *   Create Set class with constructor using XrClassBuilder.
 */

#ifndef XSET_CLASS_INIT_H
#define XSET_CLASS_INIT_H

#include "../../base/xforward_decl.h"
#include "../../base/xdefs.h"

// Create Set class with all methods (using XrClassBuilder)
XR_FUNC XrClass* xr_set_create_class(XrayIsolate *X, XrClass *objectClass);

#endif // XSET_CLASS_INIT_H
