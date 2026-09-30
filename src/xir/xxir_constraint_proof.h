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
    XR_XIR_CONTEXT_FUNCTION, XR_XIR_CONTEXT_NOMINAL, XR_XIR_CONTEXT_INTERFACE
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
    XrXirDeclarationContext declaration;
    uint32_t parameter;
    const XrXirType *arguments;
    uint32_t argument_count;
} XrXirConstraintUse;
/* Derives both environments from the module's sole declaration records. */
XR_FUNC XrXirStatus xr_xir_constraints_prove(const XrXirProofContext *context,
    const XrXirConstraintUse *use, XrXirBudget *budget);

/* Internal descriptor checking borrows the authentic declaration environment.
 * It grants neither naming authority nor a published Checked artifact. */
typedef struct XrXirConstraintEnvironment {
    const XrXirTypes *types;
    const XrXirConstraint *constraints;
    uint32_t parameter_count;
} XrXirConstraintEnvironment;
typedef struct XrXirConstraintSubstitution {
    const XrXirTypes *types;
    XrXirConstraint requirement;
    const XrXirType *arguments;
    uint32_t argument_count;
    XrXirType subject;
} XrXirConstraintSubstitution;
/* Requires shape-verified descriptors and an acyclic interface table. */
XR_FUNC XrXirStatus xr_xir_constraint_entails(const XrXirConstraintEnvironment *environment,
    const XrXirConstraintSubstitution *use, XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_constraint_structure(const XrXirTypes *types,
    XrXirConstraint constraint, uint32_t parameter_count, XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_constraint_arguments(const XrXirConstraintEnvironment *environment,
    const XrXirConstraint *requirements, const XrXirType *arguments,
    uint32_t count, XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_constraint_environment_verify(const XrXirConstraintEnvironment *environment,
    XrXirBudget *budget);
#endif // XXIR_CONSTRAINT_PROOF_H
