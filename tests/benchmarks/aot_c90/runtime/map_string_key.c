/* C reference for map_string_key.xr: open-addressing string-key map
 * (FNV-1a hash, linear probing, owned key copies). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **keys;
    int64_t *vals;
    size_t cap;
    size_t len;
} SMap;

static uint64_t hash_str(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint8_t) s[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void smap_init(SMap *m, size_t cap) {
    m->cap = cap;
    m->len = 0;
    m->keys = (char **) calloc(cap, sizeof(char *));
    m->vals = (int64_t *) malloc(cap * sizeof(int64_t));
    if (!m->keys || !m->vals)
        abort();
}

static void smap_set(SMap *m, const char *k, int64_t v) {
    size_t mask = m->cap - 1;
    size_t klen = strlen(k);
    size_t i = (size_t) hash_str(k, klen) & mask;
    for (;;) {
        if (!m->keys[i]) {
            m->keys[i] = strdup(k);
            m->vals[i] = v;
            m->len++;
            return;
        }
        if (strcmp(m->keys[i], k) == 0) {
            m->vals[i] = v;
            return;
        }
        i = (i + 1) & mask;
    }
}

static int smap_find(const SMap *m, const char *k, int64_t *out) {
    size_t mask = m->cap - 1;
    size_t klen = strlen(k);
    size_t i = (size_t) hash_str(k, klen) & mask;
    for (;;) {
        if (!m->keys[i])
            return 0;
        if (strcmp(m->keys[i], k) == 0) {
            *out = m->vals[i];
            return 1;
        }
        i = (i + 1) & mask;
    }
}

static int64_t run(int64_t n) {
    SMap m;
    size_t cap = 1;
    while ((int64_t) cap < n * 2)
        cap <<= 1;
    smap_init(&m, cap);

    char key[64];
    for (int64_t i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "user:%lld", (long long) ((i * 7) % n));
        smap_set(&m, key, i);
    }

    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "user:%lld", (long long) i);
        int64_t v;
        if (smap_find(&m, key, &v))
            sum += v;
    }

    sum += (int64_t) m.len;
    for (size_t i = 0; i < m.cap; i++)
        free(m.keys[i]);
    free(m.keys);
    free(m.vals);
    return sum;
}

int main(void) {
    printf("%lld\n", (long long) run(60000));
    return 0;
}
