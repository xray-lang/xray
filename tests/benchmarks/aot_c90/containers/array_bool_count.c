#include <stdint.h>
#include <stdio.h>

#define ARRAY_BOOL_COUNT_N 200000

static int64_t run(int64_t n) {
    uint8_t flags[ARRAY_BOOL_COUNT_N];
    int64_t i = 0;
    while (i < n) {
        flags[i] = (uint8_t) ((i % 5) == 0);
        i = i + 1;
    }

    int64_t count = 0;
    i = 0;
    while (i < n) {
        if (flags[i]) {
            count = count + 1;
        }
        i = i + 1;
    }
    return count;
}

int main(void) {
    printf("%lld\n", (long long) run(ARRAY_BOOL_COUNT_N));
    return 0;
}
