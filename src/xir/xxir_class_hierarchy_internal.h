/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_hierarchy_internal.h - Complete owned class ancestry validation
 *
 * KEY CONCEPT:
 *   Parent coordinates belong to one verified pool and grant no call permission.
 */
#ifndef XXIR_CLASS_HIERARCHY_INTERNAL_H
#define XXIR_CLASS_HIERARCHY_INTERNAL_H
#include "xxir_types.h"
XR_FUNC XrXirStatus xir_compile_class_hierarchy_verify(const XrXirCompileContext *context,
    const XrXirTypes *types,XrCompileRequestControl *control);
/* Both descriptors must belong to the same completely verified owning pool. */
XR_FUNC bool xr_xir_class_is_subtype(const XrXirTypes *types,XrXirType actual,XrXirType expected);
XR_FUNC XrXirStatus xir_compile_class_common_base(const XrXirTypes *types,XrXirType left,
    XrXirType right,XrXirType *output,XrCompileRequestControl *control);
#endif // XXIR_CLASS_HIERARCHY_INTERNAL_H
