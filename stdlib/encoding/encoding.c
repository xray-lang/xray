/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * encoding.c - pure-Xray encoding module loader
 *
 * KEY CONCEPT:
 *   The encoding module's user-facing functions (hex* / utf8* / utf16*) are
 *   pure Xray, defined in stdlib/encoding/encoding.xr. This file only registers
 *   the pure-Xray module so `import encoding` resolves as stdlib.
 */

#include "encoding.h"
#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_encoding(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_encoding: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "encoding");
    if (!module)
        return NULL;

    module->requires_script = true;
    return module;
}
