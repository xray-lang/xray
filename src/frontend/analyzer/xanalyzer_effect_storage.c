/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_effect_storage.c - Atomic effect summary storage replacement
 */
#include "xanalyzer_effect_storage.h"

static XrCompileResourceStatus xa_effect_array_copy(XrCompileState *state,
    const XaParamEffectSummary *input, int count, XaParamEffectSummary **output) {
    if (count < 0 || (count && !input))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    void *memory = NULL;
    XrCompileResourceStatus status = XR_COMPILE_RESOURCE_OK;
    if (count) {
        status = xr_compile_state_calloc(state, (size_t) count, sizeof(*input), &memory);
        if (status == XR_COMPILE_RESOURCE_OK)
            status = xr_compile_state_copy(state, memory, input, (size_t) count * sizeof(*input));
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(memory);
        return status;
    }
    *output = memory;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xa_symbol_links_set_param_effect_summaries(
    XaSymbolLinks *links, const XaParamEffectSummary *effects, int count) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XaParamEffectSummary *copy = NULL;
    status = xa_effect_array_copy(state, effects, count, &copy);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    xr_compile_state_free(links->param_effects);
    links->param_effects = copy;
    links->param_effect_count = count;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xa_symbol_links_copy_param_effect_summaries(
    XaSymbolLinks *dst, const XaSymbolLinks *src) {
    XrCompileResourceStatus status = xr_compile_state_status(dst ? dst->resource_state : NULL);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    return xa_symbol_links_set_param_effect_summaries(dst, src ? src->param_effects : NULL,
                                                       src ? src->param_effect_count : 0);
}

static XrCompileResourceStatus xa_effect_replace_return(XaSymbolLinks *dst,
    const XaParamEffectSummary *effects, int count) {
    XaParamEffectSummary *copy = NULL;
    XrCompileResourceStatus status = xa_effect_array_copy(dst->resource_state, effects, count, &copy);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    xr_compile_state_free(dst->return_fn_param_effects);
    dst->return_fn_param_effects = copy;
    dst->return_fn_param_effect_count = count;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xa_symbol_links_set_return_function_effect_summary(
    XaSymbolLinks *dst, const XaSymbolLinks *src) {
    XrCompileResourceStatus status = xr_compile_state_status(dst ? dst->resource_state : NULL);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = xa_effect_replace_return(dst, src ? src->param_effects : NULL,
                                         src ? src->param_effect_count : 0);
    if (status == XR_COMPILE_RESOURCE_OK) dst->return_fn_effect_mixed = false;
    return status;
}

XR_FUNC XrCompileResourceStatus xa_symbol_links_copy_return_function_effect_summary(
    XaSymbolLinks *dst, const XaSymbolLinks *src) {
    XrCompileResourceStatus status = xr_compile_state_status(dst ? dst->resource_state : NULL);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    bool mixed = src ? src->return_fn_effect_mixed : false;
    bool scanned = src ? src->return_fn_effect_scanned : false;
    status = xa_effect_replace_return(dst, src ? src->return_fn_param_effects : NULL,
                                         src ? src->return_fn_param_effect_count : 0);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    dst->return_fn_effect_mixed = mixed;
    dst->return_fn_effect_scanned = scanned;
    dst->return_fn_effect_scan_in_progress = false;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xa_symbol_links_copy_return_function_effect_to_param_summaries(
    XaSymbolLinks *dst, const XaSymbolLinks *src) {
    XrCompileResourceStatus status = xr_compile_state_status(dst ? dst->resource_state : NULL);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    return xa_symbol_links_set_param_effect_summaries(dst,
        src ? src->return_fn_param_effects : NULL, src ? src->return_fn_param_effect_count : 0);
}
