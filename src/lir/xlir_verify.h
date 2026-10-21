/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_verify.h - Structural and SSA validation at explicit trust boundaries
 */
#ifndef XLIR_VERIFY_H
#define XLIR_VERIFY_H
#include "xlir.h"

/* Diagnostics borrow only the module's owned source paths. Invalid graphs
 * report a structural failure; allocation failure remains distinguishable. */
XR_FUNC XrLirStatus xr_lir_verify(const XrLirModule *, XrLirDiagnostic *);

#endif  // XLIR_VERIFY_H
