/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_module_state.c - Independently owned typed module values
 *
 * KEY CONCEPT:
 *   A state retains immutable type operations and owns successful copies.
 *   Empty scratch makes replacement atomic even when reference copying fails.
 */
#include "xnative_module_state.h"
#include "xnative_string.h"
#include "xnative_array.h"
#include "xnative_map.h"
#include "xnative_module_cell.h"
#include "xnative_callable.h"
#include "xir/xxir_type_arena_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_value_internal.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"
#include "xray_native_pod.h"
#include <stdatomic.h>
#include <string.h>

typedef struct XrtModuleType XrtModuleType;
typedef XrXirValueStatus (*XrtModuleCopy)(const XrtModuleType *, const void *, void *);
typedef void (*XrtModuleDrop)(const XrtModuleType *, void *);
struct XrtModuleType {
    XrtModuleTypeKind kind;
    uint32_t size, alignment;
    XrtModuleCopy copy;
    XrtModuleDrop drop;
    const XrXirTypeArena *arena;
    XrXirType callable;
    const XrtModuleType *element;
};
struct XrtModuleLayout {
    atomic_size_t references;
    XrtModuleLayoutSpec view;
    XrtModuleType *types;
    uint32_t *slot_counts;
    uint64_t bytes;
    uint32_t scratch_size;
    XrXirTypeArena *array_types;
    XrXirTypeArena *callable_types;
    XrXirTypeArena *map_types;
};
enum { MODULE_EMPTY, MODULE_INITIALIZING, MODULE_READY };
enum { SLOT_CONSTRUCTED = 1u, SLOT_PUBLISHED = 2u };
struct XrtModuleRef {
    XrtModuleState *state;
    uint32_t slot;
    uint8_t required_flags;
};
struct XrtModuleState {
    XrtModuleLayout *layout;
    uint8_t *payload, *scratch, *flags, *modules;
    uint32_t *construction, *publication, *remaining;
    XrtModuleRef *initializers, *published;
    uint32_t construction_count, publication_count;
    uint32_t allocation_alignment;
    uint64_t bytes, byte_limit;
    uint8_t unit;
};

static XrXirValueStatus module_scalar_copy(const XrtModuleType *type, const void *source, void *empty) {
    if (!type->size) return XR_XIR_VALUE_OK;
    if (!source) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (type->kind == XRT_MODULE_BOOL && *(const uint8_t *)source > 1) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (type->kind == XRT_MODULE_RUNE) {
        int64_t rune;
        memcpy(&rune, source, sizeof(rune));
        if (!xr_xir_rune_payload_valid(rune)) return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    memcpy(empty, source, type->size);
    return XR_XIR_VALUE_OK;
}

static void module_scalar_drop(const XrtModuleType *type, void *value) {
    if (type->size) memset(value, 0, type->size);
}

static XrXirValueStatus module_string_copy(const XrtModuleType *type, const void *source, void *empty) {
    (void)type;
    if (!source) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrtString *string;
    memcpy(&string, source, sizeof(string));
    XrtString *owned = NULL;
    XrXirValueStatus status = xrt_string_owned_copy(string, &owned);
    if (status == XR_XIR_VALUE_OK) memcpy(empty, &owned, sizeof(owned));
    return status;
}

static void module_string_drop(const XrtModuleType *type, void *value) {
    (void)type;
    XrtString *owned;
    memcpy(&owned, value, sizeof(owned));
    memset(value, 0, sizeof(owned));
    xrt_string_owned_drop(&owned);
}

static XrXirValueStatus module_array_copy(const XrtModuleType *type, const void *source, void *empty) {
    if (!source) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrtArray *array = NULL;
    memcpy(&array, source, sizeof(array));
    XrtArray *owned = NULL;
    XrtArrayElement element;
    switch (type->kind) {
    case XRT_MODULE_ARRAY_I64: element = XRT_ARRAY_I64; break;
    case XRT_MODULE_ARRAY_STRING: element = XRT_ARRAY_STRING; break;
    case XRT_MODULE_ARRAY_U8: element = XRT_ARRAY_U8; break;
    case XRT_MODULE_ARRAY_U64: element = XRT_ARRAY_U64; break;
    default: return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    XrXirValueStatus status = xrt_array_owned_copy(array, type->arena, element, &owned);
    if (status == XR_XIR_VALUE_OK) memcpy(empty, &owned, sizeof(owned));
    return status;
}
static void module_array_drop(const XrtModuleType *type, void *value) {
    (void)type;
    XrtArray *array = NULL;
    memcpy(&array, value, sizeof(array));
    memset(value, 0, sizeof(array));
    xrt_array_owned_drop(&array);
}

static XrXirValueStatus module_nullable_copy(const XrtModuleType *type, const void *source, void *empty) {
    if (!source) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrtModuleType *current = type;
    const uint8_t *input = source;
    uint8_t *output = empty;
    memset(empty, 0, type->size);
    while (current->kind == XRT_MODULE_NULLABLE) {
        if (*input > 1) { memset(empty, 0, type->size); return XR_XIR_VALUE_BAD_ARGUMENT; }
        if (!*input) return XR_XIR_VALUE_OK;
        *output = 1;
        input += current->element->alignment;
        output += current->element->alignment;
        current = current->element;
    }
    XrXirValueStatus status = current->copy(current, input, output);
    if (status != XR_XIR_VALUE_OK) memset(empty, 0, type->size);
    return status;
}

static XrXirValueStatus module_cell_copy(const XrtModuleType *, const void *, void *);
static void module_cell_drop(const XrtModuleType *, void *);

static bool module_callable_charge(void *owner, uint64_t amount) {
    uint64_t *remaining = owner;
    if (amount > *remaining) return false;
    *remaining -= amount;
    return true;
}
static XrXirValueStatus module_callable_copy(const XrtModuleType *type, const void *source, void *empty) {
    if (!source) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirObject *object = NULL;
    memcpy(&object, source, sizeof(object));
    if (!object) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue value = {.type=(uint32_t)object->type, .payload=(int64_t)(intptr_t)object};
    uint64_t work = UINT64_MAX;
    if (xr_xir_value_arena(&value) != type->arena ||
        !xr_xir_value_argument(&value, type->arena, (XrXirType)value.type) ||
        xr_xir_callable_weakening_admit(xr_xir_compile_type_arena_types(type->arena),
            (XrXirType)value.type, type->callable, &work, module_callable_charge) != XR_XIR_OK ||
        !xr_xir_function_binding(&value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue received = {0};
    XrXirValueStatus status = xr_xir_value_copy(&value, &received);
    if (status == XR_XIR_VALUE_OK) memcpy(empty, &object, sizeof(object));
    return status;
}
static void module_callable_drop(const XrtModuleType *type, void *storage) {
    (void)type;
    XirObject *object = NULL;
    memcpy(&object, storage, sizeof(object));
    memset(storage, 0, sizeof(object));
    if (object) {
        XrXirValue value = {.type=(uint32_t)object->type, .payload=(int64_t)(intptr_t)object};
        xr_xir_value_drop(&value);
    }
}

static XrXirValueStatus module_map_copy(const XrtModuleType *type, const void *source, void *empty) {
    if (!source) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrtMap *map = NULL; XrtMap *copy = NULL;
    memcpy(&map, source, sizeof(map));
    XrXirValueStatus status = xrt_map_owned_copy(map, type->arena, &copy);
    if (status == XR_XIR_VALUE_OK) memcpy(empty, &copy, sizeof(copy));
    return status;
}
static void module_map_drop(const XrtModuleType *type, void *storage) {
    (void)type;
    XrtMap *map = NULL;
    memcpy(&map, storage, sizeof(map)); memset(storage, 0, sizeof(map));
    xrt_map_owned_drop(&map);
}
static bool module_type(const XrtModuleLayoutSpec *layout, uint32_t ordinal, XrtModuleType *types) {
    const XrtModuleTypeSpec *spec = &layout->types[ordinal];
    XrtModuleType *type = &types[ordinal];
    if (spec->kind != XRT_MODULE_FIXED_U8 && spec->kind != XRT_MODULE_NULLABLE &&
        spec->kind != XRT_MODULE_CALLABLE && spec->kind != XRT_MODULE_TUPLE_I64 && spec->argument) return false;
    XrtModuleTypeKind kind = spec->kind;
    *type = (XrtModuleType){.kind=kind, .copy=module_scalar_copy, .drop=module_scalar_drop};
    switch (kind) {
    case XRT_MODULE_TUPLE_I64:
        if (!spec->argument || spec->argument > UINT32_MAX / 8) return false;
        type->size = spec->argument * 8; type->alignment = 8; return true;
    case XRT_MODULE_UNIT: type->alignment = 1; return true;
    case XRT_MODULE_BOOL: case XRT_MODULE_I8: case XRT_MODULE_U8:
        type->size = type->alignment = 1; return true;
    case XRT_MODULE_I16: case XRT_MODULE_U16: type->size = type->alignment = 2; return true;
    case XRT_MODULE_I32: case XRT_MODULE_U32: case XRT_MODULE_F32:
        type->size = type->alignment = 4; return true;
    case XRT_MODULE_RUNE: case XRT_MODULE_I64: case XRT_MODULE_U64: case XRT_MODULE_F64:
        type->size = type->alignment = 8; return true;
    case XRT_MODULE_STRING:
        type->size = sizeof(XrtString *); type->alignment = _Alignof(XrtString *);
        type->copy = module_string_copy; type->drop = module_string_drop; return true;
    case XRT_MODULE_ARRAY_I64: case XRT_MODULE_ARRAY_STRING:
    case XRT_MODULE_ARRAY_U8: case XRT_MODULE_ARRAY_U64:
        type->size = sizeof(XrtArray *); type->alignment = _Alignof(XrtArray *);
        type->copy = module_array_copy; type->drop = module_array_drop; return true;
    case XRT_MODULE_MAP_STRING_I64:
        type->size = sizeof(XrtMap *); type->alignment = _Alignof(XrtMap *);
        type->copy = module_map_copy; type->drop = module_map_drop; return true;
    case XRT_MODULE_NULLABLE: {
        if (spec->argument >= ordinal) return false;
        const XrtModuleType *element = &types[spec->argument];
        if (element->copy != module_scalar_copy && element->copy != module_nullable_copy) return false;
        XrPodLayout payload = {element->size, element->alignment, 0}, shape;
        if (!xr_pod_nullable_layout(payload, &shape)) return false;
        type->size = shape.size; type->alignment = shape.alignment;
        type->copy = module_nullable_copy; type->element = element;
        return true;
    }
    case XRT_MODULE_CELL_ARRAY_I64:
        type->size = sizeof(XrXirValue); type->alignment = _Alignof(XrXirValue);
        type->copy = module_cell_copy; type->drop = module_cell_drop;
        return true;
    case XRT_MODULE_CALLABLE:
        if (spec->argument >= layout->callable_type_count ||
            layout->callable_types[spec->argument].kind != XRT_CALLABLE_SIGNATURE) return false;
        type->size = sizeof(void *); type->alignment = _Alignof(void *);
        type->copy = module_callable_copy; type->drop = module_callable_drop;
        type->callable = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 4u + spec->argument);
        return true;
    case XRT_MODULE_FIXED_U8:
        if (!spec->argument || spec->argument > 65535) return false;
        type->size = spec->argument;
        type->alignment = 1;
        return true;
    default: return false;
    }
}

static bool module_charge(uint64_t *bytes, uint64_t count, size_t size) {
    if (size && count > (UINT64_MAX - *bytes) / size) return false;
    *bytes += count * size;
    return *bytes <= SIZE_MAX;
}

static void *module_copy_array(const void *source, uint32_t count, size_t size) {
    if (!count) return NULL;
    void *copy = xr_malloc((size_t)count * size);
    if (copy) memcpy(copy, source, (size_t)count * size);
    return copy;
}

#include "xnative_module_layout.inc.c"

static void *module_payload(const XrtModuleState *state, uint32_t slot) {
    uint64_t offset = state->layout->view.slots[slot].offset;
    return state->payload ? state->payload + (size_t)offset : (void *)&state->unit;
}
XR_FUNC bool xrt_module_callable_signature(const XrtModuleLayout *layout, uint32_t type,
    const XrXirTypeArena **arena, XrXirType *signature) {
    if (!layout || type >= layout->view.type_count || !arena || !signature ||
        layout->types[type].kind != XRT_MODULE_CALLABLE) return false;
    *arena = layout->types[type].arena;
    *signature = layout->types[type].callable;
    return true;
}
XR_FUNC XrXirTypeArena *xrt_module_callable_catalog(const XrtModuleLayout *layout) {
    return layout ? layout->callable_types : NULL;
}
XR_FUNC XrXirTypeArena *xrt_module_state_callables(const XrtModuleState *state, const XrtCallableImage *image) {
    if (!state || !image || image->type_count != state->layout->view.callable_type_count ||
        !xrt_callable_specs_equal(image->types, state->layout->view.callable_types, image->type_count)) return NULL;
    return state->layout->callable_types;
}

static const XrtModuleType *module_slot_type(const XrtModuleState *state, uint32_t slot) {
    return &state->layout->types[state->layout->view.slots[slot].type];
}

static bool module_reference_valid(const XrtModuleRef *reference) {
    if (!reference || !reference->state || !reference->state->layout) return false;
    XrtModuleState *state = reference->state;
    if (reference->slot >= state->layout->view.slot_count) return false;
    return (reference == &state->initializers[reference->slot] && reference->required_flags == SLOT_CONSTRUCTED) ||
        (reference == &state->published[reference->slot] && reference->required_flags == SLOT_PUBLISHED);
}

XRAY_API void xrt_module_state_clear(XrtModuleState *state) {
    if (!state || !state->layout) return;
    while (state->construction_count) {
        uint32_t slot = state->construction[--state->construction_count];
        if (state->flags[slot] == SLOT_CONSTRUCTED) {
            const XrtModuleType *type = module_slot_type(state, slot);
            type->drop(type, module_payload(state, slot));
            state->flags[slot] = 0;
        }
    }
    while (state->publication_count) {
        uint32_t slot = state->publication[--state->publication_count];
        const XrtModuleType *type = module_slot_type(state, slot);
        type->drop(type, module_payload(state, slot));
        state->flags[slot] = 0;
    }
    if (state->modules) memset(state->modules, 0, state->layout->view.module_count);
    if (state->remaining) memcpy(state->remaining, state->layout->slot_counts,
        (size_t)state->layout->view.module_count * sizeof(*state->remaining));
}

XRAY_API void xrt_module_state_drop(XrtModuleState **owner) {
    if (!owner || !*owner) return;
    XrtModuleState *state = *owner;
    *owner = NULL;
    xrt_module_state_clear(state);
    xr_free_aligned(state->payload, state->allocation_alignment);
    xr_free_aligned(state->scratch, state->allocation_alignment);
    xr_free(state->flags); xr_free(state->modules);
    xr_free(state->construction); xr_free(state->publication); xr_free(state->remaining);
    xr_free(state->initializers); xr_free(state->published);
    xrt_module_layout_drop(&state->layout);
    xr_free(state);
}

static XrXirValueStatus module_state_allocate(XrtModuleState *state) {
    const XrtModuleLayoutSpec *layout = &state->layout->view;
    uint32_t slots = layout->slot_count, modules = layout->module_count;
    state->allocation_alignment = layout->payload_alignment < sizeof(void *) ?
        (uint32_t)sizeof(void *) : layout->payload_alignment;
    if (layout->payload_size) state->payload = xr_malloc_aligned((size_t)layout->payload_size, state->allocation_alignment);
    if (state->layout->scratch_size) state->scratch = xr_malloc_aligned(state->layout->scratch_size, state->allocation_alignment);
    if (slots) {
        state->flags = xr_calloc(slots, 1);
        state->construction = xr_malloc((size_t)slots * sizeof(*state->construction));
        state->publication = xr_malloc((size_t)slots * sizeof(*state->publication));
        state->initializers = xr_calloc(slots, sizeof(*state->initializers));
        state->published = xr_calloc(slots, sizeof(*state->published));
    }
    state->modules = xr_calloc(modules, 1);
    state->remaining = xr_malloc((size_t)modules * sizeof(*state->remaining));
    if ((layout->payload_size && !state->payload) || (state->layout->scratch_size && !state->scratch) ||
        (slots && (!state->flags || !state->construction || !state->publication || !state->initializers || !state->published)) ||
        !state->modules || !state->remaining) return XR_XIR_VALUE_OOM;
    if (state->payload) memset(state->payload, 0, (size_t)layout->payload_size);
    if (state->scratch) memset(state->scratch, 0, state->layout->scratch_size);
    memcpy(state->remaining, state->layout->slot_counts, (size_t)modules * sizeof(*state->remaining));
    for (uint32_t s = 0; s < slots; ++s) {
        state->initializers[s] = (XrtModuleRef){state, s, SLOT_CONSTRUCTED};
        state->published[s] = (XrtModuleRef){state, s, SLOT_PUBLISHED};
    }
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_state_new(XrtModuleLayout *layout, uint64_t limit, XrtModuleState **out) {
    if (!layout || !out || *out) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint64_t bytes = layout->bytes;
    uint32_t slots = layout->view.slot_count, modules = layout->view.module_count;
    uint32_t alignment = layout->view.payload_alignment < sizeof(void *) ?
        (uint32_t)sizeof(void *) : layout->view.payload_alignment;
    if (!module_charge(&bytes, 1, sizeof(XrtModuleState)) ||
        !module_charge(&bytes, layout->view.payload_size, 1) ||
        !module_charge(&bytes, layout->scratch_size, 1) ||
        !module_charge(&bytes, 2, alignment - 1) ||
        !module_charge(&bytes, slots, 1 + 2 * sizeof(uint32_t) + 2 * sizeof(XrtModuleRef)) ||
        !module_charge(&bytes, modules, 1 + sizeof(uint32_t))) return XR_XIR_VALUE_OVERFLOW;
    if (bytes > limit) return XR_XIR_VALUE_MEMORY_LIMIT;
    if (!xrt_module_layout_retain(layout)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrtModuleState *state = xr_calloc(1, sizeof(*state));
    if (!state) { xrt_module_layout_drop(&layout); return XR_XIR_VALUE_OOM; }
    state->layout = layout;
    state->bytes = bytes;
    state->byte_limit = limit;
    XrXirValueStatus status = module_state_allocate(state);
    if (status != XR_XIR_VALUE_OK) { xrt_module_state_drop(&state); return status; }
    *out = state;
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_begin(XrtModuleState *state, uint32_t module) {
    if (!state || module >= state->layout->view.module_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (state->modules[module] != MODULE_EMPTY) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrtModuleSpec spec = state->layout->view.modules[module];
    for (uint32_t d = 0; d < spec.dependency_count; ++d)
        if (state->modules[state->layout->view.dependencies[spec.first_dependency + d]] != MODULE_READY)
            return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus shared = xrt_value_root_share_types(state->layout->array_types);
    if (shared != XR_XIR_VALUE_OK) return shared;
    state->modules[module] = MODULE_INITIALIZING;
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_ready(XrtModuleState *state, uint32_t module) {
    if (!state || module >= state->layout->view.module_count ||
        state->modules[module] != MODULE_INITIALIZING || state->remaining[module]) return XR_XIR_VALUE_BAD_ARGUMENT;
    state->modules[module] = MODULE_READY;
    return XR_XIR_VALUE_OK;
}

static XrXirValueStatus module_initial_copy(XrtModuleState *state, uint32_t slot, const void *value) {
    const XrtModuleType *type = module_slot_type(state, slot);
    return type->copy(type, value, module_payload(state, slot));
}

static XrtModuleStatus module_construct_value(XrtModuleState *state, uint32_t slot,
    const void *value, XrtModuleRef **out) {
    if (!state || !out || *out || slot >= state->layout->view.slot_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrtModuleSlotSpec *spec = &state->layout->view.slots[slot];
    if (!(spec->flags & XRT_MODULE_SLOT_MUTABLE) || state->flags[slot] ||
        state->modules[spec->module] != MODULE_INITIALIZING) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = module_initial_copy(state, slot, value);
    if (status != XR_XIR_VALUE_OK) return status;
    state->flags[slot] = SLOT_CONSTRUCTED;
    state->construction[state->construction_count++] = slot;
    *out = &state->initializers[slot];
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_construct(XrtModuleState *state, uint32_t slot,
    const void *value, XrtModuleRef **out) {
    if (!state || slot >= state->layout->view.slot_count ||
        module_slot_type(state, slot)->kind == XRT_MODULE_CELL_ARRAY_I64) return XR_XIR_VALUE_BAD_ARGUMENT;
    return module_construct_value(state, slot, value, out);
}

#include "xnative_module_cell.inc.c"

XRAY_API XrtModuleStatus xrt_module_publish(XrtModuleRef *reference) {
    if (!module_reference_valid(reference) || reference->required_flags != SLOT_CONSTRUCTED)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrtModuleState *state = reference->state;
    uint32_t slot = reference->slot;
    const XrtModuleSlotSpec *spec = &state->layout->view.slots[slot];
    if (state->flags[slot] != SLOT_CONSTRUCTED || !(spec->flags & XRT_MODULE_SLOT_MUTABLE) ||
        state->modules[spec->module] != MODULE_INITIALIZING) return XR_XIR_VALUE_BAD_ARGUMENT;
    state->flags[slot] |= SLOT_PUBLISHED;
    state->publication[state->publication_count++] = slot;
    --state->remaining[spec->module];
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_initialize(XrtModuleState *state, uint32_t slot, const void *value) {
    if (!state || slot >= state->layout->view.slot_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrtModuleSlotSpec *spec = &state->layout->view.slots[slot];
    if ((spec->flags & XRT_MODULE_SLOT_MUTABLE) || state->flags[slot] ||
        state->modules[spec->module] != MODULE_INITIALIZING) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = module_initial_copy(state, slot, value);
    if (status != XR_XIR_VALUE_OK) return status;
    state->flags[slot] = SLOT_CONSTRUCTED | SLOT_PUBLISHED;
    state->publication[state->publication_count++] = slot;
    --state->remaining[spec->module];
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_initialize_i64_group(XrtModuleState *state,
    uint32_t first, uint32_t count, const int64_t *values) {
    if (!state || !count || !values || first >= state->layout->view.slot_count ||
        count > state->layout->view.slot_count - first) return XR_XIR_VALUE_BAD_ARGUMENT;
    uint32_t module = state->layout->view.slots[first].module;
    if (state->modules[module] != MODULE_INITIALIZING) return XR_XIR_VALUE_BAD_ARGUMENT;
    bool initialize = false;
    for (uint32_t n = 0; n < count; ++n) {
        uint32_t slot = first + n;
        const XrtModuleSlotSpec *spec = &state->layout->view.slots[slot];
        uint8_t required = (spec->flags & XRT_MODULE_SLOT_MUTABLE) ? SLOT_CONSTRUCTED : 0;
        if (spec->module != module || module_slot_type(state, slot)->kind != XRT_MODULE_I64 ||
            state->flags[slot] != required) return XR_XIR_VALUE_BAD_ARGUMENT;
        initialize |= !(spec->flags & XRT_MODULE_SLOT_MUTABLE);
    }
    if (initialize) {
        if (sizeof(*values) > SIZE_MAX / count) return XR_XIR_VALUE_OVERFLOW;
        size_t bytes = (size_t)count * sizeof(*values);
        if (bytes > state->byte_limit - state->bytes) return XR_XIR_VALUE_MEMORY_LIMIT;
        int64_t *prepared = xr_malloc(bytes);
        if (!prepared) return XR_XIR_VALUE_OOM;
        memcpy(prepared, values, bytes);
        for (uint32_t n = 0; n < count; ++n)
            if (!(state->layout->view.slots[first + n].flags & XRT_MODULE_SLOT_MUTABLE))
                memcpy(module_payload(state, first + n), &prepared[n], sizeof(prepared[n]));
        xr_free(prepared);
    }
    for (uint32_t n = 0; n < count; ++n) {
        state->flags[first + n] = SLOT_CONSTRUCTED | SLOT_PUBLISHED;
        state->publication[state->publication_count++] = first + n;
    }
    state->remaining[module] -= count;
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_reference(XrtModuleState *state, uint32_t slot, XrtModuleRef **out) {
    if (!state || !out || *out || slot >= state->layout->view.slot_count ||
        !(state->flags[slot] & SLOT_PUBLISHED)) return XR_XIR_VALUE_BAD_ARGUMENT;
    *out = &state->published[slot];
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_borrow(const XrtModuleRef *reference, const void **out) {
    if (!out || !module_reference_valid(reference)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrtModuleState *state = reference->state;
    if ((state->flags[reference->slot] & reference->required_flags) != reference->required_flags)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *out = module_payload(state, reference->slot);
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_replace(XrtModuleRef *reference, const void *value) {
    if (!module_reference_valid(reference)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrtModuleState *state = reference->state;
    uint32_t slot = reference->slot;
    if (!(state->layout->view.slots[slot].flags & XRT_MODULE_SLOT_MUTABLE) ||
        (state->flags[slot] & reference->required_flags) != reference->required_flags)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrtModuleType *type = module_slot_type(state, slot);
    if (type->kind == XRT_MODULE_CELL_ARRAY_I64) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = type->copy(type, value, state->scratch);
    if (status != XR_XIR_VALUE_OK) return status;
    void *payload = module_payload(state, slot);
    type->drop(type, payload);
    if (type->size) { memcpy(payload, state->scratch, type->size); memset(state->scratch, 0, type->size); }
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_reference_cache(const XrtModuleRef *reference, XrtModuleAccess *out) {
    if (!out || !module_reference_valid(reference)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrtModuleState *state = reference->state;
    uint32_t slot = reference->slot;
    const XrtModuleType *type = module_slot_type(state, slot);
    *out = (XrtModuleAccess){.reference=(XrtModuleRef *)reference, .payload=module_payload(state, slot),
        .flags=&state->flags[slot], .required_flags=reference->required_flags,
        .size=type->size, .alignment=type->alignment, .kind=type->kind,
        .is_mutable=(state->layout->view.slots[slot].flags & XRT_MODULE_SLOT_MUTABLE) != 0};
    return XR_XIR_VALUE_OK;
}

XRAY_API XrtModuleStatus xrt_module_cache(const XrtModuleState *state, uint32_t slot, XrtModuleAccess *out) {
    if (!state || slot >= state->layout->view.slot_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    return xrt_module_reference_cache(&state->published[slot], out);
}

XRAY_API void xrt_module_require(XrtModuleStatus status, uint32_t source_id) {
    if (!status) return;
    XR_CHECK(status <= XR_XIR_VALUE_IO, "Module errors preserve a shared value status");
    xrt_value_failure((XrXirValueStatus)status, source_id);
}

XRAY_API_NORET void xrt_module_uninitialized(uint32_t source_id) {
    xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT, source_id);
}
