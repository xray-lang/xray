/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_constraint_proof.h - Explicit declaration environments and substitutions
 *
 * KEY CONCEPT:
 *   Required parameters are substituted as a complete vector before entailment.
 */
#ifndef XXIR_CONSTRAINT_PROOF_H
#define XXIR_CONSTRAINT_PROOF_H
#include "xxir_interface.h"

typedef enum XrXirContextKind {
    XR_XIR_CONTEXT_FUNCTION, XR_XIR_CONTEXT_NOMINAL, XR_XIR_CONTEXT_INTERFACE, XR_XIR_CONTEXT_CLOSED
} XrXirContextKind;
typedef struct XrXirDeclarationContext {
    XrXirContextKind kind;
    uint32_t declaration;
} XrXirDeclarationContext;
typedef struct XrXirProofContext {
    const XrXirModule *module;
    XrXirDeclarationContext owner;
} XrXirProofContext;
typedef struct XrXirConstraintUse {
    const XrXirModule *declaration_module;
    XrXirDeclarationContext declaration;
    uint32_t parameter;
    const XrXirType *arguments;
    uint32_t argument_count;
} XrXirConstraintUse;
/* Derives facts and requirements only from their authentic declaration records.
 * Descriptor/declaration shapes and implementation signature priors must have
 * been admitted. Semantic failures consume budget; no pending obligation grants
 * success. Neither query admits a function body or naming access by itself. */
XR_FUNC XrXirStatus xr_xir_constraints_prove(const XrXirProofContext *context,
    const XrXirConstraintUse *use, XrXirBudget *budget);

/* Structural admission grants no proof authority. */
XR_FUNC XrXirStatus xr_xir_constraint_structure(const XrXirTypes *types,
    XrXirConstraint constraint, uint32_t parameter_count, XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_type_use_verify(const XrXirProofContext *context,
    XrXirType type, XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_type_markers_prove(const XrXirProofContext *context,
    XrXirType type, uint32_t markers, XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_context_constraints_verify(const XrXirProofContext *context,
    XrXirBudget *budget);
/* Catalog admission reads no initializer bodies or inferred instance slots. */
XR_FUNC XrXirStatus xr_xir_declaration_constraints_verify(const XrXirModule *module,
    XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_module_constraints_verify(const XrXirModule *module,
    XrXirBudget *budget);
/* Arguments and subject belong to context.module; identities and explicit
 * implementation rules belong to declaration_module. Cross-pool nominal
 * identities must already have authenticated provenance. */
XR_FUNC XrXirStatus xr_xir_interface_prove(const XrXirProofContext *context,
    const XrXirModule *declaration_module, XrXirType subject,
    XrXirInterfaceApplication application, XrXirBudget *budget);
#endif // XXIR_CONSTRAINT_PROOF_H
