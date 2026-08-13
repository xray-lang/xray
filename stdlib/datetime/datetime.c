/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * datetime.c - `datetime` module loader (pure-Xray calendar algorithms)
 *
 * KEY CONCEPT:
 *   Public DateTime semantics live in stdlib/datetime/datetime.xr. The only
 *   platform shell is the `time` module (`time.now`, `time.localOffset`,
 *   `time.localOffsetAt`). This file only anchors the stdlib module name so
 *   `import datetime` resolves and the script-extension path populates the
 *   module exports.
 */

#include "datetime.h"
#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_native_module_create_datetime(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_native_module_create_datetime: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "datetime");
    if (!module)
        return NULL;

    module->requires_script = true;
    return module;
}
