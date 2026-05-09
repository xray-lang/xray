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
XR_FUNC XrValue xr_builtin_stringbuilder_new(XrayIsolate *isolate, XrValue self, XrValue *args,
                                             int argc);

// Instance methods (self = receiver StringBuilder)
XR_FUNC XrValue xr_builtin_stringbuilder_append(XrayIsolate *isolate, XrValue self, XrValue *args,
                                                int argc);
XR_FUNC XrValue xr_builtin_stringbuilder_toString(XrayIsolate *isolate, XrValue self, XrValue *args,
                                                  int argc);
XR_FUNC XrValue xr_builtin_stringbuilder_clear(XrayIsolate *isolate, XrValue self, XrValue *args,
                                               int argc);
XR_FUNC XrValue xr_builtin_stringbuilder_length(XrayIsolate *isolate, XrValue self, XrValue *args,
                                                int argc);

// Register StringBuilder as native type (constructor + instance methods)
XR_FUNC void xr_stringbuilder_register_native_type(XrayIsolate *X);

#endif  // XSTRINGBUILDER_BUILTINS_H
