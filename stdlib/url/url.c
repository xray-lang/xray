/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * url.c - URL C API + pure-Xray module loader
 *
 * KEY CONCEPT:
 *   The user-facing url module is implemented in stdlib/url/url.xr. This file
 *   keeps the small C-level percent encode/decode API used by unit tests and
 *   creates the stdlib module shell so `import url` resolves through the normal
 *   script-extension path.
 */

#include "url.h"
#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/shared/xr_url_core.h"

XR_FUNC int xr_url_encode(const char *str, size_t len, char *buf, size_t buf_size) {
    return xr_url_core_encode_bounded(str, len, false, buf, buf_size);
}

XR_FUNC int xr_url_decode(const char *str, size_t len, char *buf, size_t buf_size) {
    return xr_url_core_decode_bounded(str, len, false, buf, buf_size);
}

XR_FUNC int xr_url_encode_form(const char *str, size_t len, char *buf, size_t buf_size) {
    return xr_url_core_encode_bounded(str, len, true, buf, buf_size);
}

XR_FUNC int xr_url_decode_form(const char *str, size_t len, char *buf, size_t buf_size) {
    return xr_url_core_decode_bounded(str, len, true, buf, buf_size);
}

XR_FUNC XrModule *xr_load_module_url(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_url: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "url");
    if (!module)
        return NULL;

    module->requires_script = true;
    module->loaded = true;
    return module;
}
