/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_defaults_internal.h - Declaration-owned parameter default authority
 *
 * KEY CONCEPT:
 *   Only an authenticated owner and ordinal authorize a private default helper.
 */
#ifndef XXIR_DEFAULTS_INTERNAL_H
#define XXIR_DEFAULTS_INTERNAL_H
#include "xxir.h"
/* These are declaration metadata, never SSA operands or alternate authority. */
static inline const uint32_t *xr_xir_default_identity(const XrXirInstruction *op) {
    return op->op == XR_XIR_INVOKE_DEFAULT ? op->args : op->targets;
}
/* Descriptor, generic and declaration structure must already be admitted. */
XR_FUNC XrXirStatus xr_xir_compile_default_helper(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t function, bool *bound);
XR_FUNC XrXirStatus xr_xir_compile_default_call_verify(const XrXirCompileContext *compile_context, const XrXirModule *module, uint32_t caller, const XrXirInstruction *instruction);
/* Exact retained Source/default correspondence only; not final provenance admission. */
XR_FUNC XrXirStatus xir_compile_default_call_origin(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t caller,uint32_t instruction,uint32_t target,bool *authorized);
#endif // XXIR_DEFAULTS_INTERNAL_H
