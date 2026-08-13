/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * csv.c - `csv` module loader (pure-Xray CSV parser/writer)
 *
 * KEY CONCEPT:
 *   `csv` is a pure-Xray stdlib module. Parsing, dynamic typing, header
 *   mapping, and stringification live in stdlib/csv/csv.xr. The only system
 *   boundary left in the public surface is parseFile/writeFile, which delegates
 *   to the io module from Xray code.
 *
 *   This loader only creates the empty native module so the resolver recognises
 *   `import csv` as stdlib; the script-extension path then compiles
 *   stdlib/csv/csv.xr and populates the module exports.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_native_module_create_csv(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_native_module_create_csv: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "csv");
    if (!module)
        return NULL;

    module->requires_script = true;
    return module;
}
