#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    double key;
    int64_t value;
} DoubleIntMapEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    DoubleIntMapEntry *entries;
} DoubleIntMap;

typedef struct {
    DoubleIntMap values;
} DoubleIntMapBag;

static void double_int_map_init(DoubleIntMap *map, int64_t cap) {
    if (cap < 8)
        cap = 8;
    map->len = 0;
    map->cap = cap;
    map->entries = (DoubleIntMapEntry *) calloc((size_t) cap, sizeof(DoubleIntMapEntry));
    if (!map->entries)
        abort();
}

static int double_int_map_find(DoubleIntMap *map, double key) {
    int64_t i = 0;
    while (i < map->len) {
        if (map->entries[i].key == key)
            return (int) i;
        i = i + 1;
    }
    return -1;
}

static void double_int_map_set(DoubleIntMap *map, double key, int64_t value) {
    int index = double_int_map_find(map, key);
    if (index >= 0) {
        map->entries[index].value = value;
        return;
    }
    if (map->len >= map->cap) {
        map->cap *= 2;
        DoubleIntMapEntry *tmp = (DoubleIntMapEntry *) realloc(
            map->entries, (size_t) map->cap * sizeof(DoubleIntMapEntry));
        if (!tmp)
            abort();
        map->entries = tmp;
    }
    map->entries[map->len].key = key;
    map->entries[map->len].value = value;
    map->len = map->len + 1;
}

static int double_int_map_has(DoubleIntMap *map, double key) {
    return double_int_map_find(map, key) >= 0;
}

static int64_t double_int_map_get(DoubleIntMap *map, double key) {
    int index = double_int_map_find(map, key);
    return index >= 0 ? map->entries[index].value : 0;
}

static void double_int_map_bag_init(DoubleIntMapBag *bag) {
    double_int_map_init(&bag->values, 8);
}

static int64_t double_int_map_bag_fill(DoubleIntMapBag *bag, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        double_int_map_set(&bag->values, (double) i + 0.5, i * 5 + 7);
        i = i + 1;
    }
    return bag->values.len;
}

static int64_t double_int_map_bag_scan(DoubleIntMapBag *bag, int64_t n, int64_t rounds) {
    int64_t r = 0;
    int64_t sum = 0;
    while (r < rounds) {
        int64_t i = 0;
        while (i < n) {
            double key = (double) i + 0.5;
            if (double_int_map_has(&bag->values, key))
                sum = sum + double_int_map_get(&bag->values, key);
            i = i + 1;
        }
        r = r + 1;
    }
    return sum;
}

static int64_t class_map_f64_i64_field_loop_run(int64_t n, int64_t rounds) {
    DoubleIntMapBag bag;
    double_int_map_bag_init(&bag);
    int64_t count = double_int_map_bag_fill(&bag, n);
    return double_int_map_bag_scan(&bag, n, rounds) + count;
}

int main(void) {
    printf("%lld\n", (long long) class_map_f64_i64_field_loop_run(64, 5000));
    return 0;
}
