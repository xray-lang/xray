/* C reference for string_concat_builder.xr: each `s + t` allocates a fresh
 * string of the combined length (same allocation pattern as the Xray
 * implementation, which rebuilds the string per concatenation). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *concat2(const char *a, size_t la, const char *b, size_t lb, size_t *out_len) {
    char *r = (char *) malloc(la + lb + 1);
    if (!r)
        abort();
    memcpy(r, a, la);
    memcpy(r + la, b, lb);
    r[la + lb] = 0;
    *out_len = la + lb;
    return r;
}

static int64_t run(int64_t rounds, int64_t parts) {
    int64_t total = 0;
    for (int64_t r = 0; r < rounds; r++) {
        char *s = (char *) malloc(1);
        size_t slen = 0;
        s[0] = 0;
        for (int64_t i = 0; i < parts; i++) {
            char piece[32];
            int plen = snprintf(piece, sizeof(piece), "part-%lld;", (long long) (i % 10));
            size_t nlen;
            char *ns = concat2(s, slen, piece, (size_t) plen, &nlen);
            free(s);
            s = ns;
            slen = nlen;
        }
        total += (int64_t) slen;
        free(s);
    }
    return total;
}

int main(void) {
    printf("%lld\n", (long long) run(4000, 40));
    return 0;
}
