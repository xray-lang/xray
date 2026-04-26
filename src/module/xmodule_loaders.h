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

struct XrayIsolate;
struct XrModule;

/* ========== Core Modules (always available) ========== */

XR_FUNC struct XrModule *xr_load_module_time(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_math(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_json(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_path(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_base64(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_regex(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_gc(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_url(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_datetime(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_log(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_encoding(struct XrayIsolate *isolate);

/* ========== Filesystem Modules ========== */

#if defined(XR_HAS_FILESYSTEM) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_io(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_os(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_test_yield(struct XrayIsolate *isolate);
#endif  // ========== Network Modules ==========

#if defined(XR_HAS_NETWORK) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_net(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_http(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_ws(struct XrayIsolate *isolate);
#endif  // ========== Crypto Module ==========

#if defined(XR_HAS_CRYPTO) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_crypto(struct XrayIsolate *isolate);
#endif  // ========== Compression Module ==========

#if defined(XR_HAS_COMPRESS) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_compress(struct XrayIsolate *isolate);
#endif  // ========== Cluster Module ==========

#if defined(XR_HAS_CLUSTER) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_cluster(struct XrayIsolate *isolate);
#endif  // ========== Data Format Modules ==========

#if defined(XR_HAS_DATA_FORMATS) || !defined(XR_STDLIB_MODULAR)
XR_FUNC struct XrModule *xr_load_module_csv(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_toml(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_yaml(struct XrayIsolate *isolate);
XR_FUNC struct XrModule *xr_load_module_xml(struct XrayIsolate *isolate);
#endif

#endif  // XMODULE_LOADERS_H
