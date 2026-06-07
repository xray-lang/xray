#include <stdint.h>
#include <stdio.h>

typedef struct {
    int64_t a;
    int64_t b;
    int64_t scale;
} Pair;

static int64_t run(int64_t n) {
    Pair p = {1, 2, 3};
    int64_t i = 0;
    int64_t sum = 0;
    while (i < n) {
        p.a = p.a + i;
        p.b = p.b + p.scale;
        sum = sum + p.a - p.b;
        i = i + 1;
    }
    return sum + p.a + p.b;
}

int main(void) {
    printf("%lld\n", (long long) run(2000000));
    return 0;
}
