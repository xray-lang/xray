/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmethod_traits.h - Method mutability traits for const-correctness checking
 *
 * KEY CONCEPT:
 *   Provides compile-time checking for const object method calls.
 *   Mutating methods modify object state, readonly methods do not.
 */

#ifndef XMETHOD_TRAITS_H
#define XMETHOD_TRAITS_H

#include <stdbool.h>
#include "../../base/xdefs.h"

typedef enum {
    XR_METHOD_READONLY = 0,  // Const objects can call
    XR_METHOD_MUTATING = 1,  // Const objects cannot call
} XrMethodTrait;

// Check if a method mutates object state
XR_FUNC bool xr_method_is_mutating(const char *type_name, const char *method_name);

// Get method trait for a given type and method
XR_FUNC XrMethodTrait xr_method_get_trait(const char *type_name, const char *method_name);

#endif  // XMETHOD_TRAITS_H
