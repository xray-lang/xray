/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_shape_oracle.h - Provider-backed generated-C to assembly realization
 */

#ifndef XTC_SHAPE_ORACLE_H
#define XTC_SHAPE_ORACLE_H

#include "xtc_probe.h"

typedef struct XrToolchainAssemblyArtifact {
    char *text;
    size_t size;
    XrToolchainProbeResult probe;
} XrToolchainAssemblyArtifact;

XR_FUNC bool xtc_shape_oracle_realize(const XrToolchainProbeOptions *options,
                                      const char *generated_c, XrToolchainAssemblyArtifact *out,
                                      char *err, size_t err_size);
XR_FUNC void xtc_shape_oracle_free(XrToolchainAssemblyArtifact *artifact);

#endif /* XTC_SHAPE_ORACLE_H */
