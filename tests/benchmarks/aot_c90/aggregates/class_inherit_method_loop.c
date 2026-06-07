#include <stdint.h>
#include <stdio.h>

typedef struct {
    int64_t kind;
} Shape;

typedef struct {
    Shape base;
    int64_t w;
    int64_t h;
} Rect;

static void shape_init(Shape *s, int64_t kind) {
    s->kind = kind;
}

static void rect_init(Rect *r, int64_t w, int64_t h) {
    shape_init(&r->base, 7);
    r->w = w;
    r->h = h;
}

static int64_t rect_area(Rect *r) {
    return r->w * r->h;
}

static int64_t rect_score(Rect *r, int64_t n) {
    int64_t i = 0;
    int64_t sum = 0;
    while (i < n) {
        r->w = r->w + (sum % 3);
        sum = sum + rect_area(r) + r->base.kind;
        i = i + 1;
    }
    return sum;
}

static int64_t run(int64_t n) {
    Rect r;
    rect_init(&r, 3, 4);
    return rect_score(&r, n);
}

int main(void) {
    printf("%lld\n", (long long) run(2000000));
    return 0;
}
