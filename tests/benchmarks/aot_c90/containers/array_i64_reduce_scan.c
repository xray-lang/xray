#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int64_t reduce_value(int64_t acc, int64_t x) {
    return acc + x * 3;
}

static int64_t run(int64_t n) {
    int64_t *values = (int64_t *) malloc((size_t) n * sizeof(int64_t));
    if (!values) {
        return -1;
    }

    int64_t i = 0;
    while (i < n) {
        values[i] = (i * 17) % 251;
        i = i + 1;
    }

    int64_t acc = 5;
    i = 0;
    while (i < n) {
        acc = reduce_value(acc, values[i]);
        i = i + 1;
    }

    free(values);
    return acc;
}

int main(void) {
    printf("%lld\n", (long long) run(200000));
    return 0;
}
