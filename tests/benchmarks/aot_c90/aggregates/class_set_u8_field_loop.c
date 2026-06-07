#include <stdint.h>
#include <stdio.h>

typedef struct {
    int64_t len;
    uint8_t items[256];
} ByteSet;

typedef struct {
    ByteSet values;
} ByteSetBag;

static void byte_set_init(ByteSet *set) {
    set->len = 0;
}

static int byte_set_has(ByteSet *set, int64_t value) {
    uint8_t needle = (uint8_t) value;
    for (int64_t i = 0; i < set->len; i++) {
        if (set->items[i] == needle)
            return 1;
    }
    return 0;
}

static void byte_set_add(ByteSet *set, int64_t value) {
    if (byte_set_has(set, value))
        return;
    set->items[set->len++] = (uint8_t) value;
}

static int byte_set_delete(ByteSet *set, int64_t value) {
    uint8_t needle = (uint8_t) value;
    for (int64_t i = 0; i < set->len; i++) {
        if (set->items[i] == needle) {
            set->items[i] = set->items[--set->len];
            return 1;
        }
    }
    return 0;
}

static void byte_set_bag_init(ByteSetBag *bag) {
    byte_set_init(&bag->values);
}

static int64_t class_set_u8_field_loop_fill(ByteSetBag *bag, int64_t n) {
    int64_t i = 0;
    while (i < n) {
        byte_set_add(&bag->values, i);
        i = i + 1;
    }
    return bag->values.len;
}

static int64_t class_set_u8_field_loop_scan(ByteSetBag *bag, int64_t n, int64_t rounds) {
    int64_t r = 0;
    int64_t hits = 0;
    while (r < rounds) {
        int64_t i = 0;
        while (i < n) {
            if (byte_set_has(&bag->values, i))
                hits = hits + i;
            i = i + 1;
        }
        r = r + 1;
    }
    return hits;
}

static int64_t class_set_u8_field_loop_prune(ByteSetBag *bag, int64_t n) {
    int64_t i = 0;
    int64_t removed = 0;
    while (i < n) {
        if (byte_set_delete(&bag->values, i))
            removed = removed + 1;
        i = i + 2;
    }
    return removed + bag->values.len;
}

static int64_t class_set_u8_field_loop_run(int64_t n, int64_t rounds) {
    ByteSetBag bag;
    byte_set_bag_init(&bag);
    int64_t count = class_set_u8_field_loop_fill(&bag, n);
    int64_t sum = class_set_u8_field_loop_scan(&bag, n, rounds);
    return sum + count + class_set_u8_field_loop_prune(&bag, n);
}

int main(void) {
    printf("%lld\n", (long long) class_set_u8_field_loop_run(64, 5000));
    return 0;
}
