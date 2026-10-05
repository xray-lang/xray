/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_checked.h - Owned target-neutral Checked packets
 *
 * KEY CONCEPT:
 *   Serialized stage claims never exempt decoded declarations from verification.
 */
#ifndef XXIR_CHECKED_H
#define XXIR_CHECKED_H
#include "xxir.h"
#define XR_XIR_CHECKED_SCHEMA 24u
#define XR_XIR_CHECKED_CONTRACT 63u
typedef struct XrXirCheckedPacket {
    uint8_t *bytes;
    size_t length;
} XrXirCheckedPacket;
XR_FUNC XrXirStatus xr_xir_compile_checked_write(const XrXirArtifact *artifact, XrXirCheckedPacket *output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_checked_read(const XrXirCompileContext *compile_context, const void *bytes, size_t length, XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC void xr_xir_compile_checked_packet_free(XrXirCheckedPacket *packet);
#endif // XXIR_CHECKED_H
