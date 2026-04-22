/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_module_sdk.h - SDK header for third-party native packages
 *
 * This is the only header third-party native packages need to include.
 * It aggregates all public APIs required to implement a native module:
 * value system, GC header, object types, module creation, native type
 * registration, allocation, isolate accessors, and export macros.
 *
 * TECH DEBT (accepted for v1):
 *   Uses ../src/... relative paths — requires xray source tree at build time.
 *   Future: extract stable SDK headers to include/xray_sdk/.
 */

#ifndef XRAY_MODULE_SDK_H
#define XRAY_MODULE_SDK_H

/* ========== ABI Version Check ========== */
/* Increment when XrGCHeader, XrValue, or XrModule layout changes.
 * Third-party packages compiled against a different ABI version will
 * fail to load at runtime with a clear error message. */
#define XRAY_MODULE_ABI_VERSION 1

/* ========== Value System ========== */
#include "../src/runtime/value/xvalue.h"

/* ========== GC Header (for custom types) ========== */
#include "../src/runtime/gc/xgc_header.h"

/* ========== Core Object Types ========== */
#include "../src/runtime/object/xstring.h"
#include "../src/runtime/object/xarray.h"
#include "../src/runtime/object/xjson.h"
#include "../src/runtime/object/xshape.h"

/* ========== Module Creation and Export ========== */
#include "../src/module/xmodule.h"
#include "../src/module/xbuiltin_decl.h"

/* ========== Native Type Registration ========== */
#include "../src/runtime/object/xnative_type.h"

/* ========== GC Allocation ========== */
#include "../src/runtime/gc/xgc.h"

/* ========== Per-Coroutine GC (mark API for traverse callbacks) ========== */
#include "../src/runtime/gc/xcoro_gc.h"

/* ========== Error Reporting ========== */
#include "../src/api/xruntime.h"

/* ========== Isolate API (opaque accessors) ========== */
#include "../src/runtime/xisolate_api.h"

/* ========== Memory ========== */
#include "../src/base/xmalloc.h"

/* ========== Checks ========== */
#include "../src/base/xchecks.h"

/* ========== CFunction Definition ========== */
#include "../src/runtime/xexec_frame.h"

/* ========== Symbol Table ========== */
#include "../src/runtime/symbol/xsymbol.h"

/* ========== Export Macros (same as stdlib/common.h) ========== */
/* XRS_EXPORT(mod, isolate, name, func)
 * XRS_EXPORT_SLOW(mod, isolate, name, func) */
#include "../stdlib/common.h"

/* ========== Dynamic Extension Type Allocation ========== */
/* Third-party packages allocate type IDs at load time:
 *   uint8_t my_type = xr_alloc_extension_type(isolate, "MyType");
 * See xisolate_api.h for declaration. */

/* ========== Module Entry Point Macro ========== */
/* Third-party packages must export two symbols:
 *   1. xr_module_abi_version_<name>  (const int, for ABI check)
 *   2. xr_load_module_<name>         (loader function)
 *
 * Usage:
 *   XRAY_MODULE_ENTRY(sqlite) {
 *       XrModule *mod = xr_module_create_native(isolate, "sqlite");
 *       ...
 *       return mod;
 *   }
 */
#define XRAY_MODULE_ENTRY(name) \
    __attribute__((visibility("default"))) \
    const int xr_module_abi_version_##name = XRAY_MODULE_ABI_VERSION; \
    __attribute__((visibility("default"))) \
    XrModule* xr_load_module_##name(XrayIsolate *isolate)

#endif // XRAY_MODULE_SDK_H
