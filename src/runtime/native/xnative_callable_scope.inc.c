/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_callable_scope.inc.c - Rooted activation owners for typed C closures
 */
#include "xnative_value.h"
#include "xnative_module_state.h"

typedef struct XrtCallableGroup {
    struct XrtCallableGroup *next;
    const XrtCallableImage *image;
    XrXirTypeArena *types;
    XrtCallableCode *code;
    XrXirDomain *domain;
    XrtCallableEntry *workspace;
} XrtCallableGroup;
typedef struct XrtCallableScopeValue {
    XrXirValue value;
    uint32_t binding;
} XrtCallableScopeValue;
typedef struct XrtCallableScopeBody {
    struct XrtCallableScopeBody *previous, *next;
    XrtCallableGroup *group;
    uint64_t identity;
    XrtCallableScopeValue *values;
    XrXirValue *arguments;
    uint32_t count, capacity, argument_capacity;
} XrtCallableScopeBody;
struct XrtCallableRegistry {
    XrtCallableGroup *groups;
    XrtCallableScopeBody *bodies;
};

static void callable_scope_require(XrXirValueStatus status, uint32_t source) {
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, source);
}
static bool callable_image_retain(void *image) { return image != NULL; }
static void callable_image_release(void *image) { (void)image; }

static XrtCallableRegistry *callable_registry(uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!root->callables) {
        root->callables = xr_calloc(1, sizeof(*root->callables));
        if (!root->callables) xrt_value_failure(XR_XIR_VALUE_OOM, source);
    }
    return root->callables;
}
static XrtCallableGroup *callable_group_receive(XrtCallableRegistry *registry,
    const XrtCallableImage *image, XrXirTypeArena *arena, uint32_t source) {
    for (XrtCallableGroup *group = registry->groups; group; group = group->next)
        if (group->image == image) {
            if (arena && group->types != arena) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
            return group;
        }
    if (!image || !image->entry_count || !image->entries || image->entry_count > 65536)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtCallableGroup *group = xr_calloc(1, sizeof(*group));
    if (!group) xrt_value_failure(XR_XIR_VALUE_OOM, source);
    group->image = image;
    group->next = registry->groups;
    registry->groups = group;
    XrtValueRoot *root = xrt_value_root_current(source);
    uint64_t limit = root->byte_limit ? root->byte_limit : UINT64_MAX;
    if (arena) {
        if (!xr_xir_compile_type_arena_retain(arena)) xrt_value_failure(XR_XIR_VALUE_REFCOUNT_LIMIT, source);
        group->types = arena;
    } else callable_scope_require(xrt_callable_types_new(image->types, image->type_count, limit, &group->types), source);
    size_t captures = 0;
    for (uint32_t e = 0; e < image->entry_count; ++e) {
        XrtCallableEntrySpec entry = image->entries[e];
        if (entry.capture_count > 65536 || (entry.capture_count && !entry.captures))
            xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        if (entry.capture_count > SIZE_MAX - captures) xrt_value_failure(XR_XIR_VALUE_OVERFLOW, source);
        captures += entry.capture_count;
    }
    size_t entries = image->entry_count;
    if (entries > SIZE_MAX / sizeof(*group->workspace)) xrt_value_failure(XR_XIR_VALUE_OVERFLOW, source);
    size_t bytes = entries * sizeof(*group->workspace);
    if (captures > (SIZE_MAX - bytes) / sizeof(XrXirType)) xrt_value_failure(XR_XIR_VALUE_OVERFLOW, source);
    group->workspace = xr_malloc(bytes + captures * sizeof(XrXirType));
    if (!group->workspace) xrt_value_failure(XR_XIR_VALUE_OOM, source);
    _Static_assert(sizeof(XrtCallableEntry) % _Alignof(XrXirType) == 0, "capture tail alignment");
    XrXirType *next = (XrXirType *)(group->workspace + entries);
    for (uint32_t e = 0; e < image->entry_count; ++e) {
        XrtCallableEntrySpec entry = image->entries[e];
        XrtCallableEntry *received = &group->workspace[e];
        if (!xrt_callable_type_id((XrtCallableValueSpec){XRT_CALLABLE_CONSTRUCTED, entry.type},
                image->type_count, &received->type)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        received->capture_count = entry.capture_count;
        received->captures = entry.capture_count ? next : NULL;
        for (uint32_t p = 0; p < entry.capture_count; ++p)
            if (!xrt_callable_type_id(entry.captures[p],
                    image->type_count, next++)) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    }
    callable_scope_require(xrt_callable_code_new(group->types, group->workspace, image->entry_count,
        (XrtCallableLease){(void *)image, callable_image_retain, callable_image_release}, &group->code), source);
    xr_free(group->workspace);
    group->workspace = NULL;
    callable_scope_require(xr_xir_domain_new(limit, &group->domain), source);
    return group;
}
static XrtCallableGroup *callable_group(XrtCallableRegistry *registry, const XrtCallableImage *image, uint32_t source) {
    return callable_group_receive(registry, image, NULL, source);
}
XRAY_API void xrt_callable_state_prepare(XrtModuleState *state, const XrtCallableImage *image, uint32_t source) {
    XrXirTypeArena *arena = xrt_module_state_callables(state, image);
    if (!arena) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    (void)callable_group_receive(callable_registry(source), image, arena, source);
}
static void callable_scope_grow(XrtCallableScopeBody *scope, bool arguments, uint32_t count, uint32_t source) {
    uint32_t *capacity = arguments ? &scope->argument_capacity : &scope->capacity;
    if (count <= *capacity) return;
    uint32_t next = *capacity > UINT32_MAX / 2 ? UINT32_MAX : *capacity * 2;
    if (next < count) next = count;
    size_t item_size = arguments ? sizeof(*scope->arguments) : sizeof(*scope->values);
    size_t bytes = next;
    if (bytes > SIZE_MAX / item_size) xrt_value_failure(XR_XIR_VALUE_OVERFLOW, source);
    bytes *= item_size;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    void *storage = xr_xir_domain_allocate(scope->group->domain, bytes, &status);
    callable_scope_require(status, source);
    void *previous = arguments ? (void *)scope->arguments : (void *)scope->values;
    if (*capacity) {
        memcpy(storage, previous, (size_t)*capacity * item_size);
        xr_xir_domain_deallocate(scope->group->domain, previous, (size_t)*capacity * item_size);
    }
    if (arguments) scope->arguments = storage;
    else scope->values = storage;
    *capacity = next;
}
static void callable_scope_drop(XrtCallableRegistry *registry, XrtCallableScopeBody *scope) {
    if (scope->previous) scope->previous->next = scope->next;
    else registry->bodies = scope->next;
    if (scope->next) scope->next->previous = scope->previous;
    for (uint32_t n = scope->count; n; --n) xr_xir_value_drop(&scope->values[n - 1].value);
    if (scope->values) xr_xir_domain_deallocate(scope->group->domain, scope->values,
        (size_t)scope->capacity * sizeof(*scope->values));
    if (scope->arguments) xr_xir_domain_deallocate(scope->group->domain, scope->arguments,
        (size_t)scope->argument_capacity * sizeof(*scope->arguments));
    xr_xir_domain_deallocate(scope->group->domain, scope, sizeof(*scope));
}
static XrtCallableScopeBody *callable_scope_body(XrtCallableScope *scope, uint32_t source) {
    if (scope->body) return scope->body;
    XrtCallableRegistry *registry = callable_registry(source);
    XrtCallableGroup *group = callable_group(registry, scope->image, source);
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    void *storage = xr_xir_domain_allocate(group->domain, sizeof(XrtCallableScopeBody), &status);
    callable_scope_require(status, source);
    XrtCallableScopeBody *body = storage;
    XrtCallableScopeBody *previous = NULL, *next = registry->bodies;
    while (next && next->identity > scope->identity) {
        previous = next;
        next = next->next;
    }
    *body = (XrtCallableScopeBody){.previous=previous, .next=next, .group=group, .identity=scope->identity};
    if (next) next->previous = body;
    if (previous) previous->next = body;
    else registry->bodies = body;
    scope->body = body;
    return body;
}
XRAY_API void xrt_callable_scope_enter(XrtCallableScope *scope, const XrtCallableImage *image, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!scope || !image)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if (root->callable_identity == UINT64_MAX) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    *scope = (XrtCallableScope){.image=image, .parent=root->callable_scope,
        .root=root, .identity=++root->callable_identity};
    root->callable_scope = scope;
}
XRAY_API XrtCallable *xrt_callable_scope_new(XrtCallableScope *scope, uint32_t entry,
    const uint64_t *captures, uint32_t count, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!scope || scope != root->callable_scope || scope->root != root ||
        entry >= scope->image->entry_count || count != scope->image->entries[entry].capture_count || (count && !captures))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtCallableScopeBody *body = callable_scope_body(scope, source);
    if (body->count == UINT32_MAX) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    callable_scope_grow(body, false, body->count + 1, source);
    callable_scope_grow(body, true, count, source);
    XrtCallableEntry expected = body->group->code->entries[entry];
    for (uint32_t p = 0; p < count; ++p) {
        body->arguments[p] = (XrXirValue){.type=(uint32_t)expected.captures[p]};
        memcpy(&body->arguments[p].payload, &captures[p], sizeof(captures[p]));
    }
    XrtCallableScopeValue *received = &body->values[body->count];
    *received = (XrtCallableScopeValue){.binding=UINT32_MAX};
    XrXirValue *value = &received->value;
    callable_scope_require(xrt_callable_new(body->group->code, body->group->domain,
        entry, count ? body->arguments : NULL, count, value), source);
    ++body->count;
    return (XrtCallable *)(intptr_t)value->payload;
}
XRAY_API uint64_t xrt_callable_capture(const XrtCallableImage *image, const XrtCallable *function,
    uint32_t entry, uint32_t capture, uint32_t source) {
    XrtCallableGroup *group = callable_group(callable_registry(source), image, source);
    if (!function || entry >= group->code->count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrXirValue value = {.type=(uint32_t)group->code->entries[entry].type, .payload=(int64_t)(intptr_t)function};
    const XrXirValue *captures = NULL;
    uint32_t count = 0;
    callable_scope_require(xrt_callable_borrow(group->code, &value, entry, &captures, &count), source);
    if (capture >= count) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    uint64_t bits;
    memcpy(&bits, &captures[capture].payload, sizeof(bits));
    return bits;
}
static XrXirValue callable_scope_value(XrtCallableGroup *group, XrtCallable *function, uint32_t source) {
    const XirObject *object = (const XirObject *)function;
    XrXirValue value = {.type=(uint32_t)object->type, .payload=(int64_t)(intptr_t)function};
    const XrXirFunctionBinding *binding = xr_xir_function_binding(&value);
    const XrXirValue *captures = NULL;
    uint32_t count = 0;
    if (!binding) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    callable_scope_require(xrt_callable_borrow(group->code, &value,
        binding->entry, &captures, &count), source);
    return value;
}
XRAY_API uint32_t xrt_callable_entry(const XrtCallableImage *image, const XrtCallable *function, uint32_t source) {
    if (!function) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtCallableGroup *group = callable_group(callable_registry(source), image, source);
    XrXirValue value = callable_scope_value(group, (XrtCallable *)function, source);
    return xr_xir_function_binding(&value)->entry;
}
XRAY_API XrtCallable *xrt_callable_scope_copy(XrtCallableScope *scope, XrtCallable *function, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!scope || scope != root->callable_scope || scope->root != root || !function)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    XrtCallableScopeBody *body = callable_scope_body(scope, source);
    if (body->count == UINT32_MAX) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
    XrXirValue value = callable_scope_value(body->group, function, source);
    callable_scope_grow(body, false, body->count + 1, source);
    XrtCallableScopeValue *received = &body->values[body->count];
    *received = (XrtCallableScopeValue){.binding=UINT32_MAX};
    callable_scope_require(xr_xir_value_copy(&value, &received->value), source);
    ++body->count;
    return function;
}
XRAY_API XrtCallable *xrt_callable_scope_bind(XrtCallableScope *scope, uint32_t binding,
    XrtCallable *function, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    XrtCallableScope *current = root->callable_scope;
    while (current && current != scope) current = current->parent;
    if (!current || current->root != root || binding == UINT32_MAX)
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if (!function && !scope->body) return NULL;
    XrtCallableScopeBody *body = callable_scope_body(scope, source);
    uint32_t slot = 0;
    while (slot < body->count && body->values[slot].binding != binding) ++slot;
    if (!function && slot == body->count) return NULL;
    bool existing = slot < body->count;
    if (slot == body->count) {
        if (body->count == UINT32_MAX) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
        callable_scope_grow(body, false, body->count + 1, source);
    }
    XrXirValue received = {0};
    if (function) {
        XrXirValue value = callable_scope_value(body->group, function, source);
        callable_scope_require(xr_xir_value_copy(&value, &received), source);
    }
    XrXirValue previous = existing ? body->values[slot].value : (XrXirValue){0};
    if (!existing) {
        slot = 0;
        while (slot < body->count && body->values[slot].binding < binding) ++slot;
        memmove(body->values + slot + 1, body->values + slot,
            (size_t)(body->count - slot) * sizeof(*body->values));
        ++body->count;
    }
    body->values[slot] = (XrtCallableScopeValue){received, binding};
    xr_xir_value_drop(&previous);
    return function;
}
XRAY_API XrtCallable *xrt_callable_scope_leave(XrtCallableScope *scope, XrtCallable *result, uint32_t source) {
    XrtValueRoot *root = xrt_value_root_current(source);
    if (!scope || scope != root->callable_scope || scope->root != root || (result && !scope->parent))
        xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
    if (result) {
        XrtCallableGroup *group = callable_group(callable_registry(source), scope->image, source);
        XrXirValue value = callable_scope_value(group, result, source);
        XrtCallableScopeBody *parent = callable_scope_body(scope->parent, source);
        if (parent->group != group) xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source);
        if (parent->count == UINT32_MAX) xrt_value_failure(XR_XIR_VALUE_LIMIT, source);
        callable_scope_grow(parent, false, parent->count + 1, source);
        XrtCallableScopeValue *received = &parent->values[parent->count];
        *received = (XrtCallableScopeValue){.binding=UINT32_MAX};
        callable_scope_require(xr_xir_value_copy(&value, &received->value), source);
        ++parent->count;
    }
    root->callable_scope = scope->parent;
    if (scope->body) callable_scope_drop(root->callables, scope->body);
    scope->body = NULL;
    return result;
}
#include "xnative_callable_cell.inc.c"

XR_FUNC void xrt_callable_root_drain(XrtValueRoot *root) {
    XrtCallableRegistry *registry = root->callables;
    /* Automatic descriptors are synchronous borrows. A longjmp can already
     * have retired their C frames; only the owned heap bodies are visited. */
    root->callable_scope = NULL;
    root->callable_identity = 0;
    if (!registry) return;
    while (registry->bodies) callable_scope_drop(registry, registry->bodies);
    while (registry->groups) {
        XrtCallableGroup *group = registry->groups;
        registry->groups = group->next;
        if (group->domain) {
            XR_CHECK(xr_xir_domain_close(group->domain) == XR_XIR_VALUE_OK, "closure graph has no active operations");
            xr_xir_domain_drop(group->domain);
        }
        xrt_callable_code_drop(&group->code);
        xr_xir_compile_type_arena_drop(group->types);
        xr_free(group->workspace);
        xr_free(group);
    }
    xr_free(registry);
    root->callables = NULL;
}
