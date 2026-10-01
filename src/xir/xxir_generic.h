/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_generic.h - Definition-context type parameters and constraint proofs
 *
 * KEY CONCEPT:
 *   Substitution cannot add operations or capabilities absent from a declaration.
 */
#ifndef XXIR_GENERIC_H
#define XXIR_GENERIC_H
#include "xxir.h"
XR_FUNC XrXirStatus xr_xir_specialize(const XrXirArtifact *checked, const XrXirBudget *budget,
    XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_generics_structure_verify(const XrXirModule *module, XrXirBudget *remaining);
XR_FUNC XrXirStatus xr_xir_result_binders_verify(const XrXirModule *module, XrXirBudget *remaining);
XR_FUNC XrXirStatus xr_xir_result_argument(const XrXirModule *module, uint32_t caller,
    XrXirType type, XrXirBudget *remaining);
XR_FUNC XrXirStatus xr_xir_result_unit_use(const XrXirModule *module, uint32_t caller,
    uint32_t argument, XrXirBudget *remaining);
XR_FUNC XrXirStatus xr_xir_generics_clone(const XrXirModule *module, XrXirGeneric **output);
XR_FUNC void xr_xir_generics_free(XrXirGeneric *generics, uint32_t functions);
XR_FUNC bool xr_xir_type_in_context(const XrXirModule *module, uint32_t function, XrXirType type);
/* Constraint proof is independent of naming authority at a substitution site. */
XR_FUNC XrXirStatus xr_xir_type_constraints(const XrXirModule *module, uint32_t function,
    XrXirType type, XrXirConstraint constraints, XrXirBudget *remaining);
XR_FUNC XrXirStatus xr_xir_type_satisfies(const XrXirModule *module, uint32_t function,
    XrXirType type, XrXirConstraint constraints, XrXirBudget *remaining);
XR_FUNC XrXirStatus xr_xir_generic_call(const XrXirModule *module, uint32_t caller,
    const XrXirInstruction *call, XrXirBudget *remaining);
/* Requires a structurally verified module and a successful generic-call check. */
XR_FUNC XrXirStatus xr_xir_call_type_matches(const XrXirModule *module, uint32_t caller,
    const XrXirInstruction *call, XrXirType type, XrXirType actual, XrXirBudget *remaining);
#endif // XXIR_GENERIC_H
