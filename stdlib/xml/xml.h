/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xml.h - XML standard library module loader
 *
 * Public XML semantics live in stdlib/xml/xml.xr. This header exposes only the
 * native loader anchor used by the stdlib resolver.
 */

#ifndef XR_STDLIB_XML_H
#define XR_STDLIB_XML_H

#include "../../src/base/xdefs.h"

struct XrVMRuntime;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_xml(struct XrVMRuntime *isolate);

#endif  // XR_STDLIB_XML_H
