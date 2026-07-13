#include <stdint.h>
#include <stdio.h>

#define BYTES_U8_INDEX_FILL_SUM_N 256
#define BYTES_U8_INDEX_FILL_SUM_ROUNDS 20000

static int64_t run(int64_t n, int64_t rounds) {
    uint8_t bytes[BYTES_U8_INDEX_FILL_SUM_N];

    int64_t i = 0;
    while (i < n) {
        bytes[i] = (uint8_t) (i * 3 + 1);
        i = i + 1;
    }

    int64_t r = 0;
    uint8_t sum = 0;
    while (r < rounds) {
        int64_t j = 0;
        while (j < n) {
            sum = (uint8_t) (sum + bytes[j]);
            j = j + 1;
        }
        r = r + 1;
    }
    return (int64_t) sum;
}

int main(void) {
    printf("%lld\n", (long long) run(BYTES_U8_INDEX_FILL_SUM_N, BYTES_U8_INDEX_FILL_SUM_ROUNDS));
    return 0;
}
