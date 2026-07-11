/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * parallel.c - `parallel` module loader.
 *
 * The public API is defined in stdlib/parallel/parallel.xr. This native loader
 * only anchors the module in the stdlib registry; hosted VM/AOT production
 * paths lower eligible calls to XI_PAR_* instead of calling the reference body.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_parallel(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_parallel: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "parallel");
    if (!module)
        return NULL;

    module->requires_script = true;
    module->loaded = true;
    return module;
}
