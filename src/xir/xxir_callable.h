/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_callable.h - Canonical owned callable signature identities
 *
 * KEY CONCEPT:
 *   A callable type records a contract, never a guessed implementation target.
 */
#ifndef XXIR_CALLABLE_H
#define XXIR_CALLABLE_H
#include "xxir.h"
XR_FUNC const XrXirCallableSignature *xr_xir_callable_signature(const XrXirCallableTypes *types, XrXirType type);
XR_FUNC XrXirStatus xr_xir_callable_types_verify(const XrXirCallableTypes *types, XrXirBudget *remaining);
/* Snapshot cloning requires successful signature-table verification first. */
XR_FUNC XrXirStatus xr_xir_callable_types_clone(const XrXirCallableTypes *types, XrXirCallableTypes **output);
XR_FUNC XrXirType xr_xir_operand_type(const XrXirFunction *function, uint32_t value);
XR_FUNC void xr_xir_callable_types_free(XrXirCallableTypes *types);
#endif // XXIR_CALLABLE_H
