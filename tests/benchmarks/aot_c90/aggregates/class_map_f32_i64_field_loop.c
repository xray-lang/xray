#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    float key;
    int64_t value;
} FloatIntMapEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    FloatIntMapEntry *entries;
} FloatIntMap;

typedef struct {
    FloatIntMap values;
} FloatIntMapBag;

static void float_int_map_init(FloatIntMap *map, int64_t cap) {
    if (cap < 8)
        cap = 8;
    map->len = 0;
    map->cap = cap;
    map->entries = (FloatIntMapEntry *) calloc((size_t) cap, sizeof(FloatIntMapEntry));
    if (!map->entries)
        abort();
}

static int float_int_map_find(FloatIntMap *map, double key) {
    float needle = (float) key;
    int64_t i = 0;
    while (i < map->len) {
        if (map->entries[i].key == needle)
            return (int) i;
        i = i + 1;
    }
    return -1;
}

static void float_int_map_set(FloatIntMap *map, double key, int64_t value) {
    int index = float_int_map_find(map, key);
    if (index >= 0) {
        map->entries[index].value = value;
        return;
    }
    if (map->len >= map->cap) {
        map->cap *= 2;
        FloatIntMapEntry *tmp = (FloatIntMapEntry *) realloc(
            map->entries, (size_t) map->cap * sizeof(FloatIntMapEntry));
        if (!tmp)
            abort();
        map->entries = tmp;
    }
    map->entries[map->len].key = (float) key;
    map->entries[map->len].value = value;
    map->len = map->len + 1;
}

static int float_int_map_has(FloatIntMap *map, double key) {
    return float_int_map_find(map, key) >= 0;
}

static int64_t float_int_map_get(FloatIntMap *map, double key) {
    int index = float_int_map_find(map, key);
    return index >= 0 ? map->entries[index].value : 0;
}

static void float_int_map_bag_init(FloatIntMapBag *bag) {
    float_int_map_init(&bag->values, 8);
}

static int64_t float_int_map_bag_fill(FloatIntMapBag *bag, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        float_int_map_set(&bag->values, (double) i + 0.5, i * 3 + 1);
        i = i + 1;
    }
    return bag->values.len;
}

static int64_t float_int_map_bag_scan(FloatIntMapBag *bag, int64_t n, int64_t rounds) {
    int64_t r = 0;
    int64_t sum = 0;
    while (r < rounds) {
        int64_t i = 0;
        while (i < n) {
            double key = (double) i + 0.5;
            if (float_int_map_has(&bag->values, key))
                sum = sum + float_int_map_get(&bag->values, key);
            i = i + 1;
        }
        r = r + 1;
    }
    return sum;
}

static int64_t class_map_f32_i64_field_loop_run(int64_t n, int64_t rounds) {
    FloatIntMapBag bag;
    float_int_map_bag_init(&bag);
    int64_t count = float_int_map_bag_fill(&bag, n);
    return float_int_map_bag_scan(&bag, n, rounds) + count;
}

int main(void) {
    printf("%lld\n", (long long) class_map_f32_i64_field_loop_run(64, 5000));
    return 0;
}
