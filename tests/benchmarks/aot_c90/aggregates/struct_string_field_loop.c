#include <stdint.h>
#include <stdio.h>

typedef struct {
    int64_t total;
    int64_t step;
    const char *name;
} Counter;

static Counter counter = {1, 3, "counter"};

static int64_t run(int64_t n) {
    Counter *p = &counter;
    int64_t i = 0;
    int64_t sum = 0;
    while (i < n) {
        p->total = p->total + p->step + i;
        sum = sum + p->total;
        i = i + 1;
    }
    return sum + p->total;
}

int main(void) {
    printf("%lld\n", (long long) run(2000000));
    return 0;
}
