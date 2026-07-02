/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * path.c - `path` module loader (pure-Xray cross-platform path manipulation)
 *
 * KEY CONCEPT:
 *   `path` is a pure-Xray stdlib module: every path algorithm lives in
 *   stdlib/path/path.xr as a pure string function, with os.getcwd() as the
 *   sole runtime shell. There is no C implementation to bind here.
 *
 *   This loader only creates the empty native module so the resolver
 *   recognises `import path` as stdlib; the hybrid script-extension mechanism
 *   (xmodule.c load_script_extension for the VM, xmodule_graph for AOT) then
 *   compiles stdlib/path/path.xr and populates the module's exports.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_path(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_path: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "path");
    if (!module)
        return NULL;

    module->requires_script = true;
    module->loaded = true;
    return module;
}
