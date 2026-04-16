/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_internal.h - Internal shared declarations for xtype_*.c files
 *
 * KEY CONCEPT:
 *   Exposes internal functions shared between xtype.c, xtype_format.c,
 *   and xtype_generic.c. Not part of the public API.
 */

#ifndef XTYPE_INTERNAL_H
#define XTYPE_INTERNAL_H

#include "xtype.h"
#include "xtype_pool.h"

// Internal: get current thread-local type pool
XR_FUNC XrTypePool *xr_type_get_current_pool(void);

// Internal: check class inheritance (walks superclass chain)
XR_FUNC bool xr_type_is_subclass_of(XrType *type, XrType *target);

#endif // XTYPE_INTERNAL_H
