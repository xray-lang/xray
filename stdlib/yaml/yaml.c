/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * yaml.c - `yaml` module loader (pure-Xray YAML parser/writer)
 *
 * KEY CONCEPT:
 *   `yaml` is a pure-Xray stdlib module. Parsing, strict diagnostics,
 *   multi-document splitting, stringification, and file wrappers live in
 *   stdlib/yaml/yaml.xr.
 *
 *   This loader only creates the empty native module so the resolver recognises
 *   `import yaml` as stdlib; the script-extension path compiles
 *   stdlib/yaml/yaml.xr and populates the module exports.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_yaml(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_yaml: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "yaml");
    if (!module)
        return NULL;

    module->requires_script = true;
    module->loaded = true;
    return module;
}
