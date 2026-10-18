/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * p09_hash_faults.h - Every actual request and local work prefix preserves roots
 */
#ifndef P09_HASH_FAULTS_H
#define P09_HASH_FAULTS_H
typedef struct HashFaultCase {
    XrXirValue root, alias, key, value;
    XrXirValuePlace place;
    XrXirValueAdmission admission;
    unsigned operation;
} HashFaultCase;
static void hash_fault_setup(HashFaultCase *test, unsigned operation,
    XrXirTypeArena *arena, XrXirDomain *domain) {
    *test = (HashFaultCase){.operation = operation,
        .admission = {arena, domain, NULL, NULL, 1000000, 65536}};
    XrXirValue keys[4], values[4];
    for (int64_t i = 0; i < 4; ++i) { keys[i] = integer(i); values[i] = integer(i * 7); }
    CHECK(xr_xir_map_new((XrXirType)256, keys, values, 4, &test->admission, &test->root) == XR_XIR_VALUE_OK);
    if (operation && operation != 4) CHECK(xr_xir_value_copy(&test->root, &test->alias) == XR_XIR_VALUE_OK);
    test->place = (XrXirValuePlace){(XrXirType)256, &test->root.payload};
    test->key = integer(operation ? 2 : 42); test->value = integer(91);
}
static XrXirValueStatus hash_fault_operation(HashFaultCase *test) {
    bool removed = false;
    if (test->operation == 2 || test->operation == 4)
        return xr_xir_hash_delete(&test->place, &test->key, &test->admission, &removed);
    if (test->operation == 3) return xr_xir_hash_clear(&test->place, &test->admission);
    return xr_xir_map_set(&test->place, &test->key, &test->value, &test->admission);
}
static void hash_fault_finish(HashFaultCase *test) {
    xr_xir_value_drop(&test->alias); xr_xir_value_drop(&test->root);
}
static void hash_fault_unchanged(HashFaultCase *test, int64_t root, const unsigned char *bytes,
    const unsigned char *data, size_t size) {
    CHECK(test->root.payload == root);
    XirHash *map = (XirHash *)object_pointer(&test->root);
    CHECK(map->length == 4 && map->capacity == 8 && map->data == data && !memcmp(data, bytes, size));
    if (test->operation && test->operation != 4) CHECK(test->alias.payload == root);
    test->admission.work = 1000000;
    for (int64_t i = 0; i < 4; ++i) hash_expect(&test->root, i, i * 7, true, &test->admission);
}
static void hash_faults(XrXirTypeArena *arena, XrXirDomain *domain) {
    for (unsigned operation = 0; operation < 5; ++operation) {
        HashFaultCase baseline; hash_fault_setup(&baseline, operation, arena, domain);
        size_t before = calls; uint64_t work = baseline.admission.work;
        CHECK(hash_fault_operation(&baseline) == XR_XIR_VALUE_OK);
        size_t sites = calls - before; uint64_t exact = work - baseline.admission.work;
        hash_fault_finish(&baseline); CHECK((sites > 0 || operation == 4) && exact > 1);
        if (operation == 4) CHECK(!sites);
        for (size_t site = 0; site < sites; ++site) {
            HashFaultCase test; hash_fault_setup(&test, operation, arena, domain);
            XirHash *map = (XirHash *)object_pointer(&test.root); unsigned char bytes[256]; size_t size = 0;
            CHECK(hash_data_size(map->capacity, map->stride, &size) && size == sizeof(bytes));
            memcpy(bytes, map->data, size); unsigned char *data = map->data; int64_t root = test.root.payload;
            uint64_t requested = xr_xir_domain_stats(domain).allocations;
            before = calls; fail_at = before + site;
            CHECK(hash_fault_operation(&test) == XR_XIR_VALUE_OOM && calls > fail_at);
            fail_at = SIZE_MAX;
            hash_fault_unchanged(&test, root, bytes, data, size);
            CHECK(hash_fault_operation(&test) == XR_XIR_VALUE_OK && xr_xir_domain_stats(domain).allocations > requested);
            hash_fault_finish(&test);
        }
        for (uint64_t prefix = 0; prefix <= exact; ++prefix) {
            HashFaultCase test; hash_fault_setup(&test, operation, arena, domain);
            XirHash *map = (XirHash *)object_pointer(&test.root); unsigned char bytes[256];
            memcpy(bytes, map->data, sizeof(bytes)); unsigned char *data = map->data; int64_t root = test.root.payload;
            test.admission.work = prefix;
            XrXirValueStatus status = hash_fault_operation(&test);
            CHECK(status == (prefix == exact ? XR_XIR_VALUE_OK : XR_XIR_VALUE_LIMIT));
            if (prefix < exact) hash_fault_unchanged(&test, root, bytes, data, sizeof(bytes));
            hash_fault_finish(&test);
        }
        printf("hash operation%u actual physical sites=%zu local work=%llu all prefixes PASS\n",
            operation, sites, (unsigned long long)exact);
    }
}
#endif // P09_HASH_FAULTS_H
