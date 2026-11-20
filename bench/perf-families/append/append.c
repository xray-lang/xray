#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Same work as seed.xr: append the two-byte suffix 2000000 times to an owned
 * growable byte buffer with geometric growth, then print the byte length. */
int main(void) {
    static const char suffix[] = "ab";
    const size_t piece = sizeof(suffix) - 1;
    size_t cap = 16, len = 0;
    char *text = malloc(cap);
    if (!text) return 2;
    for (int64_t i = 0; i < INT64_C(2000000); ++i) {
        if (piece > cap - len - 1) {
            if (cap > SIZE_MAX / 2) { free(text); return 2; }
            size_t next = cap * 2;
            char *replacement = realloc(text, next);
            if (!replacement) { free(text); return 2; }
            text = replacement; cap = next;
        }
        memcpy(text + len, suffix, piece);
        len += piece;
        text[len] = 0;
    }
    printf("%" PRId64 "\n", (int64_t)len);
    free(text);
    return 0;
}
