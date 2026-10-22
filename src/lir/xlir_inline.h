/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_inline.h - Bounded direct calls rebuilt as independently owned SSA
 */
#ifndef XLIR_INLINE_H
#define XLIR_INLINE_H
#include "xlir.h"

/* Verify a readable input once and rebuild the entire module independently.
 * Original direct calls to small, nonrecursive definitions with a structural
 * return can be inlined. Growth limits skip optimization, never admission;
 * calls copied from a callee are not reconsidered. Functions and the complete
 * registry retain their ordinals. Block/value IDs may change. Output must be
 * empty; failure publishes nothing and preserves input and occupied output.
 * This does not enable a product pipeline or expand backend admission. */
XR_FUNC XrLirStatus xr_lir_inline(const XrLirModule *input, XrLirModule **output);
#endif // XLIR_INLINE_H
