/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * log.c - `log` module loader (pure-Xray structured logger)
 *
 * KEY CONCEPT:
 *   Public log semantics live in stdlib/log/log.xr. This file only anchors the
 *   stdlib module name so `import log` resolves and the script-extension path
 *   populates the module exports.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_log(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_log: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "log");
    if (!module)
        return NULL;

    module->requires_script = true;
    return module;
}
