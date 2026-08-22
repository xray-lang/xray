/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_factories.h - Standard library module factory declarations
 *
 * KEY CONCEPT:
 *   Each stdlib module provides a factory function that creates and
 *   populates an XrModule with native C functions. These are registered
 *   during isolate initialization via xr_module_register_native_factory().
 */

#ifndef XMODULE_FACTORIES_H
#define XMODULE_FACTORIES_H

struct XrVMRuntime;
struct XrModule;

/* ========== Core Modules (always available) ========== */

XR_FUNC struct XrModule *xr_native_module_create_prelude(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_time(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_math(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_path(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_base64(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_regex(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_mem(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_runtime(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_sync(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_parallel(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_simd(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_codegen(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_sys(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_probe(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_url(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_datetime(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_log(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_encoding(struct XrVMRuntime *isolate);

/* ========== Filesystem Modules ========== */

#if defined(XR_HAS_FILESYSTEM) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_native_module_create_io(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_os(struct XrVMRuntime *isolate);
#endif

/* ========== Network Modules ========== */

#if defined(XR_HAS_TEST_MODULES)
XR_FUNC struct XrModule *xr_native_module_create_test_yield(struct XrVMRuntime *isolate);
#endif

#if defined(XR_HAS_NETWORK) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_native_module_create_net(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_http(struct XrVMRuntime *isolate);
#endif

/* ========== WebSocket Module ========== */

#if defined(XR_HAS_WS) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_native_module_create_ws(struct XrVMRuntime *isolate);
#endif

#if defined(XR_HAS_HTTP2) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_native_module_create_http2(struct XrVMRuntime *isolate);
#endif

/* ========== Crypto Module ========== */

#if defined(XR_HAS_CRYPTO) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_native_module_create_crypto(struct XrVMRuntime *isolate);
#endif

/* ========== Compression Module ========== */

#if defined(XR_HAS_COMPRESS) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_native_module_create_compress(struct XrVMRuntime *isolate);
#endif

/* ========== Cluster Module ========== */

#if defined(XR_HAS_CLUSTER) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_native_module_create_cluster(struct XrVMRuntime *isolate);
#endif

/* ========== Data Format Modules ========== */

XR_FUNC struct XrModule *xr_native_module_create_text(struct XrVMRuntime *isolate);

#if defined(XR_HAS_DATA_FORMATS) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_native_module_create_csv(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_toml(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_yaml(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_native_module_create_xml(struct XrVMRuntime *isolate);
#endif

#endif /* XMODULE_FACTORIES_H */
