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
} SharedBag;

static SharedBag shared_bag;

static IntArray *int_array_new_uninit(int64_t cap) {
    if (cap < 0)
        cap = 0;
    IntArray *a = (IntArray *) malloc(sizeof(IntArray));
    if (!a)
        abort();
    a->len = 0;
    a->cap = cap;
    a->data = cap > 0 ? (int64_t *) malloc((size_t) cap * sizeof(int64_t)) : NULL;
    if (cap > 0 && !a->data)
        abort();
    return a;
}

static void shared_bag_init(SharedBag *bag) {
    bag->values = int_array_new_uninit(0);
}

static int64_t shared_bag_fill(SharedBag *bag, int64_t n, int64_t rounds) {
    bag->values = int_array_new_uninit(n);

    int64_t i = 0;
    while (i < n) {
        bag->values->data[i] = i * 3 + 1;
        i = i + 1;
    }
    bag->values->len = n > 0 ? n : 0;

    int64_t r = 0;
    int64_t sum = 0;
    while (r < rounds) {
        int64_t j = 0;
        while (j < bag->values->len) {
            sum = sum + bag->values->data[j];
            j = j + 1;
        }
        r = r + 1;
    }
    return sum;
}

static int64_t class_exported_shared_array_field_fill_loop_run(int64_t rounds) {
    return shared_bag_fill(&shared_bag, 256, rounds);
}

int main(void) {
    shared_bag_init(&shared_bag);
    printf("%lld\n", (long long) class_exported_shared_array_field_fill_loop_run(20000));
    return 0;
}
