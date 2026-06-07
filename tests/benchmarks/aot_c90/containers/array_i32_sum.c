#include <stdint.h>
#include <stdio.h>

#define ARRAY_I32_SUM_N 200000

static int64_t run(int64_t n) {
    int32_t vals[ARRAY_I32_SUM_N];
    int64_t i = 0;
    while (i < n) {
        vals[i] = (int32_t) ((i % 65536) - 32768);
        i = i + 1;
    }

    int64_t sum = 0;
    i = 0;
    while (i < n) {
        sum = sum + (int64_t) vals[i] * ((i % 3) + 1);
        i = i + 1;
    }
    return sum;
}

int main(void) {
    printf("%lld\n", (long long) run(ARRAY_I32_SUM_N));
    return 0;
}
