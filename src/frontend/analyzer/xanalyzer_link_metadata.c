/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_link_metadata.c - Atomic original-state auxiliary metadata storage
 */
#include "xanalyzer_link_metadata.h"
#include "../parser/xast_nodes_common.h"
#include "../../runtime/value/xtype_internal.h"
#include "../../runtime/value/xtype_pool.h"
#include <limits.h>

XR_FUNC XrCompileResourceStatus xa_type_set_view_contract(XrCompileState *state,
    XrVMRuntime *isolate, XrType *type, const XrViewOrigin *origins, int count, bool was_elided) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XrTypePool *pool = xr_type_get_current_pool();
    if (!pool || pool->state != state)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XrViewOriginBindStatus bound =
        xr_type_function_set_view_origins(isolate, type, origins, count, was_elided);
    status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (bound == XR_VIEW_ORIGIN_BIND_OK) return XR_COMPILE_RESOURCE_OK;
    return xr_compile_state_fail(state, bound == XR_VIEW_ORIGIN_BIND_OUT_OF_MEMORY
        ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BAD_ARGUMENT);
}

XR_FUNC XrCompileResourceStatus xa_symbol_links_set_view_contract(
    XaSymbolLinks *links, const XaViewReturnContract *contract) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XaViewReturnContract staged = contract ? *contract : (XaViewReturnContract) {0};
    if (staged.origin_count < 0 || (staged.origin_count && !staged.origins))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    void *memory = NULL;
    if (staged.origin_count) {
        status = xr_compile_state_calloc(state, (size_t) staged.origin_count,
                                         sizeof(XrViewOrigin), &memory);
        if (status == XR_COMPILE_RESOURCE_OK)
            status = xr_compile_state_copy(state, memory, staged.origins,
                (size_t) staged.origin_count * sizeof(XrViewOrigin));
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(memory);
        return status;
    }
    staged.origins = memory;
    xr_compile_state_free(links->return_view.origins);
    links->return_view = staged;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xa_symbol_links_ensure_inferred_params(XaSymbolLinks *links) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    int count = links->param_count;
    if (count < 0 || (links->inferred_param_types &&
            (count == 0 || links->inferred_param_count != count)) ||
        (!links->inferred_param_types && links->inferred_param_count != 0))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    if (links->inferred_param_types || !count) return XR_COMPILE_RESOURCE_OK;
    void *memory = NULL;
    status = xr_compile_state_calloc(state, (size_t) count, sizeof(XrType *), &memory);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(memory);
        return status;
    }
    links->inferred_param_types = memory;
    links->inferred_param_count = count;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xa_symbol_links_set_deprecated(
    XaSymbolLinks *links, bool present, const char *message) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    char *copy = NULL;
    if (message) status = xr_compile_state_strdup(state, message, &copy);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(copy);
        return status;
    }
    xr_compile_state_free((void *) links->deprecated_message);
    links->deprecated_message = copy;
    links->is_deprecated = present;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xa_symbol_links_set_deprecated_attrs(
    XaSymbolLinks *links, XrAttribute **attributes, int count) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (count < 0 || (count && !attributes))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    for (int i = 0; i < count; ++i) {
        status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
        if (attributes[i] && attributes[i]->kind == ATTR_DEPRECATED)
            return xa_symbol_links_set_deprecated(links, true, attributes[i]->str_arg);
    }
    return xa_symbol_links_set_deprecated(links, false, NULL);
}

XR_FUNC XrCompileResourceStatus xa_symbol_add_ref(XaSymbolLinks *links, uint32_t line,
    uint32_t column, uint32_t end_column, bool is_write) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (links->ref_count < 0 || links->ref_count == INT_MAX)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    void *memory = NULL;
    status = xr_compile_state_calloc(state, 1, sizeof(XaRefLocation), &memory);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(memory);
        return status;
    }
    XaRefLocation *reference = memory;
    reference->line = line;
    reference->column = column;
    reference->end_column = end_column;
    reference->is_write = is_write;
    reference->next = links->references;
    links->references = reference;
    ++links->ref_count;
    return XR_COMPILE_RESOURCE_OK;
}

// Get references from symbol links
XR_FUNC XaRefLocation *xa_symbol_get_refs(XaSymbolLinks *links, int *count) {
    if (!links) {
        if (count)
            *count = 0;
        return NULL;
    }
    if (count)
        *count = links->ref_count;
    return links->references;
}
