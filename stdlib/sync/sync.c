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
 *   This loader exports the runtime-backed primitive class values
 *   (Semaphore/CountdownLatch/EventCount/WorkQueue/ResultGroup). The hybrid
 *   script-extension mechanism (xmodule.c load_script_extension for the VM,
 *   xmodule_graph for AOT) then compiles stdlib/sync/sync.xr and adds the
 *   pure-Xray classes that compose those primitives.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/xisolate_api.h"

static void sync_export_native_class(XrVMRuntime *isolate, XrModule *module, const char *name,
                                     uint8_t type_id) {
    XrClass *cls = xr_isolate_get_native_type_class(isolate, type_id);
    if (cls)
        xr_module_add_export(isolate, module, name, xr_value_from_class(cls));
}

XR_FUNC XrModule *xr_native_module_create_sync(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_native_module_create_sync: NULL isolate");

    /* Hybrid module: C exports runtime primitives, script layer exports the
     * ergonomic pure-Xray wrappers. The script is still required because sync's
     * primary public surface lives there. */
    XrModule *module = xr_module_create_native(isolate, "sync");
    if (!module)
        return NULL;

    sync_export_native_class(isolate, module, "Semaphore", XR_TSEMAPHORE);
    sync_export_native_class(isolate, module, "CountdownLatch", XR_TCOUNTDOWNLATCH);
    sync_export_native_class(isolate, module, "EventCount", XR_TEVENTCOUNT);
    sync_export_native_class(isolate, module, "WorkQueue", XR_TWORKQUEUE);
    sync_export_native_class(isolate, module, "ResultGroup", XR_TRESULTGROUP);

    module->requires_script = true;
    return module;
}
