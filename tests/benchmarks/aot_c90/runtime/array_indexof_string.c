/* C reference for array_indexof_string.xr: linear scan with strcmp. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t index_of(char **arr, int64_t n, const char *needle) {
    for (int64_t i = 0; i < n; i++)
        if (strcmp(arr[i], needle) == 0)
            return i;
    return -1;
}

static int64_t run(int64_t n, int64_t queries) {
    char **haystack = (char **) malloc((size_t) n * sizeof(char *));
    if (!haystack)
        abort();
    for (int64_t i = 0; i < n; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "entry-%lld", (long long) i);
        haystack[i] = strdup(buf);
    }

    int64_t found = 0;
    for (int64_t i = 0; i < queries; i++) {
        char needle[32];
        snprintf(needle, sizeof(needle), "entry-%lld", (long long) ((i * 13) % (n * 2)));
        int64_t idx = index_of(haystack, n, needle);
        if (idx >= 0)
            found += idx;
    }

    for (int64_t i = 0; i < n; i++)
        free(haystack[i]);
    free(haystack);
    return found;
}

int main(void) {
    printf("%lld\n", (long long) run(2000, 3000));
    return 0;
}
