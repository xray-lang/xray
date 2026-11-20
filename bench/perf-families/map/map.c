#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* Same work as seed.xr: a two-entry string-keyed hash table and a key whose
 * hash is computed once and cached, looked up 5000000 times by probing,
 * comparing the cached hash and the bytes, then adding the stored value. */
typedef struct Key { const char *bytes; size_t length; uint64_t hash; } Key;
typedef struct Row { Key key; int64_t value; int used; } Row;

static uint64_t fnv1a(const char *bytes, size_t length) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; ++i) {
        hash ^= (unsigned char)bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void insert(Row *rows, size_t mask, const char *bytes, int64_t value) {
    Key key = {bytes, strlen(bytes), fnv1a(bytes, strlen(bytes))};
    size_t slot = (size_t)key.hash & mask;
    while (rows[slot].used) slot = (slot + 1) & mask;
    rows[slot] = (Row){key, value, 1};
}

static int64_t lookup(const Row *rows, size_t mask, const Key *key) {
    for (size_t slot = (size_t)key->hash & mask;; slot = (slot + 1) & mask) {
        const Row *row = &rows[slot];
        if (!row->used) return 0;
        if (row->key.hash == key->hash && row->key.length == key->length &&
            !memcmp(row->key.bytes, key->bytes, key->length)) return row->value;
    }
}

int main(void) {
    Row rows[8] = {0};
    const size_t mask = 7;
    insert(rows, mask, "x", 2);
    insert(rows, mask, "y", 3);
    const Key key = {"x", 1, fnv1a("x", 1)};
    int64_t sum = 0;
    for (int64_t i = 0; i < INT64_C(5000000); ++i) sum += lookup(rows, mask, &key);
    printf("%" PRId64 "\n", sum);
    return 0;
}
