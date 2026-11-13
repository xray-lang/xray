/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_hash_order.inc.c - Stable insertion positions over dense physical owners
 *
 * KEY CONCEPT:
 *   Deletion clears one order position and moves at most one physical owner.
 *   A Fenwick index selects live ordinals without scanning deleted prefixes.
 */
static uint64_t *hash_order(const XirHash *map) {
    return (uint64_t *)(map->data + map->capacity * (sizeof(uint64_t) + map->stride));
}
static uint64_t *hash_order_tree(const XirHash *map) {
    return hash_order(map) + map->capacity;
}
static size_t hash_order_work(size_t capacity) {
    size_t work = 1;
    for (; capacity; capacity >>= 1) ++work;
    return work;
}
static size_t hash_entry_order(const XirHash *map, size_t entry) {
    uint64_t order = 0;
    memcpy(&order, hash_entry(map, entry) + sizeof(uint64_t), sizeof(order));
    return (size_t)order;
}
static void hash_entry_set_order(XirHash *map, size_t entry, size_t order) {
    uint64_t value = order;
    memcpy(hash_entry(map, entry) + sizeof(uint64_t), &value, sizeof(value));
}
static size_t hash_order_position(const XirHash *map, size_t ordinal) {
    if (ordinal >= map->length) return SIZE_MAX;
    const uint64_t *tree = hash_order_tree(map);
    size_t position = 0;
    for (size_t step = map->capacity; step; step >>= 1) {
        size_t next = position + step;
        if (next <= map->capacity && tree[next - 1] <= ordinal) {
            ordinal -= (size_t)tree[next - 1];
            position = next;
        }
    }
    return position < map->order_count ? position : SIZE_MAX;
}
static size_t hash_order_entry(const XirHash *map, size_t ordinal) {
    size_t position = hash_order_position(map, ordinal);
    if (position == SIZE_MAX) return SIZE_MAX;
    uint64_t entry = hash_order(map)[position];
    return entry && entry <= map->length ? (size_t)entry - 1 : SIZE_MAX;
}
static void hash_order_compact(XirHash *map) {
    uint64_t *order = hash_order(map), *tree = hash_order_tree(map);
    size_t count = 0;
    for (size_t i = 0; i < map->order_count; ++i) {
        if (!order[i]) continue;
        order[count] = order[i];
        hash_entry_set_order(map, (size_t)order[count] - 1, count);
        ++count;
    }
    XR_CHECK(count == map->length, "order compaction preserves every dense owner");
    memset(order + count, 0, (map->capacity - count) * sizeof(*order));
    for (size_t i = 0; i < map->capacity; ++i) tree[i] = i < count;
    for (size_t i = 0; i < map->capacity; ++i) {
        size_t parent = i | (i + 1);
        if (parent < map->capacity) tree[parent] += tree[i];
    }
    map->order_count = count;
}
static void hash_order_append(XirHash *map, size_t entry) {
    if (map->order_count == map->capacity) hash_order_compact(map);
    size_t position = map->order_count++;
    XR_CHECK(position < map->capacity, "hash load leaves room for a new order position");
    hash_order(map)[position] = entry + 1;
    hash_entry_set_order(map, entry, position);
    uint64_t *tree = hash_order_tree(map);
    for (size_t p = position; p < map->capacity; p |= p + 1) ++tree[p];
}
static void hash_order_remove(XirHash *map, size_t entry) {
    size_t position = hash_entry_order(map, entry);
    XR_CHECK(position < map->order_count && hash_order(map)[position] == entry + 1,
        "deletion consumes the exact insertion position");
    hash_order(map)[position] = 0;
    uint64_t *tree = hash_order_tree(map);
    for (size_t p = position; p < map->capacity; p |= p + 1) {
        XR_CHECK(tree[p], "each removed order position owns one live rank");
        --tree[p];
    }
}
