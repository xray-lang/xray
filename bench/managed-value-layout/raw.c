/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * raw.c - Bare C controls with matching fixed inputs and container algorithms
 *
 * KEY CONCEPT:
 *   Allocation, probing and key work are comparable without managed RC metadata.
 */
#include "raw.h"

typedef struct {
    uint64_t hash;
    XrMvlRawString *key;
    int64_t value;
} MvlRawSlot;

typedef struct {
    MvlRawSlot *slots;
    size_t length;
    size_t capacity;
} MvlRawMap;

static void mvl_raw_layout(void) {
    printf("XrMvlRawString size=%zu align=%zu length=%zu hash=%zu bytes=%zu\n",
           sizeof(XrMvlRawString), (size_t) _Alignof(XrMvlRawString),
           offsetof(XrMvlRawString, length), offsetof(XrMvlRawString, hash),
           offsetof(XrMvlRawString, bytes));
    printf("MvlRawSlot size=%zu align=%zu hash=%zu key=%zu value=%zu\n",
           sizeof(MvlRawSlot), (size_t) _Alignof(MvlRawSlot),
           offsetof(MvlRawSlot, hash), offsetof(MvlRawSlot, key), offsetof(MvlRawSlot, value));
    printf("MvlRawMap size=%zu align=%zu slots=%zu length=%zu capacity=%zu\n",
           sizeof(MvlRawMap), (size_t) _Alignof(MvlRawMap),
           offsetof(MvlRawMap, slots), offsetof(MvlRawMap, length), offsetof(MvlRawMap, capacity));
}

static bool mvl_raw_push(int64_t *result) {
    int64_t *data = NULL;
    size_t length = 0, capacity = 0;
    bool ok = true;
    for (int64_t i = 0; i < XR_MVL_PUSH_COUNT; ++i) {
        if (length == capacity) {
            size_t next;
            if (!xr_mvl_capacity(capacity, length + 1, sizeof(*data), &next)) {
                ok = false;
                break;
            }
            int64_t *replacement = xr_realloc(data, next * sizeof(*data));
            if (!replacement) {
                ok = false;
                break;
            }
            data = replacement;
            capacity = next;
        }
        data[length++] = i;
    }
    int64_t total = 0;
    if (ok)
        for (size_t i = 0; i < length; ++i)
            total += data[i];
    xr_free(data);
    *result = total;
    return ok;
}

static int64_t mvl_raw_record(void) {
    int64_t total = 0;
    for (int64_t i = 0; i < XR_MVL_RECORD_COUNT; ++i) {
        const struct { int64_t x, y; } pair = {i, i + 1};
        total += pair.x + pair.y;
    }
    return total;
}

static size_t mvl_raw_probe(const MvlRawMap *map, const XrMvlRawString *key) {
    size_t index = (size_t) key->hash & (map->capacity - 1);
    while (map->slots[index].key) {
        const MvlRawSlot *slot = &map->slots[index];
        if (slot->hash == key->hash && slot->key->length == key->length &&
            memcmp(slot->key->bytes, key->bytes, key->length) == 0)
            break;
        index = (index + 1) & (map->capacity - 1);
    }
    return index;
}

static bool mvl_raw_grow(MvlRawMap *map) {
    size_t capacity;
    if (!xr_mvl_capacity(map->capacity, map->capacity + 1, sizeof(MvlRawSlot), &capacity))
        return false;
    MvlRawSlot *slots = xr_malloc(capacity * sizeof(*slots));
    if (!slots)
        return false;
    memset(slots, 0, capacity * sizeof(*slots));
    MvlRawMap destination = {slots, 0, capacity};
    for (size_t i = 0; i < map->capacity; ++i)
        if (map->slots[i].key)
            slots[mvl_raw_probe(&destination, map->slots[i].key)] = map->slots[i];
    xr_free(map->slots);
    map->slots = slots;
    map->capacity = capacity;
    return true;
}

/* Success transfers the incoming key; an existing equal key keeps its storage. */
static bool mvl_raw_set(MvlRawMap *map, XrMvlRawString *key, int64_t value) {
    size_t index = mvl_raw_probe(map, key);
    if (map->slots[index].key) {
        map->slots[index].value = value;
        xr_free(key);
        return true;
    }
    if (!xr_mvl_map_room(map->length, map->capacity)) {
        if (!mvl_raw_grow(map))
            return false;
        index = mvl_raw_probe(map, key);
    }
    map->slots[index] = (MvlRawSlot) {key->hash, key, value};
    ++map->length;
    return true;
}

static bool mvl_raw_map(int64_t *result) {
    MvlRawMap map = {NULL, 0, 8};
    map.slots = xr_malloc(map.capacity * sizeof(*map.slots));
    if (!map.slots)
        return false;
    memset(map.slots, 0, map.capacity * sizeof(*map.slots));
    XrMvlRawString *seed = xr_mvl_raw_string_new("seed", 4);
    bool ok = seed != NULL;
    if (seed) {
        ok = mvl_raw_set(&map, seed, 0);
        if (!ok)
            xr_free(seed);
    }
    for (int64_t i = 0; ok && i < XR_MVL_MAP_COUNT; ++i) {
        char bytes[32];
        size_t length = xr_mvl_key_bytes((uint64_t) i, bytes);
        XrMvlRawString *key = xr_mvl_raw_string_new(bytes, length);
        if (!key) {
            ok = false;
            break;
        }
        ok = mvl_raw_set(&map, key, i);
        if (!ok)
            xr_free(key);
    }
    int64_t total = 0;
    for (int64_t i = 0; ok && i < XR_MVL_MAP_COUNT; ++i) {
        char bytes[32];
        size_t length = xr_mvl_key_bytes((uint64_t) i, bytes);
        XrMvlRawString *key = xr_mvl_raw_string_new(bytes, length);
        if (!key) {
            ok = false;
            break;
        }
        size_t index = mvl_raw_probe(&map, key);
        ok = map.slots[index].key != NULL;
        if (ok)
            total += map.slots[index].value;
        xr_free(key);
    }
    for (size_t i = 0; i < map.capacity; ++i)
        xr_free(map.slots[i].key);
    xr_free(map.slots);
    *result = total;
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: raw <push|record|map|layout>\n");
        return 2;
    }
    if (strcmp(argv[1], "layout") == 0) {
        mvl_raw_layout();
        return 0;
    }
    int64_t total = 0, expected;
    bool ok;
    if (strcmp(argv[1], "push") == 0) {
        expected = INT64_C(12499997500000);
        ok = mvl_raw_push(&total);
    } else if (strcmp(argv[1], "record") == 0) {
        expected = INT64_C(9000000000000);
        total = mvl_raw_record();
        ok = true;
    } else if (strcmp(argv[1], "map") == 0) {
        expected = INT64_C(19999900000);
        ok = mvl_raw_map(&total);
    } else {
        fprintf(stderr, "unknown workload: %s\n", argv[1]);
        return 2;
    }
    if (!ok || total != expected) {
        fprintf(stderr, "failed: value=%" PRId64 "\n", total);
        return 1;
    }
    printf("%" PRId64 "\n", total);
    return 0;
}
