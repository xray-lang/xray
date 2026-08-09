/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http.c - Loader for the pure-Xray HTTP module.
 *
 * All HTTP/1.x request and server semantics live in stdlib/http/http.xr; this
 * module carries no native algorithm. The former native package-manager HTTP
 * client was retired in favor of the pure-Xray request path.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"
#include "http.h"

XR_FUNC XrModule *xr_load_module_http(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_http: NULL isolate");
    XrModule *module = xr_module_create_native(isolate, "http");
    if (!module)
        return NULL;
    module->requires_script = true;
    return module;
}
