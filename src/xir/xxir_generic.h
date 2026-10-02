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
XR_FUNC XrXirStatus xr_xir_compile_specialize(const XrXirArtifact *checked, XrXirArtifact **output, XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xr_xir_compile_generics_structure_verify(const XrXirCompileContext *compile_context, const XrXirModule *module);
XR_FUNC XrXirStatus xr_xir_compile_result_binders_verify(const XrXirCompileContext *compile_context, const XrXirModule *module);
XR_FUNC XrXirStatus xr_xir_compile_result_argument(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t caller, XrXirType type);
XR_FUNC XrXirStatus xr_xir_compile_result_unit_use(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t caller, uint32_t argument);
XR_FUNC XrXirStatus xr_xir_compile_generics_clone(const XrXirCompileContext *compile_context, const XrXirModule *module, XrXirGeneric **output);
XR_FUNC void xr_xir_compile_generics_free(XrXirGeneric *generics, uint32_t functions);
XR_FUNC bool xr_xir_type_in_context(const XrXirModule *module, uint32_t function, XrXirType type);
/* Constraint proof is independent of naming authority at a substitution site. */
XR_FUNC XrXirStatus xr_xir_compile_type_constraints(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function, XrXirType type, XrXirConstraint constraints);
XR_FUNC XrXirStatus xr_xir_compile_type_satisfies(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function, XrXirType type, XrXirConstraint constraints);
XR_FUNC XrXirStatus xr_xir_compile_generic_call(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t caller, const XrXirInstruction *call);
/* Requires a structurally verified module and a successful generic-call check. */
XR_FUNC XrXirStatus xr_xir_compile_call_type_matches(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t caller, const XrXirInstruction *call, XrXirType type, XrXirType actual);
#endif // XXIR_GENERIC_H
