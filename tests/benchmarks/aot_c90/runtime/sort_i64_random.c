/* C reference for sort_i64_random.xr: libc qsort on int64 (idiomatic C). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *) a, y = *(const int64_t *) b;
    return (x > y) - (x < y);
}

static int64_t run(int64_t n) {
    int64_t *values = (int64_t *) malloc((size_t) n * sizeof(int64_t));
    if (!values)
        abort();
    int64_t seed = 123456789;
    for (int64_t i = 0; i < n; i++) {
        seed = (seed * 1103515245 + 12345) % 2147483648;
        values[i] = seed;
    }

    qsort(values, (size_t) n, sizeof(int64_t), cmp_i64);

    int64_t checksum = values[0] + values[n / 2] + values[n - 1];
    for (int64_t i = 0; i < n; i += 7919)
        checksum += values[i] % 7;
    free(values);
    return checksum;
}

int main(void) {
    printf("%lld\n", (long long) run(1000000));
    return 0;
}
