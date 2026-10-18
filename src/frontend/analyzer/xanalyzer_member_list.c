/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_member_list.c - Atomic member queries on the original compiler state
 */
#include "xanalyzer_member_list.h"
#include "xanalyzer_builtins.h"
#include <limits.h>

static XrCompileResourceStatus prepare_output(XrCompileState *state, XaMemberList *output) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    if (!output || output->items || output->count || output->owns_symbols)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC void xa_member_list_dispose(XaMemberList *list) {
    if (!list)
        return;
    if (list->owns_symbols) {
        for (int i = 0; i < list->count; ++i)
            xa_symbol_free(list->items[i]);
    }
    xr_compile_state_free(list->items);
    *list = (XaMemberList) {0};
}

XR_FUNC XrCompileResourceStatus xa_member_list_builtins(
    XaSymbolRegistry *registry, const XaBuiltinType *type, XaMemberList *output) {
    XrCompileState *state = xa_symbol_registry_state(registry);
    XrCompileResourceStatus status = prepare_output(state, output);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK || !type)
        return status;
    if (type->member_count < 0 || (type->member_count && !type->members))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    if (!type->member_count)
        return XR_COMPILE_RESOURCE_OK;
    XaMemberList result = {.owns_symbols = true};
    void *memory = NULL;
    status = xr_compile_state_calloc(state, (size_t) type->member_count,
                                   sizeof(*result.items), &memory);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    result.items = memory;
    for (int i = 0; i < type->member_count; ++i) {
        status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK)
            break;
        const XaBuiltinMember *member = &type->members[i];
        if (!member->name)
            continue;
        XaSymbolKind kind = member->is_method ? XA_SYM_METHOD : XA_SYM_FIELD;
        XaSymbol *symbol = xa_symbol_new(registry, member->name, kind);
        if (!symbol) {
            status = xr_compile_state_status(state);
            break;
        }
        symbol->is_builtin = true;
        result.items[result.count++] = symbol;
    }
    if (status != XR_COMPILE_RESOURCE_OK) {
        xa_member_list_dispose(&result);
        return status;
    }
    *output = result;
    return XR_COMPILE_RESOURCE_OK;
}

static XrCompileResourceStatus validate_inheritance(XrCompileState *state,
                                                  const XrClassInfo *info) {
    const XrClassInfo *slow = info, *fast = info;
    while (fast) {
        XrCompileResourceStatus status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK || !fast->base)
            return status;
        status = xr_compile_state_work(state, 3);
        if (status != XR_COMPILE_RESOURCE_OK)
            return status;
        slow = slow->base;
        fast = fast->base->base;
        if (slow == fast)
            return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    }
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xa_member_list_class(
    XaSymbolRegistry *registry, const XrClassInfo *info, XaMemberList *output) {
    XrCompileState *state = xa_symbol_registry_state(registry);
    XrCompileResourceStatus status = prepare_output(state, output);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    status = validate_inheritance(state, info);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    size_t total = 0;
    for (const XrClassInfo *c = info; c; c = c->base) {
        status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK)
            return status;
        if (c->field_count < 0 || c->method_count < 0 ||
            (c->field_count && !c->fields) || (c->method_count && !c->methods))
            return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        size_t count = (size_t) c->field_count + (size_t) c->method_count;
        if (count > (size_t) INT_MAX - total)
            return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
        total += count;
    }
    if (!total)
        return XR_COMPILE_RESOURCE_OK;
    XaMemberList result = {0};
    void *memory = NULL;
    status = xr_compile_state_calloc(state, total, sizeof(*result.items), &memory);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    result.items = memory;
    for (const XrClassInfo *c = info; c && status == XR_COMPILE_RESOURCE_OK;) {
        status = xr_compile_state_work(state, 1);
        for (int i = 0; status == XR_COMPILE_RESOURCE_OK && i < c->field_count; ++i) {
            status = xr_compile_state_work(state, 1);
            if (status == XR_COMPILE_RESOURCE_OK)
                result.items[result.count++] = c->fields[i];
        }
        for (int i = 0; status == XR_COMPILE_RESOURCE_OK && i < c->method_count; ++i) {
            status = xr_compile_state_work(state, 1);
            if (status == XR_COMPILE_RESOURCE_OK)
                result.items[result.count++] = c->methods[i];
        }
        if (status == XR_COMPILE_RESOURCE_OK)
            c = c->base;
    }
    if (status != XR_COMPILE_RESOURCE_OK) {
        xa_member_list_dispose(&result);
        return status;
    }
    *output = result;
    return XR_COMPILE_RESOURCE_OK;
}
