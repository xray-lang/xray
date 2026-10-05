/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_types.h - Canonical owned constructed-type identities
 *
 * KEY CONCEPT:
 *   A local type ID has a kind only in its verified owning descriptor pool.
 */
#ifndef XXIR_TYPES_H
#define XXIR_TYPES_H
#include "xxir.h"
#include "xxir_nominal.h"
XR_FUNC const XrXirTypeNode *xr_xir_type_node(const XrXirTypes *types, XrXirType type);
XR_FUNC const XrXirTypeNode *xr_xir_callable_signature(const XrXirTypes *types, XrXirType type);
/* Ordered tuple fields use mode-zero parameter storage; arity zero is Unit. */
XR_FUNC const XrXirTypeNode *xr_xir_tuple_signature(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_callable(const XrXirTypes *types, XrXirType type);
/* The shared walker borrows an explicit work policy in either allocation domain. */
XR_FUNC XrXirStatus xr_xir_callable_weakening_admit(const XrXirTypes *types,
    XrXirType source, XrXirType target, void *work_owner, bool (*charge)(void *, uint64_t));
XR_FUNC XrXirStatus xr_xir_compile_callable_weakening(const XrXirCompileContext *compile_context, const XrXirTypes *types, XrXirType source, XrXirType target);
XR_FUNC bool xr_xir_type_is_array(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_nullable(const XrXirTypes *types, XrXirType type);
XR_FUNC XrXirType xr_xir_nullable_element(const XrXirTypes *types, XrXirType type);
/* Bounded closed class storage capability; grants no access or generic proof. */
XR_FUNC XrXirStatus xr_xir_compile_class_field_verify(const XrXirCompileContext *compile_context, const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_cell(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_nominal(const XrXirTypes *types, XrXirType type);
/* Classification grants neither descriptor validity nor operation authority. */
XR_FUNC bool xr_xir_type_is_struct(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_enum(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_class(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_owned(const XrXirTypes *types, XrXirType type);
XR_FUNC XrXirType xr_xir_cell_element(const XrXirTypes *types, XrXirType type);
XR_FUNC XrXirType xr_xir_array_element(const XrXirTypes *types, XrXirType type);
XR_FUNC uint32_t xr_xir_type_span(const XrXirTypes *types, XrXirType type);
/* Structural admission precedes declaration-context constraint proofs. */
XR_FUNC XrXirStatus xr_xir_compile_type_expression_shape(const XrXirCompileContext *compile_context, const XrXirTypes *types, XrXirType type, uint32_t parameter_count);
XR_FUNC XrXirStatus xr_xir_compile_type_descriptors_verify(const XrXirCompileContext *compile_context, const XrXirTypes *types);
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches(const XrXirCompileContext *compile_context, const XrXirTypes *types, const XrXirType *arguments, uint32_t count, XrXirType expected, XrXirType actual);
/* Pools share nominal declaration identities; arguments name destination types. */
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches_between(const XrXirCompileContext *compile_context, const XrXirTypes *source_types, const XrXirTypes *types, const XrXirType *arguments, uint32_t count, XrXirType expected, XrXirType actual);
XR_FUNC XrXirStatus xr_xir_compile_types_structure_verify(const XrXirCompileContext *compile_context, const XrXirTypes *types);
/* Counts parameter slots in a structurally verified pool, bounded by its context. */
XR_FUNC XrXirStatus xr_xir_compile_types_parameter_count(const XrXirCompileContext *context,
    const XrXirTypes *types, uint32_t *output);
/* Snapshot cloning requires successful descriptor-pool verification first. */
XR_FUNC XrXirStatus xr_xir_compile_types_clone(const XrXirCompileContext *compile_context, const XrXirTypes *types, XrXirTypes **output);
XR_FUNC void xr_xir_compile_types_free(XrXirTypes *types);
/* Complete inline field storage; output offsets publish only after success. */
XR_FUNC XrXirStatus xr_xir_compile_nominal_layout(const XrXirCompileContext *compile_context, const XrXirTypes *types, XrXirType type, const XrXirTarget *target, XrXirLayout *layout, uint32_t *field_offsets, uint32_t field_count);
XR_FUNC XrXirType xr_xir_operand_type(const XrXirFunction *function, uint32_t value);
#endif // XXIR_TYPES_H
