/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_sccp.h - Owned I64/I1 conditional constant propagation
 */
#ifndef XLIR_SCCP_H
#define XLIR_SCCP_H
#include "xlir.h"

/* Verify a readable input once and rebuild its entire module independently.
 * I64/I1 constants can fold and nonexecutable blocks can disappear. Other valid
 * operations are copied conservatively; reachable calls retain their order.
 * Function ordinals/signatures and the full intrinsic registry are preserved;
 * block/value IDs may change. No root is selected or product pipeline enabled.
 * Output must be empty. Failure preserves input and any occupied output. */
XR_FUNC XrLirStatus xr_lir_sccp(const XrLirModule *input, XrLirModule **output);
#endif // XLIR_SCCP_H
