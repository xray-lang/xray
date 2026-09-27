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
XR_FUNC const XrXirTypeNode *xr_xir_type_node(const XrXirTypes *types, XrXirType type);
XR_FUNC const XrXirTypeNode *xr_xir_callable_signature(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_callable(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_array(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_cell(const XrXirTypes *types, XrXirType type);
XR_FUNC bool xr_xir_type_is_owned(const XrXirTypes *types, XrXirType type);
XR_FUNC XrXirType xr_xir_cell_element(const XrXirTypes *types, XrXirType type);
XR_FUNC XrXirType xr_xir_array_element(const XrXirTypes *types, XrXirType type);
XR_FUNC uint32_t xr_xir_type_span(const XrXirTypes *types, XrXirType type);
XR_FUNC XrXirStatus xr_xir_type_sendable(const XrXirTypes *types, XrXirType type,
    const uint32_t *constraints, uint32_t parameter_count, uint64_t *work);
XR_FUNC XrXirStatus xr_xir_types_verify(const XrXirTypes *types, XrXirBudget *remaining);
/* Snapshot cloning requires successful descriptor-pool verification first. */
XR_FUNC XrXirStatus xr_xir_types_clone(const XrXirTypes *types, XrXirTypes **output);
XR_FUNC void xr_xir_types_free(XrXirTypes *types);
XR_FUNC XrXirType xr_xir_operand_type(const XrXirFunction *function, uint32_t value);
#endif // XXIR_TYPES_H
