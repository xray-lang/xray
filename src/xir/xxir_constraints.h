/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_constraints.h - Independent ownership of verified constraint records
 */
#ifndef XXIR_CONSTRAINTS_H
#define XXIR_CONSTRAINTS_H
#include "xxir_interface.h"

/* Copies preserve declaration and type identities within the same verified pool.
 * These helpers allocate ownership; they confer no semantic proof or authority. */
XR_FUNC XrXirStatus xr_xir_compile_interface_applications_copy_verified(const XrXirCompileContext *compile_context, const XrXirInterfaceApplication *source, uint32_t count, XrXirInterfaceApplication **output);
XR_FUNC void xr_xir_compile_interface_applications_free(XrXirInterfaceApplication *applications, uint32_t count);
XR_FUNC XrXirStatus xr_xir_compile_constraint_array_copy_verified(const XrXirCompileContext *compile_context, const XrXirConstraint *source, uint32_t count, XrXirConstraint **output);
XR_FUNC void xr_xir_compile_constraint_array_free(XrXirConstraint *constraints, uint32_t count);
/* Requires verified pools sharing nominal declaration identities. Ordered record
 * equality preserves canonical interface identity and declaration-local parameter
 * ordinals while matching argument structure across those type pools. */
XR_FUNC XrXirStatus xr_xir_compile_constraint_records_match(const XrXirCompileContext *compile_context, const XrXirTypes *from_types, XrXirConstraint from, const XrXirTypes *to_types, XrXirConstraint to, uint32_t parameter_count);
#endif // XXIR_CONSTRAINTS_H
