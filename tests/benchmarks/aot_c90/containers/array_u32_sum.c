#include <stdint.h>
#include <stdio.h>

#define ARRAY_U32_SUM_N 200000

static int64_t run(int64_t n) {
    uint32_t vals[ARRAY_U32_SUM_N];
    int64_t i = 0;
    while (i < n) {
        vals[i] = (uint32_t) (UINT32_C(2147483648) + (uint32_t) (i % 4096));
        i = i + 1;
    }

    uint32_t sum = 0;
    i = 0;
    while (i < n) {
        sum = (uint32_t) ((uint64_t) sum + (uint64_t) vals[i]);
        i = i + 1;
    }
    return (int64_t) sum;
}

int main(void) {
    printf("%lld\n", (long long) run(ARRAY_U32_SUM_N));
    return 0;
}
