/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Literal provenance is temporary scalar construction data, not Map ownership. */
#include "shared/xr_utf8_core.h"

static bool map_literal_key(const XrtStringBacking *key) {
    const XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(XrtStringBacking), .alignment=_Alignof(XrtStringBacking),
        .capacity_offset=offsetof(XrtStringBacking, capacity), .stride=1};
    return key && xrt_managed_layout_matches(&key->managed, &spec) && key->capacity &&
        key->capacity <= SIZE_MAX - sizeof(*key) && key->length < key->capacity &&
        key->runes <= key->length && !key->bytes[key->length];
}
static bool map_literal_spread(const XrtMapBacking *map) {
    const size_t child = 0;
    const XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(XrtMapBacking), .alignment=_Alignof(XrtMapBacking),
        .capacity_offset=offsetof(XrtMapBacking, capacity), .stride=sizeof(XrtMapBackingRow),
        .tail_child_offsets=&child, .tail_child_count=1};
    if (!map || !xrt_managed_layout_matches(&map->managed, &spec) || !map_shape(map) ||
        map->capacity > (SIZE_MAX - sizeof(*map)) / sizeof(XrtMapBackingRow)) return false;
    for (size_t ordinal = 0; ordinal < map->length; ++ordinal) {
        size_t entry = map_order_entry(map, ordinal);
        if (entry == SIZE_MAX || !map_literal_key(map->rows[entry].key)) return false;
    }
    return true;
}
static XrtManagedHeader *map_literal_owner(const XrtMapLiteralOrigin *origin, const XrtMapLiteralInput *input) {
    return (XrtManagedHeader *)(origin->kind ? &input->source.spread->managed : &input->source.key->managed);
}
static void map_literal_drop_prefix(const XrtMapLiteral *literal, const XrtMapLiteralInput *inputs, size_t count) {
    while (count) {
        --count;
        xrt_managed_drop(map_literal_owner(&literal->items[count], &inputs[count]));
    }
}
XR_FUNCDEF void xrt_map_literal_inputs_drop(const XrtMapLiteral *literal, const XrtMapLiteralInput *inputs) {
    map_literal_drop_prefix(literal, inputs, literal->count);
}
/* This proves address-space representability, not that arbitrary pointer bytes
 * name a live array. The caller still supplies complete synchronous borrows. */
static bool map_literal_span(const void *base, size_t count, size_t stride) {
    if (count > SIZE_MAX / stride) return false;
    size_t bytes = count * stride;
    return (uintptr_t)base <= UINTPTR_MAX - bytes;
}
XR_FUNCDEF XrtManagedStatus xrt_map_literal_inputs_retain(const XrtMapLiteral *literal,
    const XrtMapLiteralInput *inputs, size_t *capacity) {
    if (!literal || !capacity || (literal->path_length != 0) != (literal->path != NULL) ||
        literal->path_length == SIZE_MAX || (literal->count && (!literal->items || !inputs))) return XRT_MANAGED_ARGUMENT;
    if (!map_literal_span(literal->items, literal->count, sizeof(*literal->items)) ||
        !map_literal_span(inputs, literal->count, sizeof(*inputs)) ||
        !map_literal_span(literal->path, literal->path_length, 1)) return XRT_MANAGED_OVERFLOW;
    if (literal->path_length &&
        (memchr(literal->path, 0, literal->path_length) ||
         xr_utf8_core_scan_strict((const uint8_t *)literal->path, literal->path_length).error != XR_UTF8_OK))
        return XRT_MANAGED_ARGUMENT;
    size_t entries = 0, reserved = 0;
    for (size_t n = 0; n < literal->count; ++n) {
        const XrtMapLiteralOrigin *origin = &literal->items[n];
        if (origin->kind > 1 || !origin->line || !origin->column) return XRT_MANAGED_ARGUMENT;
        size_t count = 1;
        if (origin->kind) {
            if (!map_literal_spread(inputs[n].source.spread)) return XRT_MANAGED_ARGUMENT;
            count = inputs[n].source.spread->length;
        } else if (!map_literal_key(inputs[n].source.key)) return XRT_MANAGED_ARGUMENT;
        if (count > SIZE_MAX - entries) return XRT_MANAGED_OVERFLOW;
        entries += count;
    }
    XrtManagedStatus status = map_capacity(0, entries, &reserved);
    if (status != XRT_MANAGED_OK) return status;
    for (size_t n = 0; n < literal->count; ++n) {
        if (!xrt_managed_retain(map_literal_owner(&literal->items[n], &inputs[n]))) {
            map_literal_drop_prefix(literal, inputs, n);
            return XRT_MANAGED_REFCOUNT;
        }
    }
    *capacity = reserved;
    return XRT_MANAGED_OK;
}

typedef struct MapLiteralConstruction {
    const XrtMapLiteral *literal;
    const XrtMapLiteralInput *inputs;
    XrtMapLiteralDuplicate duplicate;
    bool pinned;
} MapLiteralConstruction;

static XrtManagedStatus map_literal_entry(XrtMapBacking *map, const XrtStringBacking *key,
    int64_t value, bool explicit_entry, size_t ordinal, MapLiteralConstruction *construction) {
    MapProbe probe = {0};
    XrtManagedStatus status = map_probe(map, key, &probe);
    if (status != XRT_MANAGED_OK) return status;
    size_t entry = probe.found ? probe.entry : map->length;
    XR_CHECK(entry < map->capacity / 2, "literal input extent reserves every possible distinct key");
    /* Dense owners occupy at most half the rows. Their unused upper-half
     * entry_order scalars track first explicit ordinals until publication.
     * Spreads can overwrite values but cannot erase this separate provenance. */
    size_t *first = &map->rows[map->capacity / 2 + entry].entry_order;
    if (explicit_entry && *first) {
        construction->duplicate = (XrtMapLiteralDuplicate){*first - 1, ordinal};
        return XRT_MANAGED_DUPLICATE_KEY;
    }
    if (probe.found) map->rows[entry].value = value;
    else status = map_append(map, key, value, probe.hash);
    if (status == XRT_MANAGED_OK && explicit_entry) *first = ordinal + 1;
    return status;
}
static XrtManagedStatus map_literal_initialize(XrtManagedHeader *value, void *context) {
    MapLiteralConstruction *construction = context;
    XrtMapBacking *map = (XrtMapBacking *)value;
    for (size_t n = 0; n < construction->literal->count; ++n) {
        const XrtMapLiteralInput *input = &construction->inputs[n];
        if (!construction->literal->items[n].kind) {
            XrtManagedStatus status = map_literal_entry(map, input->source.key, input->value, true, n, construction);
            if (status != XRT_MANAGED_OK) return status;
        } else {
            const XrtMapBacking *source = input->source.spread;
            for (size_t ordinal = 0; ordinal < source->length; ++ordinal) {
                size_t entry = map_order_entry(source, ordinal);
                if (entry == SIZE_MAX) return XRT_MANAGED_ARGUMENT;
                XrtManagedStatus status = map_literal_entry(map, source->rows[entry].key,
                    source->rows[entry].value, false, n, construction);
                if (status != XRT_MANAGED_OK) return status;
            }
        }
    }
    for (size_t n = map->capacity / 2; n < map->capacity; ++n) map->rows[n].entry_order = 0;
    return XRT_MANAGED_OK;
}
static void map_literal_finish(void *context) {
    MapLiteralConstruction *construction = context;
    if (!construction->pinned) return;
    construction->pinned = false;
    xrt_map_literal_inputs_drop(construction->literal, construction->inputs);
}
XR_FUNCDEF XrtManagedStatus xrt_map_backing_literal_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtMapLiteral *literal, const XrtMapLiteralInput *inputs,
    XrtMapLiteralDuplicate *duplicate) {
    if (!types || !types->map || !duplicate) return XRT_MANAGED_ARGUMENT;
    size_t capacity = 0;
    XrtManagedStatus status = xrt_map_literal_inputs_retain(literal, inputs, &capacity);
    if (status != XRT_MANAGED_OK) return status;
    MapLiteralConstruction context = {.literal=literal, .inputs=inputs, .pinned=true};
    XrtManagedSlotConstruction construction = {map_literal_initialize, map_literal_finish, &context};
    status = xrt_managed_slot_construct(frame, slot, types->map,
        sizeof(XrtMapBacking) + capacity * sizeof(XrtMapBackingRow), &construction);
    /* A rejected slot entry ran no allocator callback, so the caller's
     * original borrows still own every pin that was not finished inside it. */
    map_literal_finish(&context);
    if (status == XRT_MANAGED_DUPLICATE_KEY) *duplicate = context.duplicate;
    return status;
}
