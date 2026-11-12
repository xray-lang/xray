/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_display.h - Exact inline value display through the owned type pool
 */
#ifndef XLIR_DISPLAY_H
#define XLIR_DISPLAY_H
#include "xlir_builder.h"
#include "../../include/xray_native_pod.h"

enum { XR_LIR_INTRINSIC_DISPLAY = 256 };
/* One library role receives one actual SSA value. Its complete module-local
 * type selects the catalog entry, including every Nullable layer. The empty
 * declaration signature never authorizes an untyped external call. */
XR_FUNC bool xr_lir_display_type(const XrLirModule *, XrLirType);
XR_FUNC bool xr_lir_display_intrinsic_valid(const XrLirIntrinsic *);
XR_FUNC bool xr_lir_display_point(const XrLirModule *, const XrLirInstructionView *);
XR_FUNC XrLirStatus xr_lir_display_operation(const XrLirModule *, uint32_t *);
XR_FUNC XrLirStatus xr_lir_display_catalog_install(XrLirModule *, bool views, bool arrays);
XR_FUNC XrLirStatus xr_lir_pod_type(const XrLirModule *, XrLirType, XrPodType *);
#endif // XLIR_DISPLAY_H
