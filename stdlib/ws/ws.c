/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws.c - `ws` module loader (pure-Xray WebSocket client/server)
 *
 * KEY CONCEPT:
 *   `ws` is a pure-Xray stdlib module. The RFC 6455 handshake, frame codec,
 *   connection lifecycle and the send/recv/ping/close/serve semantics all live
 *   in stdlib/ws/ws.xr, layered over the net module's socket primitives from
 *   Xray code. No native data plane remains in this module.
 *
 *   This loader only creates the empty native module so the resolver recognises
 *   `import ws` as stdlib; the script-extension path then compiles
 *   stdlib/ws/ws.xr and populates the module exports.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_ws(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_ws: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "ws");
    if (!module)
        return NULL;

    module->requires_script = true;
    return module;
}
