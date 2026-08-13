/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_native_module_create_codegen(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_native_module_create_codegen: NULL isolate");
    XrModule *module = xr_module_create_native(isolate, "codegen");
    if (!module)
        return NULL;
    module->requires_script = true;
    return module;
}
