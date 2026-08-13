/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * simd.c - `simd` portable vector module loader.
 *
 * The scalar semantic truth lives in simd.xr. This loader only anchors the
 * module in the stdlib registry so the VM can execute the embedded source and
 * AOT can recognize the same public surface for Xi vector lowering.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_native_module_create_simd(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_native_module_create_simd: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "simd");
    if (!module)
        return NULL;

    module->requires_script = true;
    return module;
}
