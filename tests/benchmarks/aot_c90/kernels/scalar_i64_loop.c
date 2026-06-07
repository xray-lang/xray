#include <stdint.h>
#include <stdio.h>

static int64_t run(int64_t n) {
    int64_t acc = 0;
    int64_t i = 0;
    while (i < n) {
        acc = acc + ((i * 31) % 1000003);
        i = i + 1;
    }
    return acc;
}

int main(void) {
    printf("%lld\n", (long long) run(1000000));
    return 0;
}
