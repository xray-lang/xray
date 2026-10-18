/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_signature.c - Atomic original-state signature arrays and strings
 */
#include "xanalyzer_signature.h"
#include "../parser/xast_nodes.h"
#include "../../base/xmalloc.h"

typedef struct XaFunctionArrays {
    XrType **types;
    const char **names;
    int count;
} XaFunctionArrays;
typedef struct XaGenericArrays {
    const char **names;
    XrType ***constraints;
    int *counts;
    int count;
} XaGenericArrays;
static void xa_signature_free_names(const char **names, int count) {
    if (!names) return;
    for (int i = 0; i < count; ++i) xr_compile_state_free((void *) names[i]);
    xr_compile_state_free(names);
}
static void xa_signature_free_function_arrays(XaFunctionArrays arrays) {
    xr_compile_state_free(arrays.types);
    xa_signature_free_names(arrays.names, arrays.count);
}
static void xa_signature_free_generic_arrays(XaGenericArrays arrays) {
    xa_signature_free_names(arrays.names, arrays.count);
    if (arrays.constraints) {
        for (int i = 0; i < arrays.count; ++i) xr_compile_state_free(arrays.constraints[i]);
        xr_compile_state_free(arrays.constraints);
    }
    xr_compile_state_free(arrays.counts);
}
XR_FUNC void xa_symbol_links_clear_return_function_effect_summary(XaSymbolLinks *links) {
    if (!links) return;
    xr_compile_state_free(links->return_fn_param_effects);
    links->return_fn_param_effects = NULL;
    links->return_fn_param_effect_count = 0;
    links->return_fn_effect_mixed = false;
}
XR_FUNC void xa_symbol_links_clear_function_sig(XaSymbolLinks *links) {
    if (!links) return;
    xa_signature_free_function_arrays((XaFunctionArrays) {links->param_types, links->param_names, links->param_count});
    links->param_types = NULL; links->param_names = NULL; links->param_count = 0;
    links->return_type = NULL;
    xr_compile_state_free(links->param_defaults); links->param_defaults = NULL;
    /* A new signature invalidates observations and contracts derived from
     * its previous parameter rows, even when the same Links value is reused. */
    xr_compile_state_free(links->inferred_param_types); links->inferred_param_types = NULL;
    links->inferred_param_count = 0;
    xr_compile_state_free(links->param_effects); links->param_effects = NULL; links->param_effect_count = 0;
    xr_compile_state_free(links->return_view.origins); links->return_view.origins = NULL;
    links->return_view.origin_count = 0; links->return_view.was_elided = false;
    links->return_view.checked = false; links->return_view.valid = false;
    xa_symbol_links_clear_return_function_effect_summary(links);
    links->return_fn_effect_scanned = false; links->return_fn_effect_scan_in_progress = false;
}
XR_FUNC void xa_symbol_links_clear_type_params(XaSymbolLinks *links) {
    if (!links) return;
    xa_signature_free_generic_arrays((XaGenericArrays) {links->type_param_names, links->type_param_constraints,
                                       links->type_param_constraint_counts, links->type_param_count});
    links->type_param_names = NULL; links->type_param_constraints = NULL;
    links->type_param_constraint_counts = NULL; links->type_param_count = 0;
}
static XrCompileResourceStatus xa_signature_copy_names(XrCompileState *state, const char **input,
                                         int count, const char ***output) {
    void *memory = NULL;
    XrCompileResourceStatus status = xr_compile_state_calloc(state, (size_t) count,
                                                           sizeof(const char *), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    const char **copy = memory;
    for (int i = 0; i < count; ++i) {
        status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK) break;
        if (input[i]) {
            char *name = NULL;
            status = xr_compile_state_strdup(state, input[i], &name);
            if (status != XR_COMPILE_RESOURCE_OK) break;
            copy[i] = name;
        }
    }
    if (status != XR_COMPILE_RESOURCE_OK) { xa_signature_free_names(copy, count); return status; }
    *output = copy;
    return XR_COMPILE_RESOURCE_OK;
}
XR_FUNC XrCompileResourceStatus xa_symbol_links_set_function_sig(XaSymbolLinks *links,
    XrType **param_types, const char **param_names, int param_count, XrType *return_type) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (param_count < 0) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XaFunctionArrays staged = {.count = param_count};
    if (param_count && param_types) {
        void *memory = NULL;
        status = xr_compile_state_calloc(state, (size_t) param_count, sizeof(XrType *), &memory);
        if (status == XR_COMPILE_RESOURCE_OK) {
            staged.types = memory;
            status = xr_compile_state_copy(state, memory, param_types, (size_t) param_count * sizeof(XrType *));
        }
    }
    if (status == XR_COMPILE_RESOURCE_OK && param_count && param_names)
        status = xa_signature_copy_names(state, param_names, param_count, &staged.names);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) { xa_signature_free_function_arrays(staged); return status; }
    /* Stage before clearing so inputs may alias the previous signature. */
    xa_symbol_links_clear_function_sig(links);
    links->param_types = staged.types; links->param_names = staged.names;
    links->param_count = param_count; links->return_type = return_type;
    return XR_COMPILE_RESOURCE_OK;
}
XR_FUNC XrCompileResourceStatus xa_symbol_links_set_param_defaults(XaSymbolLinks *links,
    struct AstNode **defaults, int count) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (count < 0 || (defaults && count && count != links->param_count))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    bool any = false;
    for (int i = 0; defaults && i < count; ++i) {
        status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
        if (defaults[i]) any = true;
    }
    void *memory = NULL;
    if (any) {
        status = xr_compile_state_calloc(state, (size_t) count, sizeof(struct AstNode *), &memory);
        if (status == XR_COMPILE_RESOURCE_OK)
            status = xr_compile_state_copy(state, memory, defaults, (size_t) count * sizeof(struct AstNode *));
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) { xr_compile_state_free(memory); return status; }
    xr_compile_state_free(links->param_defaults); links->param_defaults = memory;
    return XR_COMPILE_RESOURCE_OK;
}
XR_FUNC XrCompileResourceStatus xa_symbol_links_set_type_params(XaSymbolLinks *links,
    const char **names, XrType ***constraint_lists, const int *constraint_counts, int count) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (count < 0 || (count && !names)) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XaGenericArrays staged = {.count = count};
    if (count) {
        status = xa_signature_copy_names(state, names, count, &staged.names);
        void *memory = NULL;
        if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_calloc(state, (size_t) count, sizeof(XrType **), &memory);
        if (status == XR_COMPILE_RESOURCE_OK) staged.constraints = memory;
        memory = NULL;
        if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_calloc(state, (size_t) count, sizeof(int), &memory);
        if (status == XR_COMPILE_RESOURCE_OK) staged.counts = memory;
    }
    for (int i = 0; i < count && status == XR_COMPILE_RESOURCE_OK; ++i) {
        status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK) break;
        int n = constraint_counts ? constraint_counts[i] : 0;
        if (n < 0 || (n && (!constraint_lists || !constraint_lists[i]))) {
            status = xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT); break;
        }
        if (!n) continue;
        void *memory = NULL;
        status = xr_compile_state_calloc(state, (size_t) n, sizeof(XrType *), &memory);
        if (status != XR_COMPILE_RESOURCE_OK) break;
        staged.constraints[i] = memory;
        status = xr_compile_state_copy(state, memory, constraint_lists[i], (size_t) n * sizeof(XrType *));
        if (status == XR_COMPILE_RESOURCE_OK) staged.counts[i] = n;
    }
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) { xa_signature_free_generic_arrays(staged); return status; }
    xa_symbol_links_clear_type_params(links);
    links->type_param_names = staged.names; links->type_param_constraints = staged.constraints;
    links->type_param_constraint_counts = staged.counts; links->type_param_count = count;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrType *xa_symbol_links_get_return_type(XaSymbolLinks *links) {
    return links ? links->return_type : NULL;
}

XR_FUNC XrType **xa_symbol_links_get_param_types(XaSymbolLinks *links, int *count) {
    if (!links) {
        if (count)
            *count = 0;
        return NULL;
    }
    if (count)
        *count = links->param_count;
    return links->param_types;
}

XR_FUNC const char **xa_symbol_links_get_param_names(XaSymbolLinks *links, int *count) {
    if (!links) {
        if (count)
            *count = 0;
        return NULL;
    }
    if (count)
        *count = links->param_count;
    return links->param_names;
}

XR_FUNC int xa_symbol_links_get_type_param_count(XaSymbolLinks *links) {
    return links ? links->type_param_count : 0;
}

XR_FUNC const char *xa_symbol_links_get_type_param_name(XaSymbolLinks *links, int index) {
    if (!links || index < 0 || index >= links->type_param_count)
        return NULL;
    return links->type_param_names[index];
}

XR_FUNC XrType **xa_symbol_links_get_type_param_constraints(XaSymbolLinks *links, int index,
                                                    int *out_count) {
    if (out_count)
        *out_count = 0;
    if (!links || index < 0 || index >= links->type_param_count)
        return NULL;
    if (out_count)
        *out_count = links->type_param_constraint_counts[index];
    return links->type_param_constraints[index];
}


XR_FUNC XrCompileResourceStatus xa_symbol_links_set_generic_declarations(
    XaSymbolLinks *links, XrGenericParam **parameters, int count) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (count < 0 || (count && !parameters))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    void *memory = NULL;
    if (count) status = xr_compile_state_calloc(state, (size_t) count, sizeof(const char *), &memory);
    const char **names = memory;
    for (int i = 0; i < count && status == XR_COMPILE_RESOURCE_OK; ++i) {
        status = xr_compile_state_work(state, 1);
        if (status == XR_COMPILE_RESOURCE_OK) names[i] = parameters[i] ? parameters[i]->name : NULL;
    }
    if (status == XR_COMPILE_RESOURCE_OK)
        status = xa_symbol_links_set_type_params(links, names, NULL, NULL, count);
    xr_compile_state_free(names);
    return status;
}
