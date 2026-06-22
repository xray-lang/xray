/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_closure.h - VM-facing closure helper API
 */

#ifndef XVM_CLOSURE_H
#define XVM_CLOSURE_H

#include "../runtime/closure/xclosure.h"

// Extract a closure pointer from a callback argument value. Returns the
// closure on success, or NULL after raising a VM runtime error tagged with
// `api_name` (e.g. "Array.reduce", "Map.forEach") when the value is not a
// function. VM native APIs that accept callbacks should use this helper
// instead of blindly converting non-pointer payloads to closure pointers.
XR_FUNC XrClosure *xr_vm_closure_from_arg(XrayIsolate *isolate, XrValue v, const char *api_name);

#endif  // XVM_CLOSURE_H
