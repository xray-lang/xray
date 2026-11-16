/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Declaration-owned builtin Hashable witnesses, distinct from interfaces. */
#ifndef XXIR_HASHABLE_INTERNAL_H
#define XXIR_HASHABLE_INTERNAL_H
#include "xxir_metadata_domain_internal.h"
#include "xxir_read_scratch.h"
typedef struct XirHashableTargets {
    uint32_t functions[2];
    const XrXirType *arguments;
    uint32_t argument_count;
} XirHashableTargets;
/* IDs are candidates, never saved verification results. Each query reads the
 * current owner's actual signatures and checks their nominal identity. The
 * caller proves both method binders in the same operation's obligation FIFO;
 * these observations neither seed facts nor authorize executable dispatch. */
XR_FUNC XrXirStatus xir_hashable_witness_targets(const XrXirCompileContext *,
    const XirMetadataProofContext *,XrXirType,XirReadScratch *,
    XrCompileRequestControl *,XirHashableTargets *);
XR_FUNC XrXirStatus xir_hashable_witness_read(const XrXirCompileContext *,
    const XirMetadataProofContext *,XrXirType,XirReadScratch *,XrCompileRequestControl *);
XR_FUNC XrXirStatus xir_hashable_witnesses_verify(const XrXirCompileContext *,
    const XrXirModule *,XrCompileRequestControl *);
XR_FUNC XrXirStatus xir_hashable_witnesses_shape(const XrXirCompileContext *,
    const XrXirTypes *,const XrXirDeclarations *,uint32_t,XrCompileRequestControl *);
XR_FUNC XrXirStatus xir_hashable_witnesses_sort(const XrXirCompileContext *,
    const XrXirTypes *,XrXirHashableWitness *,uint32_t,XrCompileRequestControl *);
#endif
