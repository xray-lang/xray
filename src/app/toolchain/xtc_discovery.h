/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_discovery.h - Provider candidate discovery and concrete selection
 */

#ifndef XTC_DISCOVERY_H
#define XTC_DISCOVERY_H

#include "xtc_model.h"

#define XTC_MAX_CANDIDATES 16

typedef struct XrToolchainRequest {
    XrToolchainSelector selector;
    XrToolchainTarget target;
    const char *cc;
    const char *zig;
    const char *program_hint;
} XrToolchainRequest;

typedef struct XrToolchainCandidate {
    XrToolchainProviderId provider;
    XrToolchainOwnership ownership;
    char executable[1200];
    char version[512];
    bool runnable;
} XrToolchainCandidate;

typedef struct XrToolchainCandidates {
    XrToolchainCandidate items[XTC_MAX_CANDIDATES];
    size_t count;
} XrToolchainCandidates;

XR_FUNC bool xtc_find_executable(const char *program, char *out, size_t out_size);
XR_FUNC bool xtc_find_bundled_zig(const char *program_hint, char *out, size_t out_size);
XR_FUNC bool xtc_active_apple_sdk(char *out, size_t out_size, char *err, size_t err_size);
XR_FUNC bool xtc_discover_candidates(const XrToolchainRequest *request, XrToolchainCandidates *out,
                                     char *err, size_t err_size);
XR_FUNC bool xtc_version_from_banner(const uint8_t *source, size_t source_size, char *version,
                                     size_t version_size);
XR_FUNC bool xtc_candidate_read_version(XrToolchainCandidate *candidate, char *err,
                                        size_t err_size);
XR_FUNC bool xtc_selector_accepts_provider(XrToolchainSelector selector,
                                           XrToolchainProviderId provider);
XR_FUNC bool xtc_select_discovered(const XrToolchainRequest *request, XrToolchainSelection *out,
                                   char *err, size_t err_size);

#endif /* XTC_DISCOVERY_H */
