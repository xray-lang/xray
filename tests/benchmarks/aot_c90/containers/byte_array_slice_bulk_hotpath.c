#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BULK_N 4096
#define BULK_ROUNDS 8000

static int64_t common_prefix(const uint8_t *a, const uint8_t *b, int64_t n) {
    int64_t i = 0;
    while (i < n && a[i] == b[i])
        i = i + 1;
    return i;
}

static int64_t run(int64_t n, int64_t rounds) {
    int64_t total_len = n * 2;
    uint8_t *seed = (uint8_t *) malloc((size_t) n);
    uint8_t *out = (uint8_t *) malloc((size_t) total_len);
    uint8_t *copy = (uint8_t *) calloc((size_t) total_len, sizeof(uint8_t));
    if (!seed || !out || !copy) {
        free(seed);
        free(out);
        free(copy);
        return -1;
    }

    int64_t i = 0;
    while (i < n) {
        seed[i] = (uint8_t) (i * 17 + 3);
        i = i + 1;
    }
    memcpy(out, seed, (size_t) n);
    memcpy(out + n, seed, (size_t) n);

    int64_t total = 0;
    int64_t r = 0;
    while (r < rounds) {
        memcpy(copy, out, (size_t) total_len);
        total = total + total_len;

        int64_t p = 0;
        while (p < total_len) {
            uint16_t word = (uint16_t) copy[p] | ((uint16_t) copy[p + 1] << 8);
            word = (uint16_t) (word + 1);
            copy[p] = (uint8_t) (word & 0xffu);
            copy[p + 1] = (uint8_t) (word >> 8);
            p = p + 64;
        }

        total = total + common_prefix(copy, out, total_len);
        total = total + memcmp(copy, out, (size_t) total_len);
        r = r + 1;
    }

    total = total + copy[0] + copy[n] + total_len;
    free(seed);
    free(out);
    free(copy);
    return total;
}

int main(void) {
    printf("%lld\n", (long long) run(BULK_N, BULK_ROUNDS));
    return 0;
}
