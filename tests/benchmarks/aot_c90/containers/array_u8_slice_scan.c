#include <stdint.h>
#include <stdio.h>

#define ARRAY_U8_SLICE_SCAN_N 200000

static int64_t run(int64_t n) {
    uint8_t bytes[ARRAY_U8_SLICE_SCAN_N];
    int64_t i = 0;
    while (i < n) {
        bytes[i] = (uint8_t) i;
        i = i + 1;
    }

    uint8_t *mid = bytes + 17;
    int64_t len = n - 34;
    uint8_t sum = 0;
    i = 0;
    while (i < len) {
        sum = (uint8_t) (sum + mid[i]);
        i = i + 1;
    }
    return (int64_t) sum;
}

int main(void) {
    printf("%lld\n", (long long) run(ARRAY_U8_SLICE_SCAN_N));
    return 0;
}
