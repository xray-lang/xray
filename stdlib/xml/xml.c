/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xml.c - `xml` module loader (pure-Xray XML parser/writer)
 *
 * KEY CONCEPT:
 *   `xml` is a pure-Xray stdlib module. Public parsing, serialization, and
 *   helper node construction live in stdlib/xml/xml.xr. src/base/xxml remains
 *   a low-level C parser for base-layer tests/tooling, not the stdlib surface.
 *
 *   This loader only creates the empty native module so the resolver recognises
 *   `import xml` as stdlib; the script-extension path compiles
 *   stdlib/xml/xml.xr and populates the module exports.
 */

#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/xisolate_api.h"

XR_FUNC XrModule *xr_native_module_create_xml(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_native_module_create_xml: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "xml");
    if (!module)
        return NULL;

    module->requires_script = true;
    return module;
}
