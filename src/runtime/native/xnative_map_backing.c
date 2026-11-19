/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_map_backing.c - Ordered fixed Map transactions over real key owners
 *
 * KEY CONCEPT:
 *   Dense key owners, linear-probe buckets and Fenwick order ranks share one
 *   allocation. Every unpublished candidate is retired by its full capacity.
 */
#include "xnative_map_backing.h"
#include "xnative_array_backing.h"
#include "xnative_string_backing.h"
#include "base/xchecks.h"
#include "base/xhash.h"
#include <string.h>

typedef struct MapProbe {
    uint64_t hash;
    size_t bucket, entry;
    bool found;
} MapProbe;
typedef struct MapEdit {
    MapProbe probe;
    size_t length, capacity;
    bool noop;
} MapEdit;
typedef struct MapDelete {
    size_t end, last_bucket;
} MapDelete;
typedef struct MapDestination {
    XrtManagedFrame *frame;
    size_t slot;
    const XrtBackingTypes *types;
    const XrtManagedSlotReservation *reservation;
} MapDestination;

static bool map_shape(const XrtMapBacking *map) {
    return map && ((!map->capacity && !map->length && !map->order_count) ||
        (map->capacity >= 8 && !(map->capacity & (map->capacity - 1)) &&
         map->length <= map->capacity / 2 && map->length <= map->order_count && map->order_count <= map->capacity));
}
static XrtManagedStatus map_snapshot_fill(XrtArrayBacking *array, const XrtMapBacking *map, bool keys) {
    size_t count = 0;
    for (size_t i = 0; i < map->order_count; ++i) {
        size_t ordinal = map->rows[i].order;
        if (!ordinal) continue;
        if (ordinal > map->length || count == array->capacity) return XRT_MANAGED_ARGUMENT;
        const XrtMapBackingRow *entry = &map->rows[ordinal - 1];
        if (!entry->key || entry->entry_order != i) return XRT_MANAGED_ARGUMENT;
        if (keys) {
            XrtManagedHeader *key = &entry->key->managed;
            if (!xrt_managed_retain(key)) return XRT_MANAGED_REFCOUNT;
            memcpy(array->elements + count * sizeof(key), &key, sizeof(key));
        } else memcpy(array->elements + count * sizeof(entry->value), &entry->value, sizeof(entry->value));
        ++count;
    }
    if (count != array->capacity) return XRT_MANAGED_ARGUMENT;
    array->length = count;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_map_backing_snapshot_held(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtMapBacking *map, bool keys) {
    XrtBackingArrayKind kind = keys ? XRT_BACKING_ARRAY_STRING : XRT_BACKING_ARRAY_I64;
    if (!map_shape(map) || !types || !types->arrays[kind]) return XRT_MANAGED_ARGUMENT;
    size_t stride = xrt_backing_array_stride(kind);
    if (map->length > (SIZE_MAX - sizeof(XrtArrayBacking)) / stride) return XRT_MANAGED_OVERFLOW;
    XrtManagedHeader *pending = NULL;
    size_t bytes = sizeof(XrtArrayBacking) + map->length * stride;
    XrtManagedStatus status = xrt_managed_slot_prepare(frame, slot, types->arrays[kind], bytes, &pending);
    if (status == XRT_MANAGED_OK) {
        status = map_snapshot_fill((XrtArrayBacking *)pending, map, keys);
        if (status == XRT_MANAGED_OK) status = xrt_managed_slot_commit(frame, slot);
        if (status != XRT_MANAGED_OK)
            XR_CHECK(xrt_managed_slot_abort(frame, slot) == XRT_MANAGED_OK,
                "a failed Map snapshot retires every pending key owner");
    }
    return status;
}
static XrtManagedStatus map_capacity(size_t current, size_t required, size_t *output) {
    size_t maximum = (SIZE_MAX - sizeof(XrtMapBacking)) / sizeof(XrtMapBackingRow);
    if (required > INT64_MAX || required > maximum / 2) return XRT_MANAGED_OVERFLOW;
    size_t capacity = current;
    if (required > capacity / 2) {
        capacity = 8;
        while (required > capacity / 2) {
            if (capacity > maximum / 2) return XRT_MANAGED_OVERFLOW;
            capacity *= 2;
        }
    }
    if (capacity > maximum) return XRT_MANAGED_OVERFLOW;
    *output = capacity;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus map_probe(const XrtMapBacking *map, const XrtStringBacking *key, MapProbe *output) {
    if (!key) return XRT_MANAGED_ARGUMENT;
    MapProbe probe = {.hash=xr_hash_bytes64(key->bytes, key->length)};
    if (!map->capacity) { *output = probe; return XRT_MANAGED_OK; }
    size_t mask = map->capacity - 1;
    probe.bucket = (size_t)probe.hash & mask;
    for (size_t n = 0; n < map->capacity; ++n, probe.bucket = (probe.bucket + 1) & mask) {
        size_t index = map->rows[probe.bucket].bucket;
        if (!index) { *output = probe; return XRT_MANAGED_OK; }
        if (index > map->length) return XRT_MANAGED_ARGUMENT;
        probe.entry = index - 1;
        const XrtMapBackingRow *entry = &map->rows[probe.entry];
        if (!entry->key) return XRT_MANAGED_ARGUMENT;
        if (entry->hash == probe.hash && entry->key->length == key->length &&
            (!key->length || !memcmp(entry->key->bytes, key->bytes, key->length))) {
            probe.found = true; *output = probe; return XRT_MANAGED_OK;
        }
    }
    return XRT_MANAGED_ARGUMENT;
}
static size_t map_order_entry(const XrtMapBacking *map, size_t ordinal) {
    size_t position = 0;
    for (size_t step = map->capacity; step; step >>= 1) {
        size_t next = position + step;
        if (next <= map->capacity && map->rows[next - 1].rank <= ordinal) {
            ordinal -= map->rows[next - 1].rank;
            position = next;
        }
    }
    if (position >= map->order_count) return SIZE_MAX;
    size_t entry = map->rows[position].order;
    return entry && entry <= map->length ? entry - 1 : SIZE_MAX;
}
enum MapFormatNode { MAP_FORMAT_ROOT, MAP_FORMAT_KEY, MAP_FORMAT_VALUE };

static int map_format_read(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    switch (node.type) {
    case MAP_FORMAT_ROOT:
        view->kind = XR_VALUE_FORMAT_MAP;
        view->unsigned_value = ((const XrtMapBacking *)context)->length;
        return 1;
    case MAP_FORMAT_KEY: {
        const XrtStringBacking *key = node.value;
        view->kind = XR_VALUE_FORMAT_BYTES;
        view->bytes = (const unsigned char *)key->bytes;
        view->size = key->length;
        view->quote = '"';
        return 1;
    }
    case MAP_FORMAT_VALUE:
        view->kind = XR_VALUE_FORMAT_SIGNED;
        view->signed_value = *(const int64_t *)node.value;
        return 1;
    default: return 0;
    }
}

static int map_format_child(const void *context, XrValueFormatNode node, uint32_t index, XrValueFormatNode *child) {
    const XrtMapBacking *map = context;
    if (node.type != MAP_FORMAT_ROOT || index / 2 >= map->length) return 0;
    size_t position = map_order_entry(map, index / 2);
    if (position == SIZE_MAX) return 0;
    const XrtMapBackingRow *entry = &map->rows[position];
    if (!entry->key || entry->entry_order >= map->order_count ||
        map->rows[entry->entry_order].order != position + 1) return 0;
    *child = index % 2 ? (XrValueFormatNode){&entry->value, MAP_FORMAT_VALUE} :
        (XrValueFormatNode){entry->key, MAP_FORMAT_KEY};
    return 1;
}

XR_FUNCDEF XrtManagedStatus xrt_map_backing_display_held(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtMapBacking *map) {
    if (!map_shape(map)) return XRT_MANAGED_ARGUMENT;
    if (map->length > INT64_MAX) return XRT_MANAGED_OVERFLOW;
    XrValueFormatReader reader = {map, map_format_read, map_format_child};
    return xrt_string_backing_format_create(frame, slot, types, reader, (XrValueFormatNode){map, MAP_FORMAT_ROOT});
}
static void map_order_compact(XrtMapBacking *map) {
    size_t count = 0;
    for (size_t i = 0; i < map->order_count; ++i) {
        size_t entry = map->rows[i].order;
        if (!entry) continue;
        map->rows[count].order = entry;
        map->rows[entry - 1].entry_order = count++;
    }
    XR_CHECK(count == map->length, "order compaction preserves every dense key owner");
    for (size_t i = count; i < map->capacity; ++i) map->rows[i].order = 0;
    for (size_t i = 0; i < map->capacity; ++i) map->rows[i].rank = i < count;
    for (size_t i = 0; i < map->capacity; ++i) {
        size_t parent = i | (i + 1);
        if (parent < map->capacity) map->rows[parent].rank += map->rows[i].rank;
    }
    map->order_count = count;
}
static void map_order_append(XrtMapBacking *map, size_t entry) {
    if (map->order_count == map->capacity) map_order_compact(map);
    size_t position = map->order_count++;
    XR_CHECK(position < map->capacity, "half load reserves a fresh insertion position");
    map->rows[position].order = entry + 1;
    map->rows[entry].entry_order = position;
    for (size_t p = position; p < map->capacity; p |= p + 1) ++map->rows[p].rank;
}
static void map_publish(XrtMapBacking *map, XrtStringBacking *owned, int64_t value, uint64_t hash, size_t bucket) {
    size_t entry = map->length;
    XR_CHECK(entry < map->capacity / 2 && !map->rows[entry].key, "new entry consumes an empty owned tail slot");
    map->rows[entry].key = owned;
    map->rows[entry].value = value;
    map->rows[entry].hash = hash;
    map->rows[bucket].bucket = entry + 1;
    map_order_append(map, entry);
    ++map->length;
}
static XrtManagedStatus map_append(XrtMapBacking *map, const XrtStringBacking *key, int64_t value, uint64_t hash) {
    size_t mask = map->capacity - 1, bucket = (size_t)hash & mask;
    for (size_t n = 0; n < map->capacity; ++n, bucket = (bucket + 1) & mask) {
        if (map->rows[bucket].bucket) continue;
        XrtStringBacking *owned = (XrtStringBacking *)key;
        if (!owned) return XRT_MANAGED_ARGUMENT;
        if (!xrt_managed_retain(&owned->managed)) return XRT_MANAGED_REFCOUNT;
        map_publish(map, owned, value, hash, bucket);
        return XRT_MANAGED_OK;
    }
    return XRT_MANAGED_ARGUMENT;
}
static XrtManagedStatus map_prepare(const MapDestination *destination, size_t capacity, XrtMapBacking **output) {
    const XrtBackingTypes *types = destination->types;
    if (!types || !types->map) return XRT_MANAGED_ARGUMENT;
    XrtManagedHeader *payload = NULL;
    size_t bytes = sizeof(XrtMapBacking) + capacity * sizeof(XrtMapBackingRow);
    XrtManagedStatus status = destination->reservation ?
        xrt_managed_slot_reserved_prepare(destination->reservation, types->map, bytes, &payload) :
        xrt_managed_slot_prepare(destination->frame, destination->slot, types->map, bytes, &payload);
    if (status == XRT_MANAGED_OK) *output = (XrtMapBacking *)payload;
    return status;
}
static XrtManagedStatus map_finish(const MapDestination *destination, XrtManagedStatus status) {
    const XrtManagedSlotReservation *reservation = destination->reservation;
    if (status == XRT_MANAGED_OK) status = reservation ? xrt_managed_slot_reserved_commit(reservation) :
        xrt_managed_slot_commit(destination->frame, destination->slot);
    if (status != XRT_MANAGED_OK) {
        XrtManagedStatus cleanup = reservation ? xrt_managed_slot_reserved_abort(reservation) :
            xrt_managed_slot_abort(destination->frame, destination->slot);
        XR_CHECK(cleanup == XRT_MANAGED_OK, "only this call's prepared Map is aborted");
    }
    return status;
}
static XrtManagedStatus map_edit(const XrtMapBacking *map, const XrtMapUpdate *update, MapEdit *output) {
    if (!map_shape(map) || !update) return XRT_MANAGED_ARGUMENT;
    MapEdit edit = {.length=map->length, .capacity=map->capacity};
    if (update->operation == XRT_MAP_UPDATE_CLEAR) {
        edit.noop = !map->length; edit.length = edit.capacity = 0;
    } else if (update->operation == XRT_MAP_UPDATE_SET || update->operation == XRT_MAP_UPDATE_REMOVE) {
        XrtManagedStatus status = map_probe(map, update->key, &edit.probe);
        if (status != XRT_MANAGED_OK) return status;
        if (update->operation == XRT_MAP_UPDATE_SET) {
            if (!edit.probe.found) ++edit.length;
            status = map_capacity(map->capacity, edit.length, &edit.capacity);
            if (status != XRT_MANAGED_OK) return status;
        } else {
            edit.noop = !edit.probe.found;
            if (edit.probe.found) --edit.length;
        }
    } else return XRT_MANAGED_ARGUMENT;
    *output = edit;
    return XRT_MANAGED_OK;
}
static XrtManagedStatus map_delete_preflight(const XrtMapBacking *map, MapProbe probe, MapDelete *output) {
    MapDelete plan = {0};
    size_t last = map->length - 1, mask = map->capacity - 1;
    size_t position = map->rows[probe.entry].entry_order;
    if (position >= map->order_count || map->rows[position].order != probe.entry + 1) return XRT_MANAGED_ARGUMENT;
    for (size_t p = position; p < map->capacity; p |= p + 1)
        if (!map->rows[p].rank || map->rows[p].rank > map->length) return XRT_MANAGED_ARGUMENT;
    if (probe.entry != last) {
        size_t bucket = (size_t)map->rows[last].hash & mask;
        bool found = false;
        for (size_t n = 0; n < map->capacity; ++n, bucket = (bucket + 1) & mask) {
            size_t entry = map->rows[bucket].bucket;
            if (!entry || entry > map->length) return XRT_MANAGED_ARGUMENT;
            if (entry == last + 1) { plan.last_bucket = bucket; found = true; break; }
        }
        if (!found) return XRT_MANAGED_ARGUMENT;
    }
    size_t bucket = (probe.bucket + 1) & mask;
    for (size_t n = 0; n < map->capacity; ++n, bucket = (bucket + 1) & mask) {
        if (!map->rows[bucket].bucket) { plan.end = bucket; *output = plan; return XRT_MANAGED_OK; }
        if (map->rows[bucket].bucket > map->length) return XRT_MANAGED_ARGUMENT;
    }
    return XRT_MANAGED_ARGUMENT;
}
static XrtStringBacking *map_delete_publish(XrtMapBacking *map, MapProbe probe, MapDelete plan) {
    size_t last = map->length - 1, mask = map->capacity - 1, hole = probe.bucket;
    XrtStringBacking *owned = map->rows[probe.entry].key;
    size_t position = map->rows[probe.entry].entry_order;
    map->rows[position].order = 0;
    for (size_t p = position; p < map->capacity; p |= p + 1) --map->rows[p].rank;
    for (size_t bucket = (hole + 1) & mask; bucket != plan.end; bucket = (bucket + 1) & mask) {
        size_t entry = map->rows[bucket].bucket - 1;
        size_t home = (size_t)map->rows[entry].hash & mask;
        if (((bucket - home) & mask) >= ((bucket - hole) & mask)) {
            map->rows[hole].bucket = map->rows[bucket].bucket;
            if (entry == last) plan.last_bucket = hole;
            hole = bucket;
        }
    }
    map->rows[hole].bucket = 0;
    if (probe.entry != last) {
        // Move only entry fields; the other columns index physical row positions.
        map->rows[probe.entry].key = map->rows[last].key;
        map->rows[probe.entry].value = map->rows[last].value;
        map->rows[probe.entry].hash = map->rows[last].hash;
        map->rows[probe.entry].entry_order = map->rows[last].entry_order;
        map->rows[plan.last_bucket].bucket = probe.entry + 1;
        map->rows[map->rows[probe.entry].entry_order].order = probe.entry + 1;
    }
    map->rows[last].key = NULL;
    map->rows[last].value = 0; map->rows[last].hash = 0; map->rows[last].entry_order = 0;
    if (!--map->length) map->order_count = 0;
    return owned;
}
static void map_clear_publish(XrtMapBacking *map) {
    size_t length = map->length;
    map->length = map->order_count = 0;
    for (size_t i = 0; i < map->capacity; ++i)
        map->rows[i].bucket = map->rows[i].order = map->rows[i].rank = 0;
    for (size_t i = 0; i < length; ++i) {
        XrtStringBacking *owned = map->rows[i].key;
        map->rows[i].key = NULL;
        map->rows[i].value = 0; map->rows[i].hash = 0; map->rows[i].entry_order = 0;
        xrt_managed_drop(&owned->managed);
    }
}
XR_FUNCDEF XrtManagedStatus xrt_map_backing_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, XrtStringBacking *const *keys, const int64_t *values, size_t count) {
    if (count && (!keys || !values)) return XRT_MANAGED_ARGUMENT;
    size_t capacity = 0;
    XrtManagedStatus status = map_capacity(0, count, &capacity);
    if (status != XRT_MANAGED_OK) return status;
    for (size_t i = 0; i < count; ++i) if (!keys[i]) return XRT_MANAGED_ARGUMENT;
    XrtMapBacking *map = NULL;
    MapDestination destination = {frame, slot, types, NULL};
    status = map_prepare(&destination, capacity, &map);
    if (status != XRT_MANAGED_OK) return status;
    for (size_t i = 0; i < count; ++i) {
        MapProbe probe = {0};
        status = map_probe(map, keys[i], &probe);
        if (status != XRT_MANAGED_OK) break;
        if (probe.found) map->rows[probe.entry].value = values[i];
        else status = map_append(map, keys[i], values[i], probe.hash);
        if (status != XRT_MANAGED_OK) break;
    }
    return map_finish(&destination, status);
}
#include "xnative_map_literal.inc.c"
XR_FUNCDEF XrtManagedStatus xrt_map_backing_get(const XrtMapBacking *map,
    const XrtStringBacking *key, int64_t *value, bool *found) {
    if (!map_shape(map) || !value || !found) return XRT_MANAGED_ARGUMENT;
    MapProbe probe = {0};
    XrtManagedStatus status = map_probe(map, key, &probe);
    if (status != XRT_MANAGED_OK) return status;
    *value = probe.found ? map->rows[probe.entry].value : 0;
    *found = probe.found;
    return XRT_MANAGED_OK;
}
XR_FUNCDEF XrtManagedStatus xrt_map_backing_try_update(XrtMapBacking *map,
    const XrtMapUpdate *update, bool *applied, bool *removed) {
    if (!applied || !removed) return XRT_MANAGED_ARGUMENT;
    *applied = false;
    MapEdit edit = {0};
    XrtManagedStatus status = map_edit(map, update, &edit);
    if (status != XRT_MANAGED_OK) return status;
    if (edit.noop) { *applied = true; *removed = false; return XRT_MANAGED_OK; }
    int32_t references = xr_signed_rc_read(&map->managed.references);
    if ((references != 1 && references != -1) || edit.capacity > map->capacity) return XRT_MANAGED_OK;
    MapDelete deletion = {0};
    if (update->operation == XRT_MAP_UPDATE_REMOVE) {
        status = map_delete_preflight(map, edit.probe, &deletion);
        if (status != XRT_MANAGED_OK) return status;
    }
    XrtStringBacking *owned = NULL;
    if (update->operation == XRT_MAP_UPDATE_SET && !edit.probe.found) {
        owned = (XrtStringBacking *)update->key;
        if (!xrt_managed_retain(&owned->managed)) return XRT_MANAGED_REFCOUNT;
    }
    if (!xr_signed_rc_unique(&map->managed.references)) {
        if (owned) xrt_managed_drop(&owned->managed);
        return XRT_MANAGED_OK;
    }
    if (update->operation == XRT_MAP_UPDATE_SET) {
        if (edit.probe.found) map->rows[edit.probe.entry].value = update->value;
        else map_publish(map, owned, update->value, edit.probe.hash, edit.probe.bucket);
    } else if (update->operation == XRT_MAP_UPDATE_REMOVE) owned = map_delete_publish(map, edit.probe, deletion);
    else map_clear_publish(map);
    *applied = true; *removed = update->operation == XRT_MAP_UPDATE_REMOVE;
    if (owned && update->operation == XRT_MAP_UPDATE_REMOVE) xrt_managed_drop(&owned->managed);
    return XRT_MANAGED_OK;
}
static XrtManagedStatus map_update_create(const MapDestination *destination,
    const XrtMapBacking *source, const XrtMapUpdate *update, bool *removed) {
    if (!removed) return XRT_MANAGED_ARGUMENT;
    MapEdit edit = {0};
    XrtManagedStatus status = map_edit(source, update, &edit);
    if (status != XRT_MANAGED_OK) return status;
    XrtMapBacking *map = NULL;
    status = map_prepare(destination, edit.capacity, &map);
    if (status != XRT_MANAGED_OK) return status;
    if (update->operation != XRT_MAP_UPDATE_CLEAR) {
        for (size_t ordinal = 0; ordinal < source->length; ++ordinal) {
            size_t entry = map_order_entry(source, ordinal);
            if (entry == SIZE_MAX) { status = XRT_MANAGED_ARGUMENT; break; }
            if (update->operation == XRT_MAP_UPDATE_REMOVE && edit.probe.found && entry == edit.probe.entry) continue;
            int64_t value = update->operation == XRT_MAP_UPDATE_SET && edit.probe.found && entry == edit.probe.entry ?
                update->value : source->rows[entry].value;
            status = map_append(map, source->rows[entry].key, value, source->rows[entry].hash);
            if (status != XRT_MANAGED_OK) break;
        }
        if (status == XRT_MANAGED_OK && update->operation == XRT_MAP_UPDATE_SET && !edit.probe.found)
            status = map_append(map, update->key, update->value, edit.probe.hash);
    }
    bool did_remove = update->operation == XRT_MAP_UPDATE_REMOVE && edit.probe.found;
    status = map_finish(destination, status);
    if (status == XRT_MANAGED_OK) *removed = did_remove;
    return status;
}
XR_FUNCDEF XrtManagedStatus xrt_map_backing_update_create(XrtManagedFrame *frame, size_t slot,
    const XrtBackingTypes *types, const XrtMapBacking *source, const XrtMapUpdate *update, bool *removed) {
    MapDestination destination = {frame, slot, types, NULL};
    return map_update_create(&destination, source, update, removed);
}
typedef struct MapBindingUpdate {
    const XrtMapUpdate *update;
    XrtMapBacking *source;
    bool applied, removed;
} MapBindingUpdate;
static XrtManagedStatus map_binding_try(XrtManagedHeader *value, void *context) {
    MapBindingUpdate *binding = context;
    binding->source = (XrtMapBacking *)value;
    return xrt_map_backing_try_update(binding->source, binding->update, &binding->applied, &binding->removed);
}
XR_FUNCDEF XrtManagedStatus xrt_map_update_place(XrtPlace binding, XrtManagedFrame *frame, size_t workspace,
    const XrtBackingTypes *types, const XrtMapUpdate *update, bool *removed) {
    if (!types || !types->map || !update || !removed) return XRT_MANAGED_ARGUMENT;
    XrtPlaceReplacement replacement = {0};
    XrtManagedStatus status = xrt_place_replacement_begin(binding, frame, workspace, &replacement);
    if (status != XRT_MANAGED_OK) return status;
    MapBindingUpdate edit = {.update=update};
    status = xrt_managed_slot_reserved_apply(&replacement, map_binding_try, &edit);
    if (status == XRT_MANAGED_OK && !edit.applied) {
        MapDestination destination = {frame, workspace, types, &replacement};
        status = map_update_create(&destination, edit.source, update, &edit.removed);
        if (status == XRT_MANAGED_OK) status = xrt_place_replacement_publish(&replacement);
    }
    XR_CHECK(xrt_place_replacement_end(&replacement) == XRT_MANAGED_OK,
        "every registered Map update releases its actual domains after cleanup");
    if (status == XRT_MANAGED_OK) *removed = edit.removed;
    return status;
}
