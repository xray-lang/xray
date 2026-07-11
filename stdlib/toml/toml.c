/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * toml.c - `toml` module loader (pure-Xray TOML parser/writer)
 *
 * KEY CONCEPT:
 *   `toml` is a pure-Xray stdlib module. Parsing, strict diagnostics,
 *   stringification, and file wrappers live in stdlib/toml/toml.xr.
 *
 *   This loader only creates the empty native module so the resolver recognises
 *   `import toml` as stdlib; the script-extension path compiles
 *   stdlib/toml/toml.xr and populates the module exports.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_toml(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_toml: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "toml");
    if (!module)
        return NULL;

    module->requires_script = true;
    return module;
}
