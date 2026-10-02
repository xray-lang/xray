/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_scope.c - Metered parser-owned type aliases with typed duplicate status
 */
#include "xtype_scope.h"

struct XrTypeScope {
    XrCompileState *state;
    XrTypeScope *parent;
    XrTypeAlias *aliases;
};

static XrTypeScopeStatus type_scope_status(XrCompileResourceStatus status) {
    switch (status) {
        case XR_COMPILE_RESOURCE_OK: return XR_TYPE_SCOPE_OK;
        case XR_COMPILE_RESOURCE_BAD_ARGUMENT: return XR_TYPE_SCOPE_BAD_ARGUMENT;
        case XR_COMPILE_RESOURCE_BUDGET: return XR_TYPE_SCOPE_BUDGET;
        case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_TYPE_SCOPE_OUT_OF_MEMORY;
    }
    return XR_TYPE_SCOPE_BAD_ARGUMENT;
}

static XrTypeAlias *find_local(XrTypeScope *scope, const char *name) {
    if (!scope || xr_compile_state_status(scope->state) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!name) {
        xr_compile_state_fail(scope->state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return NULL;
    }
    for (XrTypeAlias *alias = scope->aliases; alias; alias = alias->next) {
        if (xr_compile_state_work(scope->state, 1) != XR_COMPILE_RESOURCE_OK) return NULL;
        size_t i = 0;
        for (;;) {
            if (xr_compile_state_work(scope->state, 2) != XR_COMPILE_RESOURCE_OK) return NULL;
            char left = alias->name[i], right = name[i];
            if (left != right) break;
            if (!right) return alias;
            if (i == SIZE_MAX - 1) {
                xr_compile_state_fail(scope->state, XR_COMPILE_RESOURCE_BUDGET);
                return NULL;
            }
            ++i;
        }
    }
    return NULL;
}

XR_FUNC XrCompileResourceStatus xr_compile_type_scope_open(XrCompileState *state, XrTypeScope *parent, XrTypeScope **output) {
    if (!output || *output || (parent && parent->state != state))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    void *memory = NULL;
    XrCompileResourceStatus status = xr_compile_state_alloc(state, sizeof(XrTypeScope), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = xr_compile_state_retain(state);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(memory);
        return status;
    }
    XrTypeScope *scope = memory;
    *scope = (XrTypeScope) {state, parent, NULL};
    *output = scope;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC void xr_type_scope_free(XrTypeScope *scope) {
    if (!scope) return;
    XrCompileState *state = scope->state;
    XrTypeAlias *alias = scope->aliases;
    while (alias) {
        XrTypeAlias *next = alias->next;
        xr_compile_state_free((void *) alias->name);
        xr_compile_state_free(alias);
        alias = next;
    }
    xr_compile_state_free(scope);
    xr_compile_state_release(state);
}

XR_FUNC XrTypeScopeStatus xr_compile_type_scope_define(XrTypeScope *scope, const char *name,
    XrTypeRef *type_ref, XrTypeAlias **output) {
    if (!scope) return XR_TYPE_SCOPE_BAD_ARGUMENT;
    if (!name || !output || *output)
        return type_scope_status(xr_compile_state_fail(scope->state, XR_COMPILE_RESOURCE_BAD_ARGUMENT));
    if (find_local(scope, name)) return XR_TYPE_SCOPE_DUPLICATE;
    XrCompileResourceStatus status = xr_compile_state_status(scope->state);
    if (status != XR_COMPILE_RESOURCE_OK) return type_scope_status(status);
    void *memory = NULL;
    status = xr_compile_state_calloc(scope->state, 1, sizeof(XrTypeAlias), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return type_scope_status(status);
    char *copy = NULL;
    status = xr_compile_state_strdup(scope->state, name, &copy);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(memory);
        return type_scope_status(status);
    }
    XrTypeAlias *alias = memory;
    alias->name = copy;
    alias->type_ref = type_ref;
    alias->next = scope->aliases;
    scope->aliases = alias;
    *output = alias;
    return XR_TYPE_SCOPE_OK;
}

XR_FUNC XrTypeAlias *xr_type_scope_lookup(XrTypeScope *scope, const char *name) {
    for (XrTypeScope *current = scope; current; current = current->parent) {
        if (xr_compile_state_work(current->state, 1) != XR_COMPILE_RESOURCE_OK) return NULL;
        XrTypeAlias *alias = find_local(current, name);
        if (alias || xr_compile_state_status(current->state) != XR_COMPILE_RESOURCE_OK) return alias;
    }
    return NULL;
}

XR_FUNC XrTypeAlias *xr_type_scope_lookup_local(XrTypeScope *scope, const char *name) {
    return find_local(scope, name);
}

XR_FUNC XrTypeRef *xr_type_scope_resolve(XrTypeScope *scope, const char *name) {
    XrTypeAlias *alias = xr_type_scope_lookup(scope, name);
    return alias ? alias->type_ref : NULL;
}
