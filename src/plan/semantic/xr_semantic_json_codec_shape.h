/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_json_codec_shape.h - Exact JSON namespace codec result shapes
 */
#ifndef XR_SEMANTIC_JSON_CODEC_SHAPE_H
#define XR_SEMANTIC_JSON_CODEC_SHAPE_H

#include "xr_semantic_string_shape.h"
#include <string.h>

static inline bool
xr_semantic_json_codec_result_is_exact(const char *selector, const XrSemanticTypeRecord *type) {
    if (!selector || !type)
        return false;
    if (strcmp(selector, "stringify") == 0)
        return xr_semantic_tagged_string_type_is_exact(type);
    return strcmp(selector, "value") == 0 && type->kind == XR_KIND_JSON &&
           type->builtin_type == XR_TID_NULL && type->child_count == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE;
}

#endif // XR_SEMANTIC_JSON_CODEC_SHAPE_H
