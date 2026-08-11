/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_artifact_kind.h - Canonical compiled-artifact identity probe
 *
 * KEY CONCEPT:
 *   Artifact bytes are the identity authority. A reserved extension can only
 *   confirm that authority or create a conflict; it never turns arbitrary
 *   bytes into an executable artifact.
 */

#ifndef XR_ARTIFACT_KIND_H
#define XR_ARTIFACT_KIND_H

#include "../../base/xdefs.h"
#include <stddef.h>
#include <stdint.h>

#define XR_ARTIFACT_PROBE_SIZE 8u
#define XR_XSM_ARTIFACT_MAGIC_SIZE 8u
#define XR_XTP_ARTIFACT_MAGIC_SIZE 4u
#define XR_LEGACY_XRC_ARTIFACT_MAGIC_SIZE 4u

XR_DATA const uint8_t xr_xsm_artifact_magic[XR_XSM_ARTIFACT_MAGIC_SIZE];
XR_DATA const uint8_t xr_xtp_artifact_magic[XR_XTP_ARTIFACT_MAGIC_SIZE];
XR_DATA const uint8_t xr_legacy_xrc_artifact_magic[XR_LEGACY_XRC_ARTIFACT_MAGIC_SIZE];

typedef enum XrArtifactKind {
    XR_ARTIFACT_KIND_SOURCE = 0,
    XR_ARTIFACT_KIND_LEGACY_XRC,
    XR_ARTIFACT_KIND_XSM,
    XR_ARTIFACT_KIND_XTP,
    XR_ARTIFACT_KIND_UNSUPPORTED,
    XR_ARTIFACT_KIND_CONFLICT,
} XrArtifactKind;

XR_FUNC XrArtifactKind xr_artifact_classify(const char *path,
                                             const uint8_t *prefix,
                                             size_t prefix_size);

#endif  // XR_ARTIFACT_KIND_H
