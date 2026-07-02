/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * sync.c - `sync` module loader (coroutine-aware synchronisation primitives)
 *
 * KEY CONCEPT:
 *   `sync` is a pure-Xray stdlib module: Mutex/RwLock/Once/Barrier/Condvar are
 *   defined entirely in stdlib/sync/sync.xr as generic classes composing the
 *   builtin coroutine-aware Semaphore/CountdownLatch/Atomic primitives. There
 *   is no C implementation to bind here.
 *
 *   This loader only needs to create the empty native module so the resolver
 *   recognises `import sync` as stdlib; the hybrid script-extension mechanism
 *   (xmodule.c load_script_extension for the VM, xmodule_graph for AOT) then
 *   compiles stdlib/sync/sync.xr and populates the module's exports.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_sync(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_sync: NULL isolate");

    /* Pure-Xray module: no C exports. The script extension
     * (stdlib/sync/sync.xr) provides all classes, so a missing script
     * layer must fail the load instead of yielding an empty module. */
    XrModule *module = xr_module_create_native(isolate, "sync");
    if (!module)
        return NULL;

    module->requires_script = true;
    module->loaded = true;
    return module;
}
