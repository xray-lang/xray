/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_compat.h - VM-native type aliases for AOT/VM interop
 *
 * KEY CONCEPT:
 *   XrtValue and XrValue are the same struct (same ABI).
 *   These aliases let AOT code and VM code share pointers without casting.
 */

#ifndef XRT_COMPAT_H
#define XRT_COMPAT_H

#include "xrt_value.h"

/* =========================================================================
 * VM-native type aliases
 * ========================================================================= */

typedef XrtValue XrValue;

// VM-native tag constants (must stay in sync with xvalue.h)
#define XR_TAG_NULL XRT_TAG_NULL
#define XR_TAG_BOOL XRT_TAG_BOOL
#define XR_TAG_I64 XRT_TAG_I64
#define XR_TAG_F64 XRT_TAG_F64
#define XR_TAG_PTR XRT_TAG_PTR

// VM-native type checks
#define XR_IS_NULL(v) ((v).tag == XR_TAG_NULL)
#define XR_IS_INT(v) ((v).tag == XR_TAG_I64)
#define XR_IS_FLOAT(v) ((v).tag == XR_TAG_F64)
#define XR_IS_NUM(v) (XR_IS_INT(v) || XR_IS_FLOAT(v))

// VM-native value creation macros
#define XR_FROM_INT(x) ((XrValue){.i = (int64_t) (x), .tag = XR_TAG_I64})
#define XR_FROM_FLOAT(x) ((XrValue){.f = (double) (x), .tag = XR_TAG_F64})
#define XR_FROM_BOOL(x) ((XrValue){.i = (x) ? 1 : 0, .tag = XR_TAG_BOOL})
#define XR_NULL_VAL ((XrValue){.ptr = 0, .tag = XR_TAG_NULL})
#define XR_TRUE_VAL ((XrValue){.i = 1, .tag = XR_TAG_BOOL})
#define XR_FALSE_VAL ((XrValue){.i = 0, .tag = XR_TAG_BOOL})

// VM-native value extraction
#define XR_TO_INT(v) ((v).i)
#define XR_TO_FLOAT(v) ((v).f)

/* =========================================================================
 * Runtime context — opaque handle passed to all AOT functions.
 * Points to XrCoroutine* internally; AOT code never dereferences it directly.
 * ========================================================================= */

typedef void *XrtContext;

#endif  // XRT_COMPAT_H
