/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * _probe.c - `_probe` module loader (pure-Xray stdlib capability probe)
 *
 * KEY CONCEPT:
 *   `_probe` is the compiler capability matrix for pure-Xray stdlib modules
 *   (task 148 phase 0 item 5). It exists so tests/diff can permanently pin
 *   every export shape a migrated stdlib module may use: module-level
 *   functions (named import + namespace call), varargs, plain classes,
 *   generic classes, and "class + function using it" combinations.
 *
 *   Everything lives in stdlib/_probe/_probe.xr; this loader only creates
 *   the empty native module so the resolver recognises `import _probe` as
 *   stdlib, exactly like the `sync` precedent.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_probe(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_probe: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "_probe");
    if (!module)
        return NULL;

    module->requires_script = true;
    module->loaded = true;
    return module;
}
