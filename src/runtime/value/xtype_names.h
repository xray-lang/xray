/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_names.h - Type name constants
 *
 * KEY CONCEPT:
 *   Avoids hardcoded type name strings.
 *   Primitives (int/float/string/bool/null) lowercase.
 *   Object types (Array/Map/BigInt/DateTime etc.) PascalCase.
 */

#ifndef XTYPE_NAMES_H
#define XTYPE_NAMES_H

#include "../../base/xdefs.h"
#include "../../shared/xr_type_names_core.h"

/* ========== Utility Functions ========== */

XR_FUNC int xr_type_from_name(const char *type_name);
XR_FUNC int xr_is_valid_type_name(const char *type_name);

// Type ID → name string (defined in xvalue.c)
XR_FUNC const char *xr_typeid_name(XrTypeId tid);

// XrType kind → XrTypeId (for reified generics, defined in xvalue.c)
struct XrType;
XR_FUNC uint8_t xr_type_to_tid(const struct XrType *type);

#endif  // XTYPE_NAMES_H
