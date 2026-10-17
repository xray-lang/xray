/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_layout_input_internal.h - Physical input to the single layout engine
 *
 * KEY CONCEPT:
 *   Synchronous readers locate current fields and confer no semantic authority.
 */
#ifndef XXIR_LAYOUT_INPUT_INTERNAL_H
#define XXIR_LAYOUT_INPUT_INTERNAL_H
#include "xxir_input_owned.h"
#include "xxir_metadata_domain_internal.h"

typedef struct XirLayoutInput {
    XirMetadataDomain domain;
    const void *owner;
    XrXirStatus (*head)(const XrXirCompileContext *, const void *, uint32_t,
        XirInputFunctionHead *);
    XrXirStatus (*instruction)(const XrXirCompileContext *, const void *, uint32_t,
        uint32_t, XrXirInstruction *, const XrXirInstruction **);
} XirLayoutInput;

/* The caller independently admits the complete current Input and metadata.
 * This entry checks owned layout ranges and the same target layout algorithm;
 * it neither allocates nor retains a reader, permission or workspace. */
XR_FUNC XrXirStatus xir_layout_input_match(const XrXirCompileContext *context,
    const XirLayoutInput *input, const XrXirTarget *target,
    XrXirFunctionLayout *layouts);
#endif // XXIR_LAYOUT_INPUT_INTERNAL_H
