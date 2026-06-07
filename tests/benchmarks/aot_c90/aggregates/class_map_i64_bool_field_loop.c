#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int64_t key;
    uint8_t value;
} IntBoolMapEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    IntBoolMapEntry *entries;
} IntBoolMap;

typedef struct {
    IntBoolMap values;
} IntBoolMapBag;

static void int_bool_map_init(IntBoolMap *map, int64_t cap) {
    if (cap < 8)
        cap = 8;
    map->len = 0;
    map->cap = cap;
    map->entries = (IntBoolMapEntry *) calloc((size_t) cap, sizeof(IntBoolMapEntry));
    if (!map->entries)
        abort();
}

static int int_bool_map_find(IntBoolMap *map, int64_t key) {
    int64_t i = 0;
    while (i < map->len) {
        if (map->entries[i].key == key)
            return (int) i;
        i = i + 1;
    }
    return -1;
}

static void int_bool_map_set(IntBoolMap *map, int64_t key, int value) {
    int index = int_bool_map_find(map, key);
    if (index >= 0) {
        map->entries[index].value = value ? 1 : 0;
        return;
    }
    if (map->len >= map->cap) {
        map->cap *= 2;
        IntBoolMapEntry *tmp =
            (IntBoolMapEntry *) realloc(map->entries, (size_t) map->cap * sizeof(IntBoolMapEntry));
        if (!tmp)
            abort();
        map->entries = tmp;
    }
    map->entries[map->len].key = key;
    map->entries[map->len].value = value ? 1 : 0;
    map->len = map->len + 1;
}

static int int_bool_map_has(IntBoolMap *map, int64_t key) {
    return int_bool_map_find(map, key) >= 0;
}

static int int_bool_map_get(IntBoolMap *map, int64_t key) {
    int index = int_bool_map_find(map, key);
    return index >= 0 ? map->entries[index].value != 0 : 0;
}

static void int_bool_map_bag_init(IntBoolMapBag *bag) {
    int_bool_map_init(&bag->values, 8);
}

static int64_t int_bool_map_bag_fill(IntBoolMapBag *bag, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        int_bool_map_set(&bag->values, i, i % 3 == 0);
        i = i + 1;
    }
    return bag->values.len;
}

static int64_t int_bool_map_bag_scan(IntBoolMapBag *bag, int64_t n, int64_t rounds) {
    int64_t r = 0;
    int64_t total = 0;
    while (r < rounds) {
        int64_t i = 0;
        while (i < n) {
            if (int_bool_map_has(&bag->values, i)) {
                if (int_bool_map_get(&bag->values, i))
                    total = total + 1;
            }
            i = i + 1;
        }
        r = r + 1;
    }
    return total;
}

static int64_t class_map_i64_bool_field_loop_run(int64_t n, int64_t rounds) {
    IntBoolMapBag bag;
    int_bool_map_bag_init(&bag);
    int64_t count = int_bool_map_bag_fill(&bag, n);
    return int_bool_map_bag_scan(&bag, n, rounds) + count;
}

int main(void) {
    printf("%lld\n", (long long) class_map_i64_bool_field_loop_run(64, 5000));
    return 0;
}
