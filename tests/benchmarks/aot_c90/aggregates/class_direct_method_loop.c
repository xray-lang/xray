#include <stdint.h>
#include <stdio.h>

typedef struct {
    int64_t value;
    int64_t step;
} Counter;

static void counter_init(Counter *c, int64_t init, int64_t step) {
    c->value = init;
    c->step = step;
}

static int64_t counter_bump(Counter *c, int64_t n) {
    int64_t i = 0;
    int64_t sum = 0;
    while (i < n) {
        c->value = c->value + c->step;
        sum = sum + c->value;
        i = i + 1;
    }
    return sum + c->value;
}

static int64_t run(int64_t n) {
    Counter c;
    counter_init(&c, 1, 3);
    return counter_bump(&c, n);
}

int main(void) {
    printf("%lld\n", (long long) run(2000000));
    return 0;
}
