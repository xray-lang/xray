#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int64_t key;
    int64_t value;
} IntMapEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    IntMapEntry *entries;
} IntMap;

typedef struct {
    IntMap values;
} IntMapBag;

static void int_map_init(IntMap *map, int64_t cap) {
    if (cap < 8)
        cap = 8;
    map->len = 0;
    map->cap = cap;
    map->entries = (IntMapEntry *) calloc((size_t) cap, sizeof(IntMapEntry));
    if (!map->entries)
        abort();
}

static int int_map_find(IntMap *map, int64_t key) {
    int64_t i = 0;
    while (i < map->len) {
        if (map->entries[i].key == key)
            return (int) i;
        i = i + 1;
    }
    return -1;
}

static void int_map_set(IntMap *map, int64_t key, int64_t value) {
    int index = int_map_find(map, key);
    if (index >= 0) {
        map->entries[index].value = value;
        return;
    }
    if (map->len >= map->cap) {
        map->cap *= 2;
        IntMapEntry *tmp =
            (IntMapEntry *) realloc(map->entries, (size_t) map->cap * sizeof(IntMapEntry));
        if (!tmp)
            abort();
        map->entries = tmp;
    }
    map->entries[map->len].key = key;
    map->entries[map->len].value = value;
    map->len = map->len + 1;
}

static int int_map_has(IntMap *map, int64_t key) {
    return int_map_find(map, key) >= 0;
}

static int64_t int_map_get(IntMap *map, int64_t key) {
    int index = int_map_find(map, key);
    return index >= 0 ? map->entries[index].value : 0;
}

static void int_map_bag_init(IntMapBag *bag) {
    int_map_init(&bag->values, 8);
}

static int64_t int_map_bag_fill(IntMapBag *bag, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        int_map_set(&bag->values, i, i * 3 + 1);
        i = i + 1;
    }
    return bag->values.len;
}

static int64_t int_map_bag_scan(IntMapBag *bag, int64_t n, int64_t rounds) {
    int64_t r = 0;
    int64_t sum = 0;
    while (r < rounds) {
        int64_t i = 0;
        while (i < n) {
            if (int_map_has(&bag->values, i))
                sum = sum + int_map_get(&bag->values, i);
            i = i + 1;
        }
        r = r + 1;
    }
    return sum;
}

static int64_t class_map_field_loop_run(int64_t n, int64_t rounds) {
    IntMapBag bag;
    int_map_bag_init(&bag);
    int64_t count = int_map_bag_fill(&bag, n);
    return int_map_bag_scan(&bag, n, rounds) + count;
}

int main(void) {
    printf("%lld\n", (long long) class_map_field_loop_run(64, 5000));
    return 0;
}
