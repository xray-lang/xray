/* C reference for map_i64_churn.xr: hand-rolled open-addressing i64 map
 * (power-of-two capacity, linear probing, tombstones). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMPTY 0
#define FULL 1
#define DEAD 2

typedef struct {
    int64_t *keys;
    int64_t *vals;
    uint8_t *state;
    size_t cap; /* power of two */
    size_t len;
} Map;

static uint64_t hash_i64(int64_t k) {
    uint64_t x = (uint64_t) k;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

static void map_init(Map *m, size_t cap) {
    m->cap = cap;
    m->len = 0;
    m->keys = (int64_t *) malloc(cap * sizeof(int64_t));
    m->vals = (int64_t *) malloc(cap * sizeof(int64_t));
    m->state = (uint8_t *) calloc(cap, 1);
    if (!m->keys || !m->vals || !m->state)
        abort();
}

static void map_set(Map *m, int64_t k, int64_t v) {
    size_t mask = m->cap - 1;
    size_t i = (size_t) hash_i64(k) & mask;
    size_t first_dead = (size_t) -1;
    for (;;) {
        if (m->state[i] == EMPTY) {
            if (first_dead != (size_t) -1)
                i = first_dead;
            m->keys[i] = k;
            m->vals[i] = v;
            m->state[i] = FULL;
            m->len++;
            return;
        }
        if (m->state[i] == DEAD) {
            if (first_dead == (size_t) -1)
                first_dead = i;
        } else if (m->keys[i] == k) {
            m->vals[i] = v;
            return;
        }
        i = (i + 1) & mask;
    }
}

static int map_find(const Map *m, int64_t k, int64_t *out) {
    size_t mask = m->cap - 1;
    size_t i = (size_t) hash_i64(k) & mask;
    for (;;) {
        if (m->state[i] == EMPTY)
            return 0;
        if (m->state[i] == FULL && m->keys[i] == k) {
            if (out)
                *out = m->vals[i];
            return 1;
        }
        i = (i + 1) & mask;
    }
}

static void map_delete(Map *m, int64_t k) {
    size_t mask = m->cap - 1;
    size_t i = (size_t) hash_i64(k) & mask;
    for (;;) {
        if (m->state[i] == EMPTY)
            return;
        if (m->state[i] == FULL && m->keys[i] == k) {
            m->state[i] = DEAD;
            m->len--;
            return;
        }
        i = (i + 1) & mask;
    }
}

static int64_t run(int64_t n) {
    Map m;
    size_t cap = 1;
    while ((int64_t) cap < n * 2)
        cap <<= 1;
    map_init(&m, cap);

    for (int64_t i = 0; i < n; i++)
        map_set(&m, (i * 17) % n, i * 3 + 1);

    int64_t sum = 0;
    for (int64_t i = 0; i < n * 2; i++) {
        int64_t v;
        if (map_find(&m, i, &v))
            sum += v;
    }

    for (int64_t i = 0; i < n; i++)
        if (i % 2 == 0)
            map_delete(&m, i);

    for (int64_t i = 0; i < n; i++)
        if (map_find(&m, i, NULL))
            sum += 1;

    sum += (int64_t) m.len;
    free(m.keys);
    free(m.vals);
    free(m.state);
    return sum;
}

int main(void) {
    printf("%lld\n", (long long) run(200000));
    return 0;
}
