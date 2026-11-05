/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_normalize_internal.h - Unpublished lifetime rewrite and relocation
 */
#ifndef XXIR_VIEW_NORMALIZE_INTERNAL_H
#define XXIR_VIEW_NORMALIZE_INTERNAL_H
#include "xxir.h"
typedef struct XirViewNormalizedFunction {
    XrXirInstruction *instructions;
    XrXirBlock *blocks;
    uint32_t *operands,*values,*boundaries;
    uint32_t old_instructions;
} XirViewNormalizedFunction;
typedef struct XirViewNormalization {
    XrXirModule module;
    XrXirFunction *bodies;
    XirViewNormalizedFunction *functions;
} XirViewNormalization;
typedef struct XirViewRelocation {
    const XrXirModule *module;
    const XrXirFunction *source;
    XrXirFunction *destination;
    const uint32_t *values,*boundaries;
    uint32_t first_instruction,first_block;
} XirViewRelocation;
/* Complete ordinary admission and borrowed owner checking are required on
 * this immutable producer generation. Input contains no VIEW_CLEANUP yet.
 * Only instructions, blocks, operands and maps are owned; other fields borrow
 * the source. No Artifact, root access or complete admission is published.
 * Consumers copy all output into their receiving owner, relocate every other
 * source identity, then repeat complete admission before publication. */
XR_FUNC XrXirStatus xir_compile_view_normalize_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XirViewNormalization **output,XrXirDiagnostic *diagnostic);
/* Synchronous Source owner checks each copied function against this exact
 * immutable receiving generation. No caller bitset is executable authority.
 * Complete ordinary, origin, affine and liveness admission must precede it;
 * every normalized output repeats complete common admission. */
typedef struct XirViewNormalizationSource {
    const XrXirModule *generation;
    void *owner;
    XrXirStatus (*checked_copy)(void *,const XrXirModule *,uint32_t,bool *);
} XirViewNormalizationSource;
XR_FUNC XrXirStatus xir_compile_view_normalize_source_verified(const XrXirCompileContext *context,
    const XrXirModule *module,const XirViewNormalizationSource *source,
    XirViewNormalization **output,XrXirDiagnostic *diagnostic);
XR_FUNC void xir_compile_view_normalization_free(XirViewNormalization *normalization);
/* Mechanical relocation into producer-owned writable arrays, never a proof.
 * UINT32_MAX is retained only for detached, non-executable source snapshots.
 * Prefix arrays before first_instruction/first_block are already relocated. */
XR_FUNC XrXirStatus xir_compile_view_relocate(const XrXirCompileContext *context,
    const XirViewRelocation *request);
#endif // XXIR_VIEW_NORMALIZE_INTERNAL_H
