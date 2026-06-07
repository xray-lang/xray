#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint8_t key;
    int32_t value;
} ByteMapEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    ByteMapEntry *entries;
} ByteMap;

typedef struct {
    ByteMap values;
} ByteMapBag;

static void byte_map_init(ByteMap *map, int64_t cap) {
    if (cap < 8)
        cap = 8;
    map->len = 0;
    map->cap = cap;
    map->entries = (ByteMapEntry *) calloc((size_t) cap, sizeof(ByteMapEntry));
    if (!map->entries)
        abort();
}

static int byte_map_find(ByteMap *map, int64_t key) {
    uint8_t needle = (uint8_t) key;
    int64_t i = 0;
    while (i < map->len) {
        if (map->entries[i].key == needle)
            return (int) i;
        i = i + 1;
    }
    return -1;
}

static void byte_map_set(ByteMap *map, int64_t key, int64_t value) {
    int index = byte_map_find(map, key);
    if (index >= 0) {
        map->entries[index].value = (int32_t) value;
        return;
    }
    if (map->len >= map->cap) {
        map->cap *= 2;
        ByteMapEntry *tmp =
            (ByteMapEntry *) realloc(map->entries, (size_t) map->cap * sizeof(ByteMapEntry));
        if (!tmp)
            abort();
        map->entries = tmp;
    }
    map->entries[map->len].key = (uint8_t) key;
    map->entries[map->len].value = (int32_t) value;
    map->len = map->len + 1;
}

static int byte_map_has(ByteMap *map, int64_t key) {
    return byte_map_find(map, key) >= 0;
}

static int64_t byte_map_get(ByteMap *map, int64_t key) {
    int index = byte_map_find(map, key);
    return index >= 0 ? (int64_t) map->entries[index].value : 0;
}

static void byte_map_bag_init(ByteMapBag *bag) {
    byte_map_init(&bag->values, 8);
}

static int64_t byte_map_bag_fill(ByteMapBag *bag, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        byte_map_set(&bag->values, i, i * 5 + 7);
        i = i + 1;
    }
    return bag->values.len;
}

static int64_t byte_map_bag_scan(ByteMapBag *bag, int64_t n, int64_t rounds) {
    int64_t r = 0;
    int64_t sum = 0;
    while (r < rounds) {
        int64_t i = 0;
        while (i < n) {
            if (byte_map_has(&bag->values, i))
                sum = sum + byte_map_get(&bag->values, i);
            i = i + 1;
        }
        r = r + 1;
    }
    return sum;
}

static int64_t class_map_u8_i32_field_loop_run(int64_t n, int64_t rounds) {
    ByteMapBag bag;
    byte_map_bag_init(&bag);
    int64_t count = byte_map_bag_fill(&bag, n);
    return byte_map_bag_scan(&bag, n, rounds) + count;
}

int main(void) {
    printf("%lld\n", (long long) class_map_u8_i32_field_loop_run(128, 5000));
    return 0;
}
