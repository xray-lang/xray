#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int64_t key;
    float value;
} IntFloatMapEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    IntFloatMapEntry *entries;
} IntFloatMap;

typedef struct {
    IntFloatMap values;
} IntFloatMapBag;

static void int_float_map_init(IntFloatMap *map, int64_t cap) {
    if (cap < 8)
        cap = 8;
    map->len = 0;
    map->cap = cap;
    map->entries = (IntFloatMapEntry *) calloc((size_t) cap, sizeof(IntFloatMapEntry));
    if (!map->entries)
        abort();
}

static int int_float_map_find(IntFloatMap *map, int64_t key) {
    int64_t i = 0;
    while (i < map->len) {
        if (map->entries[i].key == key)
            return (int) i;
        i = i + 1;
    }
    return -1;
}

static void int_float_map_set(IntFloatMap *map, int64_t key, double value) {
    int index = int_float_map_find(map, key);
    if (index >= 0) {
        map->entries[index].value = (float) value;
        return;
    }
    if (map->len >= map->cap) {
        map->cap *= 2;
        IntFloatMapEntry *tmp = (IntFloatMapEntry *) realloc(
            map->entries, (size_t) map->cap * sizeof(IntFloatMapEntry));
        if (!tmp)
            abort();
        map->entries = tmp;
    }
    map->entries[map->len].key = key;
    map->entries[map->len].value = (float) value;
    map->len = map->len + 1;
}

static int int_float_map_has(IntFloatMap *map, int64_t key) {
    return int_float_map_find(map, key) >= 0;
}

static double int_float_map_get(IntFloatMap *map, int64_t key) {
    int index = int_float_map_find(map, key);
    return index >= 0 ? (double) map->entries[index].value : 0.0;
}

static void int_float_map_bag_init(IntFloatMapBag *bag) {
    int_float_map_init(&bag->values, 8);
}

static int64_t int_float_map_bag_fill(IntFloatMapBag *bag, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        int_float_map_set(&bag->values, i, (double) i * 0.25 + 1.0);
        i = i + 1;
    }
    return bag->values.len;
}

static double int_float_map_bag_scan(IntFloatMapBag *bag, int64_t n, int64_t rounds) {
    int64_t r = 0;
    double sum = 0.0;
    while (r < rounds) {
        int64_t i = 0;
        while (i < n) {
            if (int_float_map_has(&bag->values, i))
                sum = sum + int_float_map_get(&bag->values, i);
            i = i + 1;
        }
        r = r + 1;
    }
    return sum;
}

static double class_map_i64_f32_field_loop_run(int64_t n, int64_t rounds) {
    IntFloatMapBag bag;
    int_float_map_bag_init(&bag);
    int64_t count = int_float_map_bag_fill(&bag, n);
    return int_float_map_bag_scan(&bag, n, rounds) + (double) count;
}

int main(void) {
    printf("%.1f\n", class_map_i64_f32_field_loop_run(64, 5000));
    return 0;
}
