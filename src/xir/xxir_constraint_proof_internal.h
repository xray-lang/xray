/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_constraint_proof_internal.h - Owned scratch storage for proof queries
 *
 * KEY CONCEPT:
 *   A verification operation reuses storage on its resource ledger. Each query
 *   rebuilds proof facts and authenticates its current declaration context.
 */
#ifndef XXIR_CONSTRAINT_PROOF_INTERNAL_H
#define XXIR_CONSTRAINT_PROOF_INTERNAL_H
#include "xxir_constraint_proof.h"
#include "xxir_metadata_domain_internal.h"
#include "xxir_read_scratch.h"
struct ConstraintProofMemory;
typedef struct XirConstraintScratch {
    XrCompileResources *resources;
    struct ConstraintProofMemory *memory;
} XirConstraintScratch;
XR_FUNC XrXirStatus xr_xir_compile_type_use_verify_scratch(const XrXirCompileContext *context,
    const XrXirProofContext *proof, XrXirType type, XirConstraintScratch *scratch);
/* A whole declaration argument vector is one operation. Every binder remains
 * a goal; only authentic declaration constraints seed its environment. */
typedef struct XirConstraintArguments {
    const XrXirModule *declaration_module;
    XrXirDeclarationContext declaration;
    const XrXirType *arguments;
    uint32_t argument_count;
} XirConstraintArguments;
XR_FUNC XrXirStatus xr_xir_compile_constraint_arguments_prove(const XrXirCompileContext *context,
    const XrXirProofContext *proof, const XirConstraintArguments *use);
/* Domain queries use the same obligation FIFO as raw queries. Scratch keeps
 * only allocation capacity; no domain pointer or proof fact survives a call. */
XR_FUNC XrXirStatus xir_metadata_type_use_verify_scratch(const XrXirCompileContext *context,
    const XirMetadataProofContext *proof,XrXirType type,XirConstraintScratch *scratch);
XR_FUNC XrXirStatus xir_metadata_context_constraints_verify(const XrXirCompileContext *context,
    const XirMetadataProofContext *proof);
XR_FUNC XrXirStatus xir_metadata_type_markers_prove(const XrXirCompileContext *context,
    const XirMetadataProofContext *proof,XrXirType type,uint32_t markers);
/* Missing capacity unwinds the complete read. The owner grows storage and
 * reframes actual Input before retrying; no proof fact survives the attempt. */
XR_FUNC XrXirStatus xir_metadata_type_markers_read(const XrXirCompileContext *context,
    const XirMetadataProofContext *proof,XrXirType type,uint32_t markers,XirReadScratch *memory);
/* The closed pool is already structurally admitted. Each enum root completes
 * the ordinary marker query independently; only temporary capacity is reused.
 * Output belongs to an unpublished owner and has exactly types->count bytes. */
XR_FUNC XrXirStatus xir_compile_enum_sendable_flags(const XrXirCompileContext *context,
    const XrXirTypes *types,uint8_t *output,uint32_t count);
XR_FUNC XrXirStatus xir_metadata_constraint_arguments_prove(const XrXirCompileContext *context,
    const XirMetadataProofContext *proof,const XirMetadataArguments *use);
XR_FUNC void xr_xir_constraint_scratch_free(XirConstraintScratch *scratch);
#endif // XXIR_CONSTRAINT_PROOF_INTERNAL_H
