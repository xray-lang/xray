#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint8_t key;
    float value;
} BoolFloatMapEntry;

typedef struct {
    int64_t len;
    int64_t cap;
    BoolFloatMapEntry *entries;
} BoolFloatMap;

typedef struct {
    BoolFloatMap values;
} BoolFloatMapBag;

static void bool_float_map_init(BoolFloatMap *map, int64_t cap) {
    if (cap < 8)
        cap = 8;
    map->len = 0;
    map->cap = cap;
    map->entries = (BoolFloatMapEntry *) calloc((size_t) cap, sizeof(BoolFloatMapEntry));
    if (!map->entries)
        abort();
}

static int bool_float_map_find(BoolFloatMap *map, int key) {
    uint8_t needle = key ? 1 : 0;
    int64_t i = 0;
    while (i < map->len) {
        if (map->entries[i].key == needle)
            return (int) i;
        i = i + 1;
    }
    return -1;
}

static void bool_float_map_set(BoolFloatMap *map, int key, double value) {
    int index = bool_float_map_find(map, key);
    if (index >= 0) {
        map->entries[index].value = (float) value;
        return;
    }
    if (map->len >= map->cap) {
        map->cap *= 2;
        BoolFloatMapEntry *tmp = (BoolFloatMapEntry *) realloc(
            map->entries, (size_t) map->cap * sizeof(BoolFloatMapEntry));
        if (!tmp)
            abort();
        map->entries = tmp;
    }
    map->entries[map->len].key = key ? 1 : 0;
    map->entries[map->len].value = (float) value;
    map->len = map->len + 1;
}

static int bool_float_map_has(BoolFloatMap *map, int key) {
    return bool_float_map_find(map, key) >= 0;
}

static double bool_float_map_get(BoolFloatMap *map, int key) {
    int index = bool_float_map_find(map, key);
    return index >= 0 ? (double) map->entries[index].value : 0.0;
}

static void bool_float_map_bag_init(BoolFloatMapBag *bag) {
    bool_float_map_init(&bag->values, 8);
}

static int64_t bool_float_map_bag_fill(BoolFloatMapBag *bag) {
    bool_float_map_set(&bag->values, 1, 1.5);
    bool_float_map_set(&bag->values, 0, 2.25);
    return bag->values.len;
}

static double bool_float_map_bag_scan(BoolFloatMapBag *bag, int64_t rounds) {
    int64_t r = 0;
    double sum = 0.0;
    while (r < rounds) {
        if (bool_float_map_has(&bag->values, 1))
            sum = sum + bool_float_map_get(&bag->values, 1);
        if (bool_float_map_has(&bag->values, 0))
            sum = sum + bool_float_map_get(&bag->values, 0);
        r = r + 1;
    }
    return sum;
}

static double class_map_bool_f32_field_loop_run(int64_t rounds) {
    BoolFloatMapBag bag;
    bool_float_map_bag_init(&bag);
    int64_t count = bool_float_map_bag_fill(&bag);
    return bool_float_map_bag_scan(&bag, rounds) + (double) count;
}

int main(void) {
    printf("%.1f\n", class_map_bool_f32_field_loop_run(400000));
    return 0;
}
