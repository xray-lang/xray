#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int64_t len;
    int64_t cap;
    int64_t *data;
} IntArray;

typedef struct {
    IntArray *values;
} IntBag;

static IntArray *int_array_new(int64_t cap) {
    if (cap < 4)
        cap = 4;
    IntArray *a = (IntArray *) malloc(sizeof(IntArray));
    if (!a)
        abort();
    a->len = 0;
    a->cap = cap;
    a->data = (int64_t *) calloc((size_t) cap, sizeof(int64_t));
    if (!a->data)
        abort();
    return a;
}

static void int_array_push(IntArray *a, int64_t value) {
    if (a->len >= a->cap) {
        a->cap *= 2;
        int64_t *tmp = (int64_t *) realloc(a->data, (size_t) a->cap * sizeof(int64_t));
        if (!tmp)
            abort();
        a->data = tmp;
    }
    a->data[a->len++] = value;
}

static IntArray *make_values(int64_t n) {
    IntArray *values = int_array_new(4);
    int64_t i = 0;
    while (i < n) {
        int_array_push(values, i * 3 + 1);
        i = i + 1;
    }
    return values;
}

static void int_bag_init(IntBag *bag, IntArray *values) {
    bag->values = values;
}

static int64_t int_bag_scan(IntBag *bag, int64_t rounds) {
    int64_t r = 0;
    int64_t sum = 0;
    while (r < rounds) {
        int64_t i = 0;
        while (i < bag->values->len) {
            sum = sum + bag->values->data[i];
            i = i + 1;
        }
        r = r + 1;
    }
    return sum;
}

static int64_t class_array_field_loop_run(int64_t rounds) {
    IntBag bag;
    int_bag_init(&bag, make_values(256));
    return int_bag_scan(&bag, rounds);
}

int main(void) {
    printf("%lld\n", (long long) class_array_field_loop_run(20000));
    return 0;
}
