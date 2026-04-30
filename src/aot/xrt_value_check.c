/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_value_check.c -- compile-time guard verifying that the AOT
 * standalone XrValue layout (xrt_value.h) matches the VM runtime
 * XrValue layout (xvalue.h).
 *
 * Both define their own XrValue struct + tag constants because AOT
 * must be self-contained (no VM runtime dependency). This file
 * links into xray_core to catch layout drift at compile time.
 *
 * Note: tags 0-7 are shared. Tags >= 8 are AOT-specific extensions
 * (XR_TAG_STR, XR_TAG_ARRAY, etc.) and have no VM counterpart.
 */

/* Include only the tag constants header, not the full xrt_value.h
 * (which would redefine the XrValue struct). */
#include "../runtime/value/xvalue.h"
#include <stddef.h>

/* Verify struct size */
_Static_assert(sizeof(XrValue) == 16, "XrValue must be 16 bytes");

/* Verify field offsets match documented layout */
_Static_assert(offsetof(XrValue, tag) == 0, "XrValue.tag offset");
_Static_assert(offsetof(XrValue, flags) == 1, "XrValue.flags offset");
_Static_assert(offsetof(XrValue, heap_type) == 2, "XrValue.heap_type offset");
_Static_assert(offsetof(XrValue, ext) == 4, "XrValue.ext offset");
_Static_assert(offsetof(XrValue, i) == 8, "XrValue.i offset");
_Static_assert(offsetof(XrValue, f) == 8, "XrValue.f offset");
_Static_assert(offsetof(XrValue, ptr) == 8, "XrValue.ptr offset");

/* Verify base tag values match AOT constants (xrt_value.h #defines):
 *   XR_TAG_NULL=0, BOOL=1, I64=3, F64=4, PTR=5, STRUCT_REF=6, NOTFOUND=7 */
_Static_assert(XR_TAG_NULL == 0, "XR_TAG_NULL drift");
_Static_assert(XR_TAG_BOOL == 1, "XR_TAG_BOOL drift");
_Static_assert(XR_TAG_I64 == 3, "XR_TAG_I64 drift");
_Static_assert(XR_TAG_F64 == 4, "XR_TAG_F64 drift");
_Static_assert(XR_TAG_PTR == 5, "XR_TAG_PTR drift");
_Static_assert(XR_TAG_STRUCT_REF == 6, "XR_TAG_STRUCT_REF drift");
_Static_assert(XR_TAG_NOTFOUND == 7, "XR_TAG_NOTFOUND drift");
