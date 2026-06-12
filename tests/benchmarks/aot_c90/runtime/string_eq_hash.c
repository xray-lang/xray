/* C reference for string_eq_hash.xr: string array + open-addressing
 * string-count map + adjacent content comparisons. */
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

static int64_t *smap_slot(SMap *m, const char *k) {
    size_t mask = m->cap - 1;
    size_t i = (size_t) hash_str(k, strlen(k)) & mask;
    for (;;) {
        if (!m->keys[i]) {
            m->keys[i] = strdup(k);
            m->vals[i] = 0;
            m->len++;
            return &m->vals[i];
        }
        if (strcmp(m->keys[i], k) == 0)
            return &m->vals[i];
        i = (i + 1) & mask;
    }
}

static int64_t smap_get(SMap *m, const char *k) {
    size_t mask = m->cap - 1;
    size_t i = (size_t) hash_str(k, strlen(k)) & mask;
    for (;;) {
        if (!m->keys[i])
            return 0;
        if (strcmp(m->keys[i], k) == 0)
            return m->vals[i];
        i = (i + 1) & mask;
    }
}

static int64_t run(int64_t n) {
    char **keys = (char **) malloc((size_t) n * sizeof(char *));
    if (!keys)
        abort();
    for (int64_t i = 0; i < n; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "key-%lld", (long long) ((i * 31) % 1000));
        keys[i] = strdup(buf);
    }

    SMap counts;
    smap_init(&counts, 4096);
    for (int64_t i = 0; i < n; i++)
        (*smap_slot(&counts, keys[i]))++;

    int64_t eq = 0;
    for (int64_t i = 0; i < n - 1; i++)
        if (strcmp(keys[i], keys[i + 1]) == 0)
            eq++;

    int64_t result = (int64_t) counts.len + eq + smap_get(&counts, "key-0");

    for (int64_t i = 0; i < n; i++)
        free(keys[i]);
    free(keys);
    for (size_t i = 0; i < counts.cap; i++)
        free(counts.keys[i]);
    free(counts.keys);
    free(counts.vals);
    return result;
}

int main(void) {
    printf("%lld\n", (long long) run(400000));
    return 0;
}
