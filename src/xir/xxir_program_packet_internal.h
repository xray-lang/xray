/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_program_packet_internal.h - External transport spelling for C emission
 */
#ifndef XXIR_PROGRAM_PACKET_INTERNAL_H
#define XXIR_PROGRAM_PACKET_INTERNAL_H
#include "xxir_program.h"
/* The synchronous C writer owns the live Artifact borrow. This view grants
 * neither a semantic proof nor a Program; receiving must decode and verify. */
XR_FUNC XrXirProgramPacket xir_program_packet_borrow(const XrXirArtifact *artifact);
#endif // XXIR_PROGRAM_PACKET_INTERNAL_H
