/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_program_input_internal.h - Exclusive canonical Program input ownership
 *
 * KEY CONCEPT:
 *   A carrier moves real Input and Final storage; every receive verifies it.
 */
#ifndef XXIR_PROGRAM_INPUT_INTERNAL_H
#define XXIR_PROGRAM_INPUT_INTERNAL_H
#include "xxir_program.h"
#include "xxir_internal.h"

struct XrXirProgramInput {
    XrXirCompileContext context;
    XrXirArtifact *artifact;
};
/* Move physically owned matching body/layout storage, without granting a
 * semantic permission or retaining a verification result. Every receiver
 * independently verifies the same current Input and Final before execution. */
XR_FUNC XrXirStatus xir_program_input_take(const XrXirCompileContext *context,
    XrXirArtifact **artifact,XrXirProgramInput **output);
XR_FUNC XrXirStatus xir_program_input_read(const XrXirCompileContext *context,
    const XrXirProgramPacket *packet,const XrXirTarget *target,XrXirProgramInput **output);
XR_FUNC XrXirStatus xir_program_input_verify(const XrXirCompileContext *context,
    const XrXirProgramInput *input);
XR_FUNC void xir_program_input_free(XrXirProgramInput *input);
#endif // XXIR_PROGRAM_INPUT_INTERNAL_H
