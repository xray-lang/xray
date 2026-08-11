/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_artifact_kind.c - Canonical compiled-artifact identity probe
 */

#include "xr_artifact_kind.h"
#include <stdbool.h>
#include <string.h>

XR_DATADEF const uint8_t xr_xsm_artifact_magic[XR_XSM_ARTIFACT_MAGIC_SIZE] = {
    'X', 'R', 'A', 'Y', 'X', 'S', 'M', 0};
XR_DATADEF const uint8_t xr_xtp_artifact_magic[XR_XTP_ARTIFACT_MAGIC_SIZE] = {
    'X', 'T', 'P', 'F'};
XR_DATADEF const uint8_t
    xr_legacy_xrc_artifact_magic[XR_LEGACY_XRC_ARTIFACT_MAGIC_SIZE] = {
        'X', 'R', 'A', 'Y'};

static bool has_extension(const char *path, const char *extension) {
    if (!path || !extension)
        return false;
    size_t path_size = strlen(path);
    size_t extension_size = strlen(extension);
    return path_size >= extension_size &&
           strcmp(path + path_size - extension_size, extension) == 0;
}

static XrArtifactKind extension_claim(const char *path) {
    if (has_extension(path, ".xsm"))
        return XR_ARTIFACT_KIND_XSM;
    if (has_extension(path, ".xtp"))
        return XR_ARTIFACT_KIND_XTP;
    if (has_extension(path, ".xrc"))
        return XR_ARTIFACT_KIND_LEGACY_XRC;
    return XR_ARTIFACT_KIND_SOURCE;
}

static XrArtifactProbeResult result(XrArtifactProbeStatus status,
                                    XrArtifactKind kind,
                                    size_t required_size) {
    return (XrArtifactProbeResult) {
        .status = status,
        .kind = kind,
        .required_size = required_size,
    };
}

static bool is_partial_magic(const uint8_t *prefix, size_t prefix_size,
                             const uint8_t *magic, size_t magic_size) {
    return prefix_size < magic_size &&
           memcmp(prefix, magic, prefix_size) == 0;
}

static XrArtifactProbeResult magic_identity(const uint8_t *prefix,
                                            size_t prefix_size) {
    static const uint8_t removed_xtp_v1_magic[8] = {
        'X', 'R', 'A', 'Y', 'X', 'T', 'P', 0};
    if (!prefix || prefix_size == 0)
        return result(XR_ARTIFACT_PROBE_MATCH,
                      XR_ARTIFACT_KIND_SOURCE, 0);
    if (prefix_size >= XR_XSM_ARTIFACT_MAGIC_SIZE &&
        memcmp(prefix, xr_xsm_artifact_magic,
               XR_XSM_ARTIFACT_MAGIC_SIZE) == 0)
        return result(XR_ARTIFACT_PROBE_MATCH, XR_ARTIFACT_KIND_XSM,
                      XR_XSM_ARTIFACT_MAGIC_SIZE);
    if (prefix_size >= sizeof(removed_xtp_v1_magic) &&
        memcmp(prefix, removed_xtp_v1_magic,
               sizeof(removed_xtp_v1_magic)) == 0)
        return result(XR_ARTIFACT_PROBE_UNKNOWN_RESERVED,
                      XR_ARTIFACT_KIND_SOURCE, sizeof(removed_xtp_v1_magic));
    if (prefix_size >= XR_XTP_ARTIFACT_MAGIC_SIZE &&
        memcmp(prefix, xr_xtp_artifact_magic,
               XR_XTP_ARTIFACT_MAGIC_SIZE) == 0)
        return result(XR_ARTIFACT_PROBE_MATCH, XR_ARTIFACT_KIND_XTP,
                      XR_XTP_ARTIFACT_MAGIC_SIZE);
    if (is_partial_magic(prefix, prefix_size, xr_xsm_artifact_magic,
                         XR_XSM_ARTIFACT_MAGIC_SIZE) ||
        is_partial_magic(prefix, prefix_size, removed_xtp_v1_magic,
                         sizeof(removed_xtp_v1_magic)))
        return result(XR_ARTIFACT_PROBE_NEED_MORE,
                      XR_ARTIFACT_KIND_SOURCE, XR_ARTIFACT_PROBE_SIZE);
    if (is_partial_magic(prefix, prefix_size, xr_xtp_artifact_magic,
                         XR_XTP_ARTIFACT_MAGIC_SIZE))
        return result(XR_ARTIFACT_PROBE_NEED_MORE,
                      XR_ARTIFACT_KIND_SOURCE, XR_XTP_ARTIFACT_MAGIC_SIZE);
    if (prefix_size >= XR_LEGACY_XRC_ARTIFACT_MAGIC_SIZE &&
        memcmp(prefix, xr_legacy_xrc_artifact_magic,
               XR_LEGACY_XRC_ARTIFACT_MAGIC_SIZE) == 0) {
        if (prefix_size < XR_LEGACY_XRC_HEADER_IDENTITY_SIZE)
            return result(XR_ARTIFACT_PROBE_NEED_MORE,
                          XR_ARTIFACT_KIND_SOURCE,
                          XR_LEGACY_XRC_HEADER_IDENTITY_SIZE);
        uint16_t version = (uint16_t) prefix[4] |
                           ((uint16_t) prefix[5] << 8);
        if (version == XR_LEGACY_XRC_VERSION)
            return result(XR_ARTIFACT_PROBE_MATCH,
                          XR_ARTIFACT_KIND_LEGACY_XRC,
                          XR_LEGACY_XRC_HEADER_IDENTITY_SIZE);
        return result(XR_ARTIFACT_PROBE_UNKNOWN_RESERVED,
                      XR_ARTIFACT_KIND_SOURCE,
                      XR_LEGACY_XRC_HEADER_IDENTITY_SIZE);
    }
    return result(XR_ARTIFACT_PROBE_MATCH, XR_ARTIFACT_KIND_SOURCE, 0);
}

XR_FUNCDEF XrArtifactProbeResult xr_artifact_probe(const char *path,
                                                   const uint8_t *prefix,
                                                   size_t prefix_size) {
    XrArtifactKind claim = extension_claim(path);
    XrArtifactProbeResult probe = magic_identity(prefix, prefix_size);
    if (probe.status == XR_ARTIFACT_PROBE_MATCH &&
        claim != XR_ARTIFACT_KIND_SOURCE && claim != probe.kind)
        probe.status = XR_ARTIFACT_PROBE_CONFLICT;
    return probe;
}
