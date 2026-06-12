/* C reference for set_i64_membership.xr: open-addressing i64 set. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t *keys;
    uint8_t *full;
    size_t cap;
    size_t len;
} Set;

static uint64_t hash_i64(int64_t k) {
    uint64_t x = (uint64_t) k;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

static void set_init(Set *s, size_t cap) {
    s->cap = cap;
    s->len = 0;
    s->keys = (int64_t *) malloc(cap * sizeof(int64_t));
    s->full = (uint8_t *) calloc(cap, 1);
    if (!s->keys || !s->full)
        abort();
}

static void set_add(Set *s, int64_t k) {
    size_t mask = s->cap - 1;
    size_t i = (size_t) hash_i64(k) & mask;
    for (;;) {
        if (!s->full[i]) {
            s->keys[i] = k;
            s->full[i] = 1;
            s->len++;
            return;
        }
        if (s->keys[i] == k)
            return;
        i = (i + 1) & mask;
    }
}

static int set_has(const Set *s, int64_t k) {
    size_t mask = s->cap - 1;
    size_t i = (size_t) hash_i64(k) & mask;
    for (;;) {
        if (!s->full[i])
            return 0;
        if (s->keys[i] == k)
            return 1;
        i = (i + 1) & mask;
    }
}

static int64_t run(int64_t n, int64_t rounds) {
    Set s;
    size_t cap = 1;
    while ((int64_t) cap < n * 2)
        cap <<= 1;
    set_init(&s, cap);

    for (int64_t i = 0; i < n; i++)
        set_add(&s, i * 2);

    int64_t hits = 0;
    for (int64_t r = 0; r < rounds; r++) {
        for (int64_t i = 0; i < n * 2; i++)
            if (set_has(&s, i))
                hits++;
    }

    hits += (int64_t) s.len;
    free(s.keys);
    free(s.full);
    return hits;
}

int main(void) {
    printf("%lld\n", (long long) run(150000, 4));
    return 0;
}
