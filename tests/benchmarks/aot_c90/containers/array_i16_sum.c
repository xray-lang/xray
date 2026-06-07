#include <stdint.h>
#include <stdio.h>

#define ARRAY_I16_SUM_N 200000

static int64_t wrap_i16(int64_t v) {
    uint64_t u = (uint64_t) v & UINT64_C(0xffff);
    return u <= UINT64_C(0x7fff) ? (int64_t) u : (int64_t) u - INT64_C(65536);
}

static int64_t run(int64_t n) {
    int16_t vals[ARRAY_I16_SUM_N];
    int64_t i = 0;
    while (i < n) {
        vals[i] = (int16_t) ((i % 65536) - 32768);
        i = i + 1;
    }

    int64_t sum = 0;
    i = 0;
    while (i < n) {
        int64_t product = wrap_i16((int64_t) vals[i] * ((i % 5) + 1));
        sum = wrap_i16(sum + product);
        i = i + 1;
    }
    return sum;
}

int main(void) {
    printf("%lld\n", (long long) run(ARRAY_I16_SUM_N));
    return 0;
}
