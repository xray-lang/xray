#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint8_t data[4];
    int64_t bias;
} Buf;

static Buf buf = {{1, 2, 3, 4}, 5};

static int64_t run(int64_t n) {
    int64_t i = 0;
    while (i < n) {
        buf.data[0] = (uint8_t) i;
        buf.data[1] = buf.data[0];
        buf.data[2] = buf.data[1];
        buf.data[3] = buf.data[2];
        i = i + 1;
    }
    return buf.data[3];
}

int main(void) {
    printf("%lld\n", (long long) run(2000000));
    return 0;
}
