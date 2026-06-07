#include <stdint.h>
#include <stdio.h>

typedef struct {
    int64_t x;
    int64_t y;
    int64_t step;
} Point;

typedef struct {
    Point point;
    int64_t bias;
} Box;

static Box box = {{1, 2, 3}, 5};

static int64_t run(int64_t n) {
    int64_t i = 0;
    int64_t sum = 0;
    while (i < n) {
        box.point.x = box.point.x + i;
        box.point.y = box.point.y + box.point.step;
        sum = sum + box.point.x - box.point.y + box.bias;
        i = i + 1;
    }
    return sum + box.point.x + box.point.y + box.bias;
}

int main(void) {
    printf("%lld\n", (long long) run(2000000));
    return 0;
}
