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
XR_FUNC void xr_xir_constraint_scratch_free(XirConstraintScratch *scratch);
#endif // XXIR_CONSTRAINT_PROOF_INTERNAL_H
