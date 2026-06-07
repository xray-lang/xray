#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    float key;
    float value;
} FloatMapEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    FloatMapEntry *entries;
} FloatMap;

typedef struct {
    FloatMap values;
} FloatMapBag;

static void float_map_init(FloatMap *map, int64_t cap) {
    if (cap < 8)
        cap = 8;
    map->len = 0;
    map->cap = cap;
    map->entries = (FloatMapEntry *) calloc((size_t) cap, sizeof(FloatMapEntry));
    if (!map->entries)
        abort();
}

static int float_map_find(FloatMap *map, double key) {
    float needle = (float) key;
    int64_t i = 0;
    while (i < map->len) {
        if (map->entries[i].key == needle)
            return (int) i;
        i = i + 1;
    }
    return -1;
}

static void float_map_set(FloatMap *map, double key, double value) {
    int index = float_map_find(map, key);
    if (index >= 0) {
        map->entries[index].value = (float) value;
        return;
    }
    if (map->len >= map->cap) {
        map->cap *= 2;
        FloatMapEntry *tmp =
            (FloatMapEntry *) realloc(map->entries, (size_t) map->cap * sizeof(FloatMapEntry));
        if (!tmp)
            abort();
        map->entries = tmp;
    }
    map->entries[map->len].key = (float) key;
    map->entries[map->len].value = (float) value;
    map->len = map->len + 1;
}

static int float_map_has(FloatMap *map, double key) {
    return float_map_find(map, key) >= 0;
}

static double float_map_get(FloatMap *map, double key) {
    int index = float_map_find(map, key);
    return index >= 0 ? (double) map->entries[index].value : 0.0;
}

static void float_map_bag_init(FloatMapBag *bag) {
    float_map_init(&bag->values, 8);
}

static int64_t float_map_bag_fill(FloatMapBag *bag, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        float_map_set(&bag->values, (double) i + 0.5, (double) i * 0.25 + 1.0);
        i = i + 1;
    }
    return bag->values.len;
}

static double float_map_bag_scan(FloatMapBag *bag, int64_t n, int64_t rounds) {
    int64_t r = 0;
    double sum = 0.0;
    while (r < rounds) {
        int64_t i = 0;
        while (i < n) {
            double key = (double) i + 0.5;
            if (float_map_has(&bag->values, key))
                sum = sum + float_map_get(&bag->values, key);
            i = i + 1;
        }
        r = r + 1;
    }
    return sum;
}

static double class_map_f32_f32_field_loop_run(int64_t n, int64_t rounds) {
    FloatMapBag bag;
    float_map_bag_init(&bag);
    int64_t count = float_map_bag_fill(&bag, n);
    return float_map_bag_scan(&bag, n, rounds) + (double) count;
}

int main(void) {
    printf("%.1f\n", class_map_f32_f32_field_loop_run(64, 5000));
    return 0;
}
