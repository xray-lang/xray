#include <stdint.h>
#include <stdio.h>

#define ARRAY_U8_WRAP_SUM_N 200000

static int64_t run(int64_t n) {
    uint8_t bytes[ARRAY_U8_WRAP_SUM_N];
    int64_t i = 0;
    while (i < n) {
        bytes[i] = (uint8_t) i;
        i = i + 1;
    }

    uint8_t sum = 0;
    i = 0;
    while (i < n) {
        sum = (uint8_t) (sum + bytes[i]);
        i = i + 1;
    }
    return (int64_t) sum;
}

int main(void) {
    printf("%lld\n", (long long) run(ARRAY_U8_WRAP_SUM_N));
    return 0;
}
