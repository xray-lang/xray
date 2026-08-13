/*
 * base64.c - Loader for the pure-Xray Base64 module.
 *
 * All codec semantics live in base64.xr.  Native WebSocket framing owns its
 * private codec in stdlib/ws, so stdlib retains no C algorithm.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_native_module_create_base64(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_native_module_create_base64: NULL isolate");
    XrModule *module = xr_module_create_native(isolate, "base64");
    if (!module)
        return NULL;
    module->requires_script = true;
    return module;
}
