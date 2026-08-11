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

static XrArtifactKind magic_identity(const uint8_t *prefix,
                                     size_t prefix_size) {
    static const uint8_t removed_xtp_v1_magic[8] = {
        'X', 'R', 'A', 'Y', 'X', 'T', 'P', 0};
    if (!prefix)
        return XR_ARTIFACT_KIND_SOURCE;
    if (prefix_size >= XR_XSM_ARTIFACT_MAGIC_SIZE &&
        memcmp(prefix, xr_xsm_artifact_magic,
               XR_XSM_ARTIFACT_MAGIC_SIZE) == 0)
        return XR_ARTIFACT_KIND_XSM;
    if (prefix_size >= XR_XTP_ARTIFACT_MAGIC_SIZE &&
        memcmp(prefix, xr_xtp_artifact_magic,
               XR_XTP_ARTIFACT_MAGIC_SIZE) == 0)
        return XR_ARTIFACT_KIND_XTP;
    if (prefix_size >= sizeof(removed_xtp_v1_magic) &&
        memcmp(prefix, removed_xtp_v1_magic,
               sizeof(removed_xtp_v1_magic)) == 0)
        return XR_ARTIFACT_KIND_UNSUPPORTED;
    if (prefix_size >= XR_LEGACY_XRC_ARTIFACT_MAGIC_SIZE &&
        memcmp(prefix, xr_legacy_xrc_artifact_magic,
               XR_LEGACY_XRC_ARTIFACT_MAGIC_SIZE) == 0)
        return XR_ARTIFACT_KIND_LEGACY_XRC;
    return XR_ARTIFACT_KIND_SOURCE;
}

XR_FUNCDEF XrArtifactKind xr_artifact_classify(const char *path,
                                               const uint8_t *prefix,
                                               size_t prefix_size) {
    XrArtifactKind claim = extension_claim(path);
    XrArtifactKind identity = magic_identity(prefix, prefix_size);
    if (claim != XR_ARTIFACT_KIND_SOURCE && claim != identity)
        return XR_ARTIFACT_KIND_CONFLICT;
    return identity;
}
