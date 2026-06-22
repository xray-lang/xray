/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstringbuilder_builtins.h - StringBuilder builtin methods
 */

#ifndef XSTRINGBUILDER_BUILTINS_H
#define XSTRINGBUILDER_BUILTINS_H

#include "xvalue.h"
#include "xdefs.h"

// Constructor (self = class value, ignored)
XR_FUNC XrValue xr_builtin_stringbuilder_new(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                             int argc);

// Instance methods (self = receiver StringBuilder)
XR_FUNC XrValue xr_builtin_stringbuilder_append(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                                int argc);
XR_FUNC XrValue xr_builtin_stringbuilder_toString(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                                  int argc);
XR_FUNC XrValue xr_builtin_stringbuilder_clear(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                               int argc);
XR_FUNC XrValue xr_builtin_stringbuilder_length(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                                int argc);

// Build StringBuilder class with native body and register in core classes
XR_FUNC void xr_stringbuilder_register_class(XrVMRuntime *X);

#endif  // XSTRINGBUILDER_BUILTINS_H
