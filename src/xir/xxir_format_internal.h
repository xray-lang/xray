/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_format_internal.h - Borrowed display access to canonical value storage
 */
#ifndef XXIR_FORMAT_INTERNAL_H
#define XXIR_FORMAT_INTERNAL_H
#include "xxir_format.h"

/* A zero node type borrows an actual value; a nonzero type borrows packed
 * storage in the root arena. Scalar leaves are copied without acquiring RC. */
XR_FUNC bool xr_xir_value_format_storage(const void *arena, XrValueFormatNode node,
    XrXirValue *leaf, XrValueFormatView *view);
XR_FUNC int xr_xir_value_format_child(const void *arena, XrValueFormatNode node,
    uint32_t index, XrValueFormatNode *child);
#endif // XXIR_FORMAT_INTERNAL_H
