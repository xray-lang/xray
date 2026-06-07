#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int keep_value(int64_t x) {
    return (x % 3) == 1;
}

static int64_t run(int64_t n) {
    int64_t *values = (int64_t *) malloc((size_t) n * sizeof(int64_t));
    int64_t *kept = (int64_t *) malloc((size_t) n * sizeof(int64_t));
    if (!values || !kept) {
        free(values);
        free(kept);
        return -1;
    }

    int64_t i = 0;
    while (i < n) {
        values[i] = (i * 17) % 251;
        i = i + 1;
    }

    int64_t kept_len = 0;
    i = 0;
    while (i < n) {
        int64_t x = values[i];
        if (keep_value(x)) {
            kept[kept_len] = x;
            kept_len = kept_len + 1;
        }
        i = i + 1;
    }

    int64_t sum = 0;
    i = 0;
    while (i < kept_len) {
        sum = sum + kept[i];
        i = i + 1;
    }

    free(kept);
    free(values);
    return sum;
}

int main(void) {
    printf("%lld\n", (long long) run(200000));
    return 0;
}
