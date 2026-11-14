/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * main.c - Fixed workloads for the standalone managed-layout experiment
 *
 * KEY CONCEPT:
 *   Whole-program outputs are checked; no timing or product runtime is embedded.
 */
#include "access.h"

static void mvl_layout(void) {
#define MVL_TYPE(type) printf(#type " size=%zu align=%zu", sizeof(type), (size_t) _Alignof(type))
#define MVL_FIELD(type, field) printf(" " #field "=%zu", offsetof(type, field))
    MVL_TYPE(XrMvlHeader);
    MVL_FIELD(XrMvlHeader, refs);
    printf(" refs_bits=%zu", sizeof(((XrMvlHeader *) 0)->refs) * CHAR_BIT);
    MVL_FIELD(XrMvlHeader, flags);
    MVL_FIELD(XrMvlHeader, allocation_bytes);
    MVL_FIELD(XrMvlHeader, domain);
    MVL_FIELD(XrMvlHeader, ops);
    puts("");
    MVL_TYPE(XrMvlArray);
    MVL_FIELD(XrMvlArray, header);
    MVL_FIELD(XrMvlArray, data);
    MVL_FIELD(XrMvlArray, length);
    MVL_FIELD(XrMvlArray, capacity);
    puts("");
    MVL_TYPE(XrMvlString);
    MVL_FIELD(XrMvlString, header);
    MVL_FIELD(XrMvlString, length);
    MVL_FIELD(XrMvlString, hash);
    MVL_FIELD(XrMvlString, bytes);
    puts("");
    MVL_TYPE(XrMvlMapSlot);
    MVL_FIELD(XrMvlMapSlot, hash);
    MVL_FIELD(XrMvlMapSlot, key);
    MVL_FIELD(XrMvlMapSlot, value);
    puts("");
    MVL_TYPE(XrMvlMap);
    MVL_FIELD(XrMvlMap, header);
    MVL_FIELD(XrMvlMap, slots);
    MVL_FIELD(XrMvlMap, length);
    MVL_FIELD(XrMvlMap, capacity);
    puts("");
    MVL_TYPE(XrMvlPair);
    MVL_FIELD(XrMvlPair, x);
    MVL_FIELD(XrMvlPair, y);
    puts("");
    MVL_TYPE(XrMvlPairObject);
    MVL_FIELD(XrMvlPairObject, header);
    MVL_FIELD(XrMvlPairObject, value);
    puts("");
    MVL_TYPE(XrMvlTypeOps);
    MVL_FIELD(XrMvlTypeOps, destroy);
    MVL_FIELD(XrMvlTypeOps, name);
    puts("");
    MVL_TYPE(XrMvlDomain);
    MVL_FIELD(XrMvlDomain, live_blocks);
    MVL_FIELD(XrMvlDomain, live_bytes);
    MVL_FIELD(XrMvlDomain, allocations);
    MVL_FIELD(XrMvlDomain, fail_after);
    puts("");
    MVL_TYPE(XrMvlArrayAccess);
    MVL_FIELD(XrMvlArrayAccess, owner);
    MVL_FIELD(XrMvlArrayAccess, data);
    MVL_FIELD(XrMvlArrayAccess, length);
    MVL_FIELD(XrMvlArrayAccess, capacity);
    MVL_FIELD(XrMvlArrayAccess, writable);
    puts("");
    MVL_TYPE(XrMvlArrayWrite);
    MVL_FIELD(XrMvlArrayWrite, status);
    MVL_FIELD(XrMvlArrayWrite, access);
    MVL_FIELD(XrMvlArrayWrite, previous);
    puts("");
#undef MVL_TYPE
#undef MVL_FIELD
}

static bool mvl_push(XrMvlDomain *domain, int64_t *result) {
    XrMvlArray *array = xr_mvl_array_new(domain);
    if (!array)
        return false;
    bool ok = true;
    for (int64_t i = 0; i < XR_MVL_PUSH_COUNT; ++i) {
        if (xr_mvl_array_push(&array, i) != XR_MVL_OK) {
            ok = false;
            break;
        }
    }
    int64_t total = 0;
    if (ok)
        for (size_t i = 0; i < array->length; ++i)
            total += array->data[i];
    xr_mvl_drop(&array->header);
    *result = total;
    return ok;
}

static bool mvl_push_prepared(XrMvlDomain *domain, int64_t *result) {
    XrMvlArray *array = xr_mvl_array_new(domain);
    if (!array)
        return false;
    bool ok = true;
    XrMvlArrayAccess access = xr_mvl_array_access_start(NULL);
    if (XR_MVL_PUSH_COUNT > 0) {
        int64_t value = 0;
        XrMvlArrayWrite write = xr_mvl_array_access_write(xr_mvl_array_access_start(array), 0, value, true);
        ok = write.status == XR_MVL_OK;
        if (ok) {
            array = write.access.owner;
            access = write.access;
            if (write.previous) xr_mvl_drop(&write.previous->header);
        }
    }
    for (int64_t i = 1; ok && i < XR_MVL_PUSH_COUNT; ++i) {
        XrMvlArrayWrite write = xr_mvl_array_access_push(access, i);
        if (write.status != XR_MVL_OK) {
            ok = false;
            break;
        }
        array = write.access.owner;
        access = write.access;
        if (write.previous) xr_mvl_drop(&write.previous->header);
    }
    access = xr_mvl_array_access_start(NULL);
    int64_t total = 0;
    if (ok)
        for (size_t i = 0; i < array->length; ++i)
            total += array->data[i];
    xr_mvl_drop(&array->header);
    *result = total;
    return ok;
}

static int64_t mvl_record(void) {
    int64_t total = 0;
    for (int64_t i = 0; i < XR_MVL_RECORD_COUNT; ++i) {
        const XrMvlPair pair = {i, i + 1};
        total += pair.x + pair.y;
    }
    return total;
}

static bool mvl_record_escape(XrMvlDomain *domain, int64_t *result) {
    int64_t total = 0;
    bool ok = true;
    for (int64_t i = 0; i < XR_MVL_RECORD_COUNT; ++i) {
        XrMvlPairObject *object = xr_mvl_pair_new(domain, (XrMvlPair) {i, i + 1});
        if (!object) {
            ok = false;
            break;
        }
        if (!xr_mvl_pair_escape(object)) {
            xr_mvl_drop(&object->header);
            ok = false;
            break;
        }
        total += object->value.x + object->value.y;
        xr_mvl_drop(&object->header);
    }
    xr_mvl_pair_escape_clear();
    *result = total;
    return ok;
}

static bool mvl_seed(XrMvlMap **map, XrMvlDomain *domain) {
    XrMvlString *seed = xr_mvl_string_new(domain, "seed", 4);
    if (!seed)
        return false;
    XrMvlStatus status = xr_mvl_map_set(map, seed, 0);
    xr_mvl_drop(&seed->header);
    return status == XR_MVL_OK;
}

static bool mvl_map(XrMvlDomain *domain, int64_t *result) {
    XrMvlMap *map = xr_mvl_map_new(domain);
    if (!map)
        return false;
    bool ok = mvl_seed(&map, domain);
    for (int64_t i = 0; ok && i < XR_MVL_MAP_COUNT; ++i) {
        char bytes[32];
        size_t length = xr_mvl_key_bytes((uint64_t) i, bytes);
        XrMvlString *key = xr_mvl_string_new(domain, bytes, length);
        if (!key) {
            ok = false;
            break;
        }
        ok = xr_mvl_map_set(&map, key, i) == XR_MVL_OK;
        xr_mvl_drop(&key->header);
    }
    int64_t total = 0;
    for (int64_t i = 0; ok && i < XR_MVL_MAP_COUNT; ++i) {
        char bytes[32];
        size_t length = xr_mvl_key_bytes((uint64_t) i, bytes);
        XrMvlString *key = xr_mvl_string_new(domain, bytes, length);
        if (!key) {
            ok = false;
            break;
        }
        int64_t value;
        ok = xr_mvl_map_get(map, key, &value);
        if (ok)
            total += value;
        xr_mvl_drop(&key->header);
    }
    xr_mvl_drop(&map->header);
    *result = total;
    return ok;
}

/* Keep the borrowed workload intact as a control. Only insertion ownership
 * differs; lookup still constructs and drops every temporary String. */
static bool mvl_map_take(XrMvlDomain *domain, int64_t *result) {
    XrMvlMap *map = xr_mvl_map_new(domain);
    if (!map)
        return false;
    XrMvlString *key = xr_mvl_string_new(domain, "seed", 4);
    bool ok = key && xr_mvl_map_set_take(&map, &key, 0) == XR_MVL_OK;
    if (key)
        xr_mvl_drop(&key->header);
    for (int64_t i = 0; ok && i < XR_MVL_MAP_COUNT; ++i) {
        char bytes[32];
        size_t length = xr_mvl_key_bytes((uint64_t) i, bytes);
        key = xr_mvl_string_new(domain, bytes, length);
        if (!key) {
            ok = false;
            break;
        }
        ok = xr_mvl_map_set_take(&map, &key, i) == XR_MVL_OK;
        if (key)
            xr_mvl_drop(&key->header);
    }
    int64_t total = 0;
    for (int64_t i = 0; ok && i < XR_MVL_MAP_COUNT; ++i) {
        char bytes[32];
        size_t length = xr_mvl_key_bytes((uint64_t) i, bytes);
        key = xr_mvl_string_new(domain, bytes, length);
        if (!key) {
            ok = false;
            break;
        }
        int64_t value;
        ok = xr_mvl_map_get(map, key, &value);
        if (ok)
            total += value;
        xr_mvl_drop(&key->header);
    }
    xr_mvl_drop(&map->header);
    *result = total;
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: managed <push|push-prepared|record|map|map-take|record-escape|verify|layout>\n");
        return 2;
    }
    if (strcmp(argv[1], "layout") == 0) {
        mvl_layout();
        return 0;
    }
    if (strcmp(argv[1], "verify") == 0) {
        if (!xr_mvl_verify())
            return 1;
        puts("verified");
        return 0;
    }
    static XrMvlDomain domain;
    xr_mvl_domain_init(&domain);
    int64_t total = 0, expected;
    bool ok;
    if (strcmp(argv[1], "push") == 0) {
        expected = INT64_C(12499997500000);
        ok = mvl_push(&domain, &total);
    } else if (strcmp(argv[1], "push-prepared") == 0) {
        expected = INT64_C(12499997500000);
        ok = mvl_push_prepared(&domain, &total);
    } else if (strcmp(argv[1], "record") == 0) {
        expected = INT64_C(9000000000000);
        total = mvl_record();
        ok = true;
    } else if (strcmp(argv[1], "map") == 0) {
        expected = INT64_C(19999900000);
        ok = mvl_map(&domain, &total);
    } else if (strcmp(argv[1], "map-take") == 0) {
        expected = INT64_C(19999900000);
        ok = mvl_map_take(&domain, &total);
    } else if (strcmp(argv[1], "record-escape") == 0) {
        expected = INT64_C(9000000000000);
        ok = mvl_record_escape(&domain, &total);
    } else {
        fprintf(stderr, "unknown workload: %s\n", argv[1]);
        return 2;
    }
    if (!ok || total != expected || domain.live_blocks || domain.live_bytes) {
        fprintf(stderr, "failed: value=%" PRId64 " blocks=%zu bytes=%zu\n",
                total, domain.live_blocks, domain.live_bytes);
        return 1;
    }
    printf("%" PRId64 "\n", total);
    return 0;
}
