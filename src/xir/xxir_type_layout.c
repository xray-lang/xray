/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_layout.c - One target-bound type layout for compiler and runtime
 *
 * KEY CONCEPT:
 *   Compact storage and compiler frames consume the same physical authority.
 */
#include "xxir_types.h"
#include "../base/xmalloc.h"

XrXirStatus xr_xir_layout(const XrXirTypes *types, XrXirType type, const XrXirTarget *target,
                        XrXirLayoutContext context, XrXirLayout *layout) {
    if (!layout)
        return XR_XIR_BAD_LAYOUT;
    *layout = (XrXirLayout) {0, 0};
    if (!target || target->architecture != XR_XIR_ARCH_X86_64 ||
        target->abi_version != XR_XIR_VALUE_ABI_VERSION ||
        context < XR_XIR_LAYOUT_STORAGE || context > XR_XIR_LAYOUT_FRAME ||
        (type != XR_XIR_UNIT && type != XR_XIR_BOOL && !xr_xir_type_is_number(type) && !xr_xir_type_is_owned(types, type)) ||
        (xr_xir_type_is_nominal(types, type) && context == XR_XIR_LAYOUT_STORAGE) ||
        (type == XR_XIR_UNIT && context == XR_XIR_LAYOUT_PARAMETER) || xr_xir_type_span(types, type))
        return XR_XIR_BAD_LAYOUT;
    if (context == XR_XIR_LAYOUT_PARAMETER || context == XR_XIR_LAYOUT_RESULT ||
        context == XR_XIR_LAYOUT_BOXED)
        *layout = (XrXirLayout) {16, 8};
    else if (type == XR_XIR_UNIT)
        *layout = (XrXirLayout) {0, 1};
    else if (type == XR_XIR_BOOL && context == XR_XIR_LAYOUT_STORAGE)
        *layout = (XrXirLayout) {1, 1};
    else if (context == XR_XIR_LAYOUT_STORAGE && xr_xir_type_is_integer(type))
        *layout = (XrXirLayout) {xr_xir_integer_bits(type) / 8, xr_xir_integer_bits(type) / 8};
    else if (context == XR_XIR_LAYOUT_STORAGE && xr_xir_float_bits(type))
        *layout = (XrXirLayout) {xr_xir_float_bits(type) / 8, xr_xir_float_bits(type) / 8};
    else
        *layout = (XrXirLayout) {8, 8};
    return XR_XIR_OK;
}

#include "xxir_nominal_storage.inc.c"
