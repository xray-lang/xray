#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint8_t key;
    int64_t value;
} BoolIntMapEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    BoolIntMapEntry *entries;
} BoolIntMap;

typedef struct {
    BoolIntMap values;
} BoolIntMapBag;

static void bool_int_map_init(BoolIntMap *map, int64_t cap) {
    if (cap < 8)
        cap = 8;
    map->len = 0;
    map->cap = cap;
    map->entries = (BoolIntMapEntry *) calloc((size_t) cap, sizeof(BoolIntMapEntry));
    if (!map->entries)
        abort();
}

static int bool_int_map_find(BoolIntMap *map, int key) {
    uint8_t needle = key ? 1 : 0;
    int64_t i = 0;
    while (i < map->len) {
        if (map->entries[i].key == needle)
            return (int) i;
        i = i + 1;
    }
    return -1;
}

static void bool_int_map_set(BoolIntMap *map, int key, int64_t value) {
    int index = bool_int_map_find(map, key);
    if (index >= 0) {
        map->entries[index].value = value;
        return;
    }
    if (map->len >= map->cap) {
        map->cap *= 2;
        BoolIntMapEntry *tmp =
            (BoolIntMapEntry *) realloc(map->entries, (size_t) map->cap * sizeof(BoolIntMapEntry));
        if (!tmp)
            abort();
        map->entries = tmp;
    }
    map->entries[map->len].key = key ? 1 : 0;
    map->entries[map->len].value = value;
    map->len = map->len + 1;
}

static int bool_int_map_has(BoolIntMap *map, int key) {
    return bool_int_map_find(map, key) >= 0;
}

static int64_t bool_int_map_get(BoolIntMap *map, int key) {
    int index = bool_int_map_find(map, key);
    return index >= 0 ? map->entries[index].value : 0;
}

static void bool_int_map_bag_init(BoolIntMapBag *bag) {
    bool_int_map_init(&bag->values, 8);
}

static int64_t bool_int_map_bag_fill(BoolIntMapBag *bag) {
    bool_int_map_set(&bag->values, 1, 21);
    bool_int_map_set(&bag->values, 0, 34);
    return bag->values.len;
}

static int64_t bool_int_map_bag_scan(BoolIntMapBag *bag, int64_t rounds) {
    int64_t r = 0;
    int64_t sum = 0;
    while (r < rounds) {
        if (bool_int_map_has(&bag->values, 1))
            sum = sum + bool_int_map_get(&bag->values, 1);
        if (bool_int_map_has(&bag->values, 0))
            sum = sum + bool_int_map_get(&bag->values, 0);
        r = r + 1;
    }
    return sum;
}

static int64_t class_map_bool_i64_field_loop_run(int64_t rounds) {
    BoolIntMapBag bag;
    bool_int_map_bag_init(&bag);
    int64_t count = bool_int_map_bag_fill(&bag);
    return bool_int_map_bag_scan(&bag, rounds) + count;
}

int main(void) {
    printf("%lld\n", (long long) class_map_bool_i64_field_loop_run(400000));
    return 0;
}
