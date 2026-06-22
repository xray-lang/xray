/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_closure.c - VM-facing closure helpers
 */

#include "xvm_closure.h"
#include "../runtime/xisolate_api.h"

XrClosure *xr_vm_closure_from_arg(XrayIsolate *isolate, XrValue v, const char *api_name) {
    XrClosure *closure = xr_value_to_closure(v);
    if (!closure) {
        xr_runtime_error(isolate, "%s: callback must be a function\n",
                         api_name ? api_name : "callback");
    }
    return closure;
}
