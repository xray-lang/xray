/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * url.c - pure-Xray URL module loader
 *
 * KEY CONCEPT:
 *   The user-facing url module is implemented in stdlib/url/url.xr. This file
 *   only creates the stdlib module shell so `import url` resolves through the
 *   normal script-extension path.
 */

#include "url.h"
#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_url(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_url: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "url");
    if (!module)
        return NULL;

    module->requires_script = true;
    return module;
}
