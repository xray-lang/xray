#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
/* Same work as seed.xr: fill 5000001 int64 flags by appending to a growable
 * buffer, then cross out multiples from p * p in steps of p and count the
 * primes up to 5000000. */
int main(void) {
    const int64_t n = INT64_C(5000000);
    size_t cap = 16, len = 0;
    int64_t *flags = malloc(cap * sizeof(*flags));
    if (!flags) return 2;
    for (int64_t i = 0; i <= n; ++i) {
        if (len == cap) {
            int64_t *grown = realloc(flags, cap * 2 * sizeof(*flags));
            if (!grown) { free(flags); return 2; }
            flags = grown; cap *= 2;
        }
        flags[len++] = 1;
    }
    int64_t count = 0;
    for (int64_t p = 2; p <= n; ++p) {
        if (flags[p] != 1) continue;
        ++count;
        for (int64_t m = p * p; m <= n; m += p) flags[m] = 0;
    }
    printf("%" PRId64 "\n", count);
    free(flags);
    return 0;
}
