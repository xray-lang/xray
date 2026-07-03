/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_loaders.h - Standard library module loader declarations
 *
 * KEY CONCEPT:
 *   Each stdlib module provides a loader function that creates and
 *   populates an XrModule with native C functions. These are registered
 *   during isolate initialization via xr_module_register_native().
 */

#ifndef XMODULE_LOADERS_H
#define XMODULE_LOADERS_H

struct XrVMRuntime;
struct XrModule;

/* ========== Core Modules (always available) ========== */

XR_FUNC struct XrModule *xr_load_module_prelude(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_time(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_math(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_path(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_base64(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_regex(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_mem(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_runtime(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_sync(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_sys(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_probe(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_url(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_datetime(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_log(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_encoding(struct XrVMRuntime *isolate);

/* ========== Filesystem Modules ========== */

#if defined(XR_HAS_FILESYSTEM) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_io(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_os(struct XrVMRuntime *isolate);
#endif  // ========== Network Modules ==========

#if defined(XR_HAS_TEST_MODULES)
XR_FUNC struct XrModule *xr_load_module_test_yield(struct XrVMRuntime *isolate);
#endif

#if defined(XR_HAS_NETWORK) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_net(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_http(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_ws(struct XrVMRuntime *isolate);
#endif  // ========== Crypto Module ==========

#if defined(XR_HAS_CRYPTO) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_crypto(struct XrVMRuntime *isolate);
#endif  // ========== Compression Module ==========

#if defined(XR_HAS_COMPRESS) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_compress(struct XrVMRuntime *isolate);
#endif  // ========== Cluster Module ==========

#if defined(XR_HAS_CLUSTER) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_cluster(struct XrVMRuntime *isolate);
#endif  // ========== Data Format Modules ==========

#if defined(XR_HAS_DATA_FORMATS) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_csv(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_toml(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_yaml(struct XrVMRuntime *isolate);
XR_FUNC struct XrModule *xr_load_module_xml(struct XrVMRuntime *isolate);
#endif

#endif  // XMODULE_LOADERS_H
