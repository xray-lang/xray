/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLIR_RANGE_H
#define XLIR_RANGE_H
#include "xlir_builder.h"
enum { XR_LIR_RANGE_OP_CHECK = 768 };
XR_FUNC bool xr_lir_range_intrinsic_valid(const XrLirIntrinsic *);
XR_FUNC bool xr_lir_range_point(const XrLirModule *, const XrLirInstructionView *);
XR_FUNC XrLirStatus xr_lir_range_operation(const XrLirModule *, uint32_t *);
XR_FUNC XrLirStatus xr_lir_range_catalog_install(XrLirModule *, bool views, bool arrays, bool display, bool strings);
#endif // XLIR_RANGE_H
