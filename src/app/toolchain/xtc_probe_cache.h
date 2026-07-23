/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#ifndef XTC_PROBE_CACHE_H
#define XTC_PROBE_CACHE_H

#include "xtc_probe.h"

#define XTC_PROBE_CACHE_MAX_ENTRIES 16

typedef struct XrToolchainProbeCacheEntry {
    char key[320];
    char provider[32];
    char fingerprint[80];
    char runtime_artifact[256];
    bool ready;
} XrToolchainProbeCacheEntry;

XR_FUNC bool xtc_probe_cache_load(const XrToolchainProbeOptions *options,
                                  XrToolchainProbeResult *result, bool *hit, char *err,
                                  size_t err_size);
XR_FUNC bool xtc_probe_cache_store(const XrToolchainProbeOptions *options,
                                   const XrToolchainProbeResult *result, char *err,
                                   size_t err_size);
XR_FUNC bool xtc_probe_cache_reset(const char *normalized_target, char *err, size_t err_size);
XR_FUNC bool xtc_probe_cache_list(const char *normalized_target,
                                  XrToolchainProbeCacheEntry *entries, size_t capacity,
                                  size_t *out_count, char *err, size_t err_size);

#endif /* XTC_PROBE_CACHE_H */
