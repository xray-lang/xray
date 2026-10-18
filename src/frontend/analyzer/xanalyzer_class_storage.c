/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_class_storage.c - Retained class identity and transactional members
 */
#include "xanalyzer_symbol.h"
#include "xanalyzer_vtable.h"
#include "../../base/xcompile_state.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xstruct_layout.h"
#include <limits.h>
#include <stddef.h>

typedef struct ClassOwner {
    XrCompileState *state;
    XrClassInfo info;
} ClassOwner;

static ClassOwner *class_owner(XrClassInfo *info) {
    return (ClassOwner *) ((char *) info - offsetof(ClassOwner, info));
}

XR_FUNC XrClassInfo *xa_class_info_new(XrCompileState *state, const char *name) {
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return NULL;
    size_t length = 0;
    if (xr_compile_state_string_length(state, name, &length) != XR_COMPILE_RESOURCE_OK)
        return NULL;
    if (!length) {
        (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return NULL;
    }
    void *memory = NULL;
    if (xr_compile_state_calloc(state, 1, sizeof(ClassOwner), &memory) != XR_COMPILE_RESOURCE_OK)
        return NULL;
    ClassOwner *owner = memory;
    char *copy = NULL;
    if (xr_compile_state_strndup(state, name, length, &copy) != XR_COMPILE_RESOURCE_OK ||
        xr_compile_state_retain(state) != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(copy);
        xr_compile_state_free(owner);
        return NULL;
    }
    owner->state = state;
    owner->info.name = copy;
    return &owner->info;
}

XR_FUNC void xa_class_info_free(XrClassInfo *info) {
    if (!info) return;
    ClassOwner *owner = class_owner(info);
    XrCompileState *state = owner->state;
    xr_compile_state_free((void *) info->name);
    xr_compile_state_free(info->fields);
    xr_compile_state_free(info->methods);
    xr_compile_state_free(info->static_fields);
    xr_compile_state_free(info->static_methods);
    /* These declaration-specific tables retain their existing allocators. */
    xr_free((void *) info->base_name);
    xr_free(info->constructor_params);
    xr_free(info->interface_types);
    if (info->interface_conformances) {
        for (int i = 0; i < info->interface_conformance_count; ++i)
            xr_free(info->interface_conformances[i].witnesses);
        xr_free(info->interface_conformances);
    }
    xa_class_vtable_free_owned(info);
    if (info->struct_layout) xr_aggregate_layout_free_owned(info->struct_layout);
    xr_compile_state_free(owner);
    xr_compile_state_release(state);
}

static XrCompileResourceStatus append_member(XrCompileState *state,
    XaSymbol ***list, int *count, XaSymbol *item) {
    int n = *count;
    if (n < 0 || (n && !*list) || (!n && *list) || !item->name)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    if (n == INT_MAX)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
    XaSymbol **staged = *list;
    bool grow = !n || (n >= 4 && (n & (n - 1)) == 0);
    if (grow) {
        size_t capacity = !n ? 4 : (size_t) n * 2;
        if (capacity > INT_MAX || capacity > SIZE_MAX / sizeof(*staged))
            return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
        void *memory = NULL;
        XrCompileResourceStatus status = xr_compile_state_alloc(state,
            capacity * sizeof(*staged), &memory);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
        staged = memory;
        status = xr_compile_state_copy(state, staged, *list, (size_t) n * sizeof(*staged));
        if (status != XR_COMPILE_RESOURCE_OK) {
            xr_compile_state_free(staged);
            return status;
        }
    }
    XrCompileResourceStatus status = xr_compile_state_work(state, 2);
    if (status != XR_COMPILE_RESOURCE_OK) {
        if (grow) xr_compile_state_free(staged);
        return status;
    }
    /* No fallible operation may follow the first published member write. */
    staged[n] = item;
    XaSymbol **previous = *list;
    *list = staged;
    *count = n + 1;
    if (grow) xr_compile_state_free(previous);
    return XR_COMPILE_RESOURCE_OK;
}

static XrCompileResourceStatus add_member(XrCompileState *request, XrClassInfo *info,
    XaSymbol *member, bool method) {
    XrCompileResourceStatus status = xr_compile_state_status(request);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    if (!info) return xr_compile_state_fail(request, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XrCompileState *state = class_owner(info)->state;
    if (xr_compile_state_resources(state) != xr_compile_state_resources(request))
        return xr_compile_state_fail(request, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    status = xr_compile_state_status(state);
    if (status == XR_COMPILE_RESOURCE_OK) {
        if (!member) status = xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        else if (method) status = member->is_static ?
            append_member(state, &info->static_methods, &info->static_method_count, member) :
            append_member(state, &info->methods, &info->method_count, member);
        else status = member->is_static ?
            append_member(state, &info->static_fields, &info->static_field_count, member) :
            append_member(state, &info->fields, &info->field_count, member);
    }
    return xr_compile_state_fail(request, status);
}

XR_FUNC XrCompileResourceStatus xa_class_info_add_field(XrCompileState *request,
    XrClassInfo *info, XaSymbol *field) {
    return add_member(request, info, field, false);
}

XR_FUNC XrCompileResourceStatus xa_class_info_add_method(XrCompileState *request,
    XrClassInfo *info, XaSymbol *method) {
    return add_member(request, info, method, true);
}
