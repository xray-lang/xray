/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_native_cache_internal.h - Owned Checked and static native pairs
 *
 * KEY CONCEPT: Trusted registry entries accelerate the same sealed program.
 */
#ifndef XXIR_NATIVE_CACHE_INTERNAL_H
#define XXIR_NATIVE_CACHE_INTERNAL_H
#include "xxir_program.h"
#include "xxir_library_catalog.h"
typedef struct XirNativeCache XirNativeCache;
typedef enum XirNativeCacheHitKind {
    XIR_NATIVE_CACHE_MISS = 0, XIR_NATIVE_CACHE_MATCH = 1
} XirNativeCacheHitKind;
typedef struct XirNativeCacheHit {
    XirNativeCacheHitKind kind;
    XrXirCallEntry entry;
} XirNativeCacheHit;
/* Only the build-authenticated static registry supplies code. Failure preserves
 * output; no caller-supplied callback or digest can grant execution authority. */
XR_FUNC XrXirStatus xir_native_cache_open(const XrXirCompileContext *context,
    XirNativeCache **output);
/* The trusted registry supplies only Checked bytes to this compiler-owned
 * catalog. Native code admission remains the VM sealing worker's responsibility.
 * Construction uses the caller's ledger, copies its root, and preserves output. */
XR_FUNC XrXirStatus xir_native_cache_library_catalog_new(const XrXirCompileContext *context,
    const char *physical_root, XrXirLibraryCatalog **output);
XR_FUNC XrXirStatus xir_native_cache_retain(XirNativeCache *cache);
XR_FUNC void xir_native_cache_drop(XirNativeCache *cache);
XR_FUNC const XrXirCompileContext *xir_native_cache_context(const XirNativeCache *cache);
/* The whole Lowered input must first be verified. A miss keeps its VM binding;
 * an advertised corrupt match fails, with output unchanged. Success borrows the
 * entry metadata and code until the retained cache lease is released. */
XR_FUNC XrXirStatus xir_native_cache_lookup(XirNativeCache *cache,
    const XrXirArtifact *lowered, uint32_t function, XirNativeCacheHit *output);
#endif
