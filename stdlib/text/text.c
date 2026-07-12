/* Pure-Xray text module loader. */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_load_module_text(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_text: NULL isolate");
    XrModule *module = xr_module_create_native(isolate, "text");
    if (!module)
        return NULL;
    module->requires_script = true;
    return module;
}
