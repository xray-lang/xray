/* C reference for sort_string.xr: qsort over char* with strcmp. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *) a, *(const char *const *) b);
}

static int64_t run(int64_t n) {
    char **values = (char **) malloc((size_t) n * sizeof(char *));
    if (!values)
        abort();
    for (int64_t i = 0; i < n; i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "item-%lld-tail", (long long) ((i * 37) % 5000));
        values[i] = strdup(buf);
    }

    qsort(values, (size_t) n, sizeof(char *), cmp_str);

    int64_t checksum = (int64_t) strlen(values[0]) + (int64_t) strlen(values[n / 2]) +
                       (int64_t) strlen(values[n - 1]);
    for (int64_t i = 0; i < n; i += 997)
        checksum += (int64_t) strlen(values[i]);

    for (int64_t i = 0; i < n; i++)
        free(values[i]);
    free(values);
    return checksum;
}

int main(void) {
    printf("%lld\n", (long long) run(100000));
    return 0;
}
