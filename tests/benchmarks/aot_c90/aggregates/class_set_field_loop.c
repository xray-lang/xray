#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int64_t len;
    int64_t cap;
    int64_t *items;
} IntSet;

typedef struct {
    IntSet *values;
} IntSetBag;

static IntSet *int_set_new(int64_t cap) {
    if (cap < 8)
        cap = 8;
    IntSet *set = (IntSet *) calloc(1, sizeof(IntSet));
    set->cap = cap;
    set->items = (int64_t *) calloc((size_t) cap, sizeof(int64_t));
    return set;
}

static int int_set_has(IntSet *set, int64_t value) {
    for (int64_t i = 0; i < set->len; i++) {
        if (set->items[i] == value)
            return 1;
    }
    return 0;
}

static void int_set_add(IntSet *set, int64_t value) {
    if (int_set_has(set, value))
        return;
    if (set->len >= set->cap) {
        set->cap *= 2;
        set->items = (int64_t *) realloc(set->items, (size_t) set->cap * sizeof(int64_t));
    }
    set->items[set->len++] = value;
}

static int int_set_delete(IntSet *set, int64_t value) {
    for (int64_t i = 0; i < set->len; i++) {
        if (set->items[i] == value) {
            set->items[i] = set->items[--set->len];
            return 1;
        }
    }
    return 0;
}

static void int_set_bag_init(IntSetBag *bag) {
    bag->values = int_set_new(8);
}

static int64_t class_set_field_loop_fill(IntSetBag *bag, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        int_set_add(bag->values, i);
        i = i + 1;
    }
    return bag->values->len + bag->values->len;
}

static int64_t class_set_field_loop_scan(IntSetBag *bag, int64_t n, int64_t rounds) {
    int64_t r = 0;
    int64_t hits = 0;
    while (r < rounds) {
        int64_t i = 0;
        while (i < n) {
            if (int_set_has(bag->values, i))
                hits = hits + i;
            i = i + 1;
        }
        r = r + 1;
    }
    return hits;
}

static int64_t class_set_field_loop_prune(IntSetBag *bag, int64_t n) {
    int64_t i = 0;
    int64_t removed = 0;
    while (i < n) {
        if (int_set_delete(bag->values, i))
            removed = removed + 1;
        i = i + 2;
    }
    return removed + bag->values->len;
}

static int64_t class_set_field_loop_run(int64_t n, int64_t rounds) {
    IntSetBag bag;
    int_set_bag_init(&bag);
    int64_t count = class_set_field_loop_fill(&bag, n);
    int64_t sum = class_set_field_loop_scan(&bag, n, rounds);
    return sum + count + class_set_field_loop_prune(&bag, n);
}

int main(void) {
    printf("%lld\n", (long long) class_set_field_loop_run(64, 5000));
    return 0;
}
