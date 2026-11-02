/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_view_internal.h - Actual class identity behind a logical ancestor view
 *
 * KEY CONCEPT:
 *   A view narrows field access; the object retains its complete allocation type.
 */
#ifndef XXIR_CLASS_VIEW_INTERNAL_H
#define XXIR_CLASS_VIEW_INTERNAL_H
#include "xxir_value.h"
XR_FUNC XrXirValueStatus xir_class_actual_type(const XrXirValue *value,
    const XrXirTypeArena *arena,XrXirType *output);
#endif // XXIR_CLASS_VIEW_INTERNAL_H
