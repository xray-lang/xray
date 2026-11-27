/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Literal provenance is temporary scalar construction data, not Map ownership. */
#include "shared/xr_utf8_core.h"

static bool map_literal_key(const XrtStringBacking *key) {
    return xrt_backing_string_matches((const XrtManagedHeader *)key);
}
static bool map_literal_spread(const XrtMapBacking *map, bool strings) {
    const size_t child = 0;
    const XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(XrtMapBacking), .alignment=_Alignof(XrtMapBacking),
        .capacity_offset=offsetof(XrtMapBacking, capacity), .stride=sizeof(XrtMapBackingRow),
        .tail_child_offsets=&child, .tail_child_count=1};
    if (!map || (strings ? !xrt_map_string_value_matches(&map->managed) :
                           !xrt_managed_layout_matches(&map->managed, &spec))) return false;
    if (!map_shape(map) || map->capacity > (SIZE_MAX - sizeof(*map)) / sizeof(XrtMapBackingRow))
        return false;
    for (size_t ordinal = 0; ordinal < map->length; ++ordinal) {
        size_t entry = map_order_entry(map, ordinal);
        if (entry == SIZE_MAX || !map_literal_key(map->rows[entry].key) ||
            (strings && !map_literal_key(map->rows[entry].string_value))) return false;
    }
    return true;
}
typedef struct MapLiteralInputs {
    const XrtMapLiteralInput *scalar;
    const XrtMapStringLiteralInput *strings;
    bool uses_strings;
} MapLiteralInputs;

static XrtMapLiteralSource map_literal_source(MapLiteralInputs inputs, size_t n) {
    return inputs.uses_strings ? inputs.strings[n].source : inputs.scalar[n].source;
}
static XrtManagedHeader *map_literal_owner(const XrtMapLiteralOrigin *origin, XrtMapLiteralSource source) {
    return (XrtManagedHeader *)(origin->kind ? &source.spread->managed : &source.key->managed);
}
static void map_literal_drop_prefix(const XrtMapLiteral *literal, MapLiteralInputs inputs, size_t count) {
    while (count) {
        --count;
        if (inputs.uses_strings && !literal->items[count].kind)
            xrt_managed_drop(&inputs.strings[count].value->managed);
        xrt_managed_drop(map_literal_owner(&literal->items[count], map_literal_source(inputs, count)));
    }
}
XR_FUNCDEF void xrt_map_literal_inputs_drop(const XrtMapLiteral *literal, const XrtMapLiteralInput *inputs) {
    map_literal_drop_prefix(literal, (MapLiteralInputs){.scalar=inputs}, literal->count);
}
/* This proves address-space representability, not that arbitrary pointer bytes
 * name a live array. The caller still supplies complete synchronous borrows. */
static bool map_literal_span(const void *base, size_t count, size_t stride) {
    if (count > SIZE_MAX / stride) return false;
    size_t bytes = count * stride;
    return (uintptr_t)base <= UINTPTR_MAX - bytes;
}
static XrtManagedStatus map_literal_inputs_pin(const XrtMapLiteral *literal,
    MapLiteralInputs inputs, size_t *capacity) {
    const void *base = inputs.uses_strings ? (const void *)inputs.strings : (const void *)inputs.scalar;
    size_t stride = inputs.uses_strings ? sizeof(*inputs.strings) : sizeof(*inputs.scalar);
    if (!literal || !capacity || (literal->path_length != 0) != (literal->path != NULL) ||
        literal->path_length == SIZE_MAX || (literal->count && (!literal->items || !base))) return XRT_MANAGED_ARGUMENT;
    if (!map_literal_span(literal->items, literal->count, sizeof(*literal->items)) ||
        !map_literal_span(base, literal->count, stride) ||
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
        XrtMapLiteralSource source = map_literal_source(inputs, n);
        if (origin->kind) {
            if (!map_literal_spread(source.spread, inputs.uses_strings)) return XRT_MANAGED_ARGUMENT;
            count = source.spread->length;
        } else if (!map_literal_key(source.key) ||
            (inputs.uses_strings && !map_literal_key(inputs.strings[n].value))) return XRT_MANAGED_ARGUMENT;
        if (count > SIZE_MAX - entries) return XRT_MANAGED_OVERFLOW;
        entries += count;
    }
    XrtManagedStatus status = map_capacity(0, entries, &reserved);
    if (status != XRT_MANAGED_OK) return status;
    for (size_t n = 0; n < literal->count; ++n) {
        XrtManagedHeader *owner = map_literal_owner(&literal->items[n], map_literal_source(inputs, n));
        if (!xrt_managed_retain(owner)) {
            map_literal_drop_prefix(literal, inputs, n);
            return XRT_MANAGED_REFCOUNT;
        }
        if (inputs.uses_strings && !literal->items[n].kind &&
            !xrt_managed_retain(&inputs.strings[n].value->managed)) {
            xrt_managed_drop(owner);
            map_literal_drop_prefix(literal, inputs, n);
            return XRT_MANAGED_REFCOUNT;
        }
    }
    *capacity = reserved;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_map_literal_inputs_retain(const XrtMapLiteral *literal,
    const XrtMapLiteralInput *inputs, size_t *capacity) {
    return map_literal_inputs_pin(literal, (MapLiteralInputs){.scalar=inputs}, capacity);
}

XR_FUNCDEF XrtManagedStatus xrt_map_string_literal_inputs_retain(const XrtMapLiteral *literal,
    const XrtMapStringLiteralInput *inputs, size_t *capacity) {
    return map_literal_inputs_pin(literal, (MapLiteralInputs){.strings=inputs,.uses_strings=true}, capacity);
}
XR_FUNCDEF void xrt_map_string_literal_inputs_drop(const XrtMapLiteral *literal, const XrtMapStringLiteralInput *inputs) {
    map_literal_drop_prefix(literal, (MapLiteralInputs){.strings=inputs,.uses_strings=true}, literal->count);
}

typedef struct MapLiteralConstruction {
    const XrtMapLiteral *literal;
    MapLiteralInputs inputs;
    XrtMapLiteralDuplicate duplicate;
    bool pinned;
} MapLiteralConstruction;

static XrtManagedStatus map_literal_entry(XrtMapBacking *map, const XrtStringBacking *key,
    int64_t value, XrtStringBacking *string_value, bool explicit_entry,
    size_t ordinal, MapLiteralConstruction *construction) {
    MapProbe probe;
    XrtManagedStatus status = map_probe(map, key, &probe);
    if (status != XRT_MANAGED_OK) return status;
    size_t entry = probe.entry_id ? probe.entry_id - 1 : map->length;
    XR_CHECK(entry < map->capacity / 2, "literal input extent reserves every possible distinct key");
    /* Dense owners occupy at most half the rows. Their unused upper-half
     * entry_order scalars track first explicit ordinals until publication.
     * Spreads can overwrite values but cannot erase this separate provenance. */
    size_t *first = &map->rows[map->capacity / 2 + entry].entry_order;
    if (explicit_entry && *first) {
        construction->duplicate = (XrtMapLiteralDuplicate){*first - 1, ordinal};
        return XRT_MANAGED_DUPLICATE_KEY;
    }
    if (construction->inputs.uses_strings) status = map_string_publish(map, key, string_value, probe);
    else if (probe.entry_id) map->rows[entry].value = value;
    else status = map_append(map, key, value, probe.hash);
    if (status == XRT_MANAGED_OK && explicit_entry) *first = ordinal + 1;
    return status;
}
static XrtManagedStatus map_literal_initialize(XrtManagedHeader *value, void *context) {
    MapLiteralConstruction *construction = context;
    XrtMapBacking *map = (XrtMapBacking *)value;
    for (size_t n = 0; n < construction->literal->count; ++n) {
        MapLiteralInputs inputs = construction->inputs;
        XrtMapLiteralSource original = map_literal_source(inputs, n);
        if (!construction->literal->items[n].kind) {
            XrtManagedStatus status = map_literal_entry(map, original.key,
                inputs.uses_strings ? 0 : inputs.scalar[n].value,
                inputs.uses_strings ? inputs.strings[n].value : NULL, true, n, construction);
            if (status != XRT_MANAGED_OK) return status;
        } else {
            const XrtMapBacking *source = original.spread;
            for (size_t ordinal = 0; ordinal < source->length; ++ordinal) {
                size_t entry = map_order_entry(source, ordinal);
                if (entry == SIZE_MAX) return XRT_MANAGED_ARGUMENT;
                const XrtMapBackingRow *row = &source->rows[entry];
                XrtManagedStatus status = map_literal_entry(map, row->key,
                    inputs.uses_strings ? 0 : row->value, inputs.uses_strings ? row->string_value : NULL,
                    false, n, construction);
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
    map_literal_drop_prefix(construction->literal, construction->inputs, construction->literal->count);
}
static XrtManagedStatus map_literal_create(XrtManagedFrame *frame, size_t slot,
    XrtManagedOps *ops, const XrtMapLiteral *literal, MapLiteralInputs inputs,
    XrtMapLiteralDuplicate *duplicate) {
    if (!ops || !duplicate) return XRT_MANAGED_ARGUMENT;
    size_t capacity = 0;
    XrtManagedStatus status = map_literal_inputs_pin(literal, inputs, &capacity);
    if (status != XRT_MANAGED_OK) return status;
    MapLiteralConstruction context = {.literal=literal, .inputs=inputs, .pinned=true};
    XrtManagedSlotConstruction construction = {map_literal_initialize, map_literal_finish, &context};
    status = xrt_managed_slot_construct(frame, slot, ops,
        sizeof(XrtMapBacking) + capacity * sizeof(XrtMapBackingRow), &construction);
    /* A rejected slot entry ran no allocator callback, so the caller's
     * original borrows still own every pin that was not finished inside it. */
    map_literal_finish(&context);
    if (status == XRT_MANAGED_DUPLICATE_KEY) *duplicate = context.duplicate;
    return status;
}

XR_FUNCDEF XrtManagedStatus xrt_map_backing_literal_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtMapLiteral *literal, const XrtMapLiteralInput *inputs,
    XrtMapLiteralDuplicate *duplicate) {
    if (!types || xrt_map_string_ops_match(types->map)) return XRT_MANAGED_ARGUMENT;
    return map_literal_create(frame, slot, types->map, literal,
        (MapLiteralInputs){.scalar=inputs}, duplicate);
}
XR_FUNCDEF XrtManagedStatus xrt_map_string_literal_create(XrtManagedFrame *frame, size_t slot,
    XrtManagedOps *ops, const XrtMapLiteral *literal, const XrtMapStringLiteralInput *inputs,
    XrtMapLiteralDuplicate *duplicate) {
    if (!xrt_map_string_ops_match(ops)) return XRT_MANAGED_ARGUMENT;
    return map_literal_create(frame, slot, ops, literal,
        (MapLiteralInputs){.strings=inputs,.uses_strings=true}, duplicate);
}
