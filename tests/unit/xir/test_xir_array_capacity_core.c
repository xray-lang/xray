/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_array_capacity_core.c - Owned capacity candidates and physical rollback
 *
 * KEY CONCEPT:
 *   Real allocation faults and finite ledgers preserve source owners and capacity.
 */
#define main original_value_allocation_main
#include "test_xir_value_allocations.c"
#undef main

typedef struct CapacityCore {
    XrXirDomain *domain, *reading;
    XrXirTypeArena *arena;
    XrXirValueAdmission admission;
} CapacityCore;

static CapacityCore capacity_core_new(uint64_t live_limit, uint64_t requested, uint64_t work) {
    CapacityCore f = {0};
    XrXirDomainBudgetControls controls = {requested, 65536, work, 65536, 65536};
    CHECK(xr_xir_domain_new_budgeted(live_limit, &controls, &f.domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &f.reading) == XR_XIR_VALUE_OK);
    XrXirCallableParameter empty_field = {XR_XIR_UNIT, 0};
    XrXirNominalIdentity empty = {.module = {"capacity", 8}, .name = {"Empty", 5},
        .exported = 1, .kind = XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable nominals = {.count = 1, .identities = &empty};
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)256},
        {.kind = XR_XIR_TYPE_NOMINAL},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)259},
        {.kind = XR_XIR_TYPE_TUPLE, .parameters = &empty_field, .parameter_count = 1},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)261}
    };
    XrXirTypes types = {nodes, 7, &nominals, NULL};
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(65536, 0, 10000), &f.arena) == XR_XIR_VALUE_OK);
    f.admission = (XrXirValueAdmission){f.arena, f.domain, NULL, NULL, 65536, 65536};
    return f;
}
static void capacity_core_free(CapacityCore *f) {
    CHECK(xr_xir_domain_stats(f->domain).live_bytes == sizeof(XrXirDomain));
    CHECK(xr_xir_domain_stats(f->reading).live_bytes == sizeof(XrXirDomain));
    xr_xir_compile_type_arena_drop(f->arena);
    xr_xir_domain_drop(f->domain); xr_xir_domain_drop(f->reading);
    CHECK(!live && !value_compile_live && !value_compile_bytes);
}
static int64_t capacity_core_capacity(CapacityCore *f, const XrXirValue *array) {
    XrXirValueAdmission read = {f->arena, f->reading, NULL, NULL, 65536, 65536};
    int64_t result = -1; size_t allocations = calls;
    CHECK(xr_xir_array_capacity(array, &read, &result) == XR_XIR_VALUE_OK && calls == allocations);
    return result;
}
static void capacity_core_i64(CapacityCore *f, const XrXirValue *array, const int64_t *values, size_t count) {
    XrXirValueAdmission read = {f->arena, f->reading, NULL, NULL, 65536, 65536};
    int64_t length = -1;
    CHECK(xr_xir_array_len(array, &read, &length) == XR_XIR_VALUE_OK && length == (int64_t)count);
    CHECK(capacity_core_capacity(f, array) >= length);
    for (size_t i = 0; i < count; ++i) {
        XrXirValue element = {0}; XrXirFaultDetail fault = {0};
        CHECK(xr_xir_array_get(array, (int64_t)i, &read, &element, &fault) == XR_XIR_VALUE_OK);
        CHECK(element.type == XR_XIR_I64 && !element.reserved && element.payload == values[i]);
        CHECK(xr_xir_fault_empty(fault)); xr_xir_value_drop(&element);
    }
}
static void capacity_core_owners(void) {
    CapacityCore f = capacity_core_new(65536, 65536, 65536);
    XrXirValue array = {0}, alias = {0}, candidate = {0};
    CHECK(xr_xir_array_with_capacity((XrXirType)256, 17, &f.admission, &array) == XR_XIR_VALUE_OK);
    capacity_core_i64(&f, &array, NULL, 0); int64_t reserved = capacity_core_capacity(&f, &array);
    CHECK(reserved >= 17);
    XrXirValue one = {XR_XIR_I64, 0, 1}, two = {XR_XIR_I64, 0, 2}, nine = {XR_XIR_I64, 0, 9};
    XrXirValuePlace place = {(XrXirType)256, &array.payload}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_push(&place, &one, &f.admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&array, &alias) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_push(&place, &two, &f.admission) == XR_XIR_VALUE_OK);
    CHECK(capacity_core_capacity(&f, &array) >= reserved && capacity_core_capacity(&f, &alias) == reserved);
    int64_t original[] = {1}, appended[] = {1, 2};
    capacity_core_i64(&f, &array, appended, 2); capacity_core_i64(&f, &alias, original, 1);
    XirObject *before = object_pointer(&array);
    CHECK(xr_xir_array_reserve(&array, 0, &f.admission, &candidate) == XR_XIR_VALUE_OK);
    CHECK(object_pointer(&array) == before && capacity_core_capacity(&f, &candidate) >= reserved);
    XrXirValuePlace returned = {(XrXirType)256, &candidate.payload};
    CHECK(xr_xir_array_set(&returned, 0, &nine, &f.admission, &fault) == XR_XIR_VALUE_OK);
    int64_t replaced[] = {9, 2};
    capacity_core_i64(&f, &candidate, replaced, 2); capacity_core_i64(&f, &array, appended, 2);
    xr_xir_value_drop(&candidate);
    CHECK(xr_xir_value_copy(&array, &candidate) == XR_XIR_VALUE_OK);
    f.admission.domain = f.reading;
    CHECK(xr_xir_array_set(&place, 0, &nine, &f.admission, &fault) == XR_XIR_VALUE_OK);
    CHECK(object_pointer(&array)->domain == f.reading && capacity_core_capacity(&f, &array) >= reserved);
    capacity_core_i64(&f, &array, replaced, 2); capacity_core_i64(&f, &candidate, appended, 2);
    xr_xir_value_drop(&array); xr_xir_value_drop(&alias); xr_xir_value_drop(&candidate); capacity_core_free(&f);
}
static void capacity_core_nested(void) {
    CapacityCore f = capacity_core_new(65536, 65536, 65536);
    XrXirValue inner = {0}, outer = {0}, alias = {0}, read = {0}, one = {XR_XIR_I64, 0, 1};
    CHECK(xr_xir_array_with_capacity((XrXirType)256, 13, &f.admission, &inner) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_with_capacity((XrXirType)258, 19, &f.admission, &outer) == XR_XIR_VALUE_OK);
    XrXirValuePlace root = {(XrXirType)258, &outer.payload};
    CHECK(xr_xir_array_push(&root, &inner, &f.admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&outer, &alias) == XR_XIR_VALUE_OK);
    XrXirValuePathStep step = {XR_XIR_PATH_INDEX, (XrXirType)258, 0};
    XrXirValuePath path = {&step, 1}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_value_path_push(&root, &path, &one, &f.admission, &fault) == XR_XIR_VALUE_OK);
    CHECK(capacity_core_capacity(&f, &outer) >= 19 && capacity_core_capacity(&f, &alias) >= 19);
    CHECK(xr_xir_value_path_read(&root, &path, &f.admission, &read, &fault) == XR_XIR_VALUE_OK);
    CHECK(capacity_core_capacity(&f, &read) >= 13); int64_t expected[] = {1};
    capacity_core_i64(&f, &read, expected, 1); capacity_core_i64(&f, &inner, NULL, 0);
    xr_xir_value_drop(&read);
    XrXirValuePlace old = {(XrXirType)258, &alias.payload};
    CHECK(xr_xir_value_path_read(&old, &path, &f.admission, &read, &fault) == XR_XIR_VALUE_OK);
    capacity_core_i64(&f, &read, NULL, 0); CHECK(capacity_core_capacity(&f, &read) >= 13);
    xr_xir_value_drop(&read); xr_xir_value_drop(&outer); xr_xir_value_drop(&inner); xr_xir_value_drop(&alias);
    capacity_core_free(&f);
}
static void capacity_core_empty_element(void) {
    CapacityCore f = capacity_core_new(65536, 65536, 65536);
    XrXirValue array = {0}, element = {0}, copy = {0};
    int64_t bound = SIZE_MAX < (uint64_t)INT64_MAX ? (int64_t)SIZE_MAX : INT64_MAX;
    XrXirType stored = XR_XIR_UNIT; uint32_t stride = UINT32_MAX;
    CHECK(array_layout(f.arena, (XrXirType)262, &stored, &stride) && stored == (XrXirType)261 && stride > 1);
    XrXirValue rejected = {0}; size_t allocations = calls;
    CHECK(xr_xir_array_with_capacity((XrXirType)262, bound, &f.admission, &rejected) == XR_XIR_VALUE_LIMIT);
    CHECK(calls == allocations && !rejected.type && !rejected.reserved && !rejected.payload);
    CHECK(array_layout(f.arena, (XrXirType)260, &stored, &stride) && stored == (XrXirType)259 && stride == 0);
    CHECK(xr_xir_array_with_capacity((XrXirType)260, bound, &f.admission, &array) == XR_XIR_VALUE_OK);
    CHECK(capacity_core_capacity(&f, &array) >= bound);
    CHECK(xr_xir_struct_new((XrXirType)259, NULL, 0, &f.admission, &element) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType)260, &array.payload};
    CHECK(xr_xir_array_push(&place, &element, &f.admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_reserve(&array, bound, &f.admission, &copy) == XR_XIR_VALUE_OK);
    CHECK(capacity_core_capacity(&f, &copy) == bound);
    XirArray *storage = (XirArray *)object_pointer(&copy);
    CHECK(storage->length == 1 && !storage->stride && !storage->data);
    xr_xir_value_drop(&array); xr_xir_value_drop(&element); xr_xir_value_drop(&copy); capacity_core_free(&f);
}
static void capacity_core_rejections(void) {
    CapacityCore f = capacity_core_new(65536, 65536, 65536); XrXirValue array = {0}, out = {0};
    CHECK(xr_xir_array_with_capacity((XrXirType)256, 3, &f.admission, &array) == XR_XIR_VALUE_OK);
    size_t before = calls;
    CHECK(xr_xir_array_with_capacity((XrXirType)256, -1, &f.admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_array_reserve(&array, -1, &f.admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_array_with_capacity((XrXirType)256, INT64_MAX, &f.admission, &out) == XR_XIR_VALUE_LIMIT);
    CHECK(xr_xir_array_reserve(&array, INT64_MAX, &f.admission, &out) == XR_XIR_VALUE_LIMIT);
    CHECK(calls == before && !out.type && !out.reserved && !out.payload);
    out = (XrXirValue){XR_XIR_I64, 0, 71}; XrXirValue preserved = out;
    CHECK(xr_xir_array_with_capacity((XrXirType)256, 1, &f.admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_array_reserve(&array, 1, &f.admission, &out) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!memcmp(&out, &preserved, sizeof(out)) && calls == before);
    f.admission.work = 0; int64_t capacity = 71;
    CHECK(xr_xir_array_capacity(&array, &f.admission, &capacity) == XR_XIR_VALUE_LIMIT && capacity == 71);
    xr_xir_value_drop(&out); xr_xir_value_drop(&array); capacity_core_free(&f);
}
static void capacity_core_no_growth(void) {
    CapacityCore f = capacity_core_new(65536, 65536, 65536);
    XrXirValue source = {0}, copy = {0};
    const XrXirValue values[] = {{XR_XIR_I64, 0, 1}, {XR_XIR_I64, 0, 2}};
    CHECK(xr_xir_array_new((XrXirType)256, values, 2, &f.admission, &source) == XR_XIR_VALUE_OK);
    int64_t capacity = capacity_core_capacity(&f, &source);
    XrXirDomainStats before = xr_xir_domain_stats(f.domain);
    XrXirDomainBudgetStats charged = xr_xir_domain_budget_stats(f.domain);
    size_t allocations = calls; fail_at = calls;
    CHECK(xr_xir_array_reserve(&source, capacity, &f.admission, &copy) == XR_XIR_VALUE_OK);
    fail_at = SIZE_MAX;
    CHECK(copy.payload == source.payload && calls == allocations);
    CHECK(xr_xir_domain_stats(f.domain).live_bytes == before.live_bytes);
    CHECK(xr_xir_domain_budget_stats(f.domain).requested_bytes == charged.requested_bytes);
    XrXirValuePlace place = {(XrXirType)256, &copy.payload};
    XrXirValue nine = {XR_XIR_I64, 0, 9}; XrXirFaultDetail fault = {0};
    allocations = calls; fail_at = calls;
    CHECK(xr_xir_array_set(&place, 0, &nine, &f.admission, &fault) == XR_XIR_VALUE_OOM);
    fail_at = SIZE_MAX;
    CHECK(copy.payload == source.payload && calls > allocations);
    const int64_t original[] = {1, 2}, changed[] = {9, 2};
    capacity_core_i64(&f, &source, original, 2); capacity_core_i64(&f, &copy, original, 2);
    CHECK(xr_xir_array_set(&place, 0, &nine, &f.admission, &fault) == XR_XIR_VALUE_OK);
    capacity_core_i64(&f, &source, original, 2); capacity_core_i64(&f, &copy, changed, 2);
    CHECK(capacity_core_capacity(&f, &copy) == capacity);
    xr_xir_value_drop(&copy);
    f.admission.domain = f.reading;
    CHECK(xr_xir_array_reserve(&source, 0, &f.admission, &copy) == XR_XIR_VALUE_OK);
    CHECK(copy.payload != source.payload && object_pointer(&copy)->domain == f.reading);
    capacity_core_i64(&f, &copy, original, 2);
    xr_xir_value_drop(&copy); f.admission.domain = f.domain;
    uint64_t repeated_allocations = xr_xir_domain_stats(f.domain).allocations;
    uint64_t repeated_requests = xr_xir_domain_budget_stats(f.domain).requested_bytes;
    for (unsigned i = 0; i < 12000; ++i) {
        CHECK(xr_xir_array_reserve(&source, 0, &f.admission, &copy) == XR_XIR_VALUE_OK);
        CHECK(copy.payload == source.payload); xr_xir_value_drop(&copy);
    }
    CHECK(xr_xir_domain_stats(f.domain).allocations == repeated_allocations);
    CHECK(xr_xir_domain_budget_stats(f.domain).requested_bytes == repeated_requests);
    xr_xir_value_drop(&source); capacity_core_free(&f);
}
static void capacity_core_no_growth_admission(void) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        CapacityCore f = capacity_core_new(65536, 65536, 65536);
        XrXirValue source = {0}, copy = {0}, text = {0};
        CHECK(xr_xir_string_new(f.domain, "owned", 5, &text) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_array_new((XrXirType)257, &text, 1, &f.admission, &source) == XR_XIR_VALUE_OK);
        XirObject *root = object_pointer(&source), *element = object_pointer(&text);
        uint32_t root_refs = atomic_load(&root->references), element_refs = atomic_load(&element->references);
        size_t allocations = calls;
        if (mode == 1) f.admission.work = 0;
        if (mode == 2) atomic_store(&root->references, UINT32_MAX);
        XrXirValueStatus status = xr_xir_array_reserve(&source, 0, &f.admission, &copy);
        CHECK(status == (mode == 1 ? XR_XIR_VALUE_LIMIT : mode == 2 ? XR_XIR_VALUE_REFCOUNT_LIMIT : XR_XIR_VALUE_OK));
        CHECK(calls == allocations && atomic_load(&element->references) == element_refs);
        if (!mode) { CHECK(copy.payload == source.payload); xr_xir_value_drop(&copy); }
        else CHECK(!copy.type && !copy.reserved && !copy.payload);
        if (mode == 2) atomic_store(&root->references, root_refs);
        xr_xir_value_drop(&source); xr_xir_value_drop(&text); capacity_core_free(&f);
    }
}
static size_t capacity_core_fault(unsigned mode, size_t offset) {
    CapacityCore f = capacity_core_new(65536, 65536, 65536);
    XrXirValue source = {0}, text = {0}, alias = {0}, out = {0};
    CHECK(xr_xir_string_new(f.domain, "A\0B", 3, &text) == XR_XIR_VALUE_OK);
    XrXirValue values[] = {text, text};
    CHECK(xr_xir_array_new((XrXirType)257, values, 2, &f.admission, &source) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&source, &alias) == XR_XIR_VALUE_OK);
    XirObject *identity = object_pointer(&source); int64_t old_capacity = capacity_core_capacity(&f, &source);
    uint32_t references = atomic_load(&object_pointer(&text)->references);
    if (mode == 3) f.admission.domain = f.reading;
    size_t start = calls, physical = live;
    uint64_t bytes = xr_xir_domain_stats(f.domain).live_bytes;
    uint64_t reader_bytes = xr_xir_domain_stats(f.reading).live_bytes;
    fail_at = offset == SIZE_MAX ? SIZE_MAX : start + offset;
    XrXirValueStatus status = mode == 0 ?
        xr_xir_array_with_capacity((XrXirType)257, 31, &f.admission, &out) :
        xr_xir_array_reserve(&source, mode == 1 ? 31 : 0, &f.admission, &out);
    size_t count = calls - start; fail_at = SIZE_MAX;
    if (offset == SIZE_MAX) CHECK(status == XR_XIR_VALUE_OK && out.type == 257);
    else {
        CHECK(status == XR_XIR_VALUE_OOM && !out.type && !out.reserved && !out.payload);
        CHECK(live == physical && xr_xir_domain_stats(f.domain).live_bytes == bytes);
        CHECK(xr_xir_domain_stats(f.reading).live_bytes == reader_bytes);
        CHECK(atomic_load(&object_pointer(&text)->references) == references);
    }
    CHECK(object_pointer(&source) == identity && capacity_core_capacity(&f, &source) == old_capacity);
    XirArray *backing = (XirArray *)identity;
    CHECK(backing->length == 2);
    for (size_t i = 0; i < 2; ++i) {
        XrXirValue value = storage_leaf_value(XR_XIR_STRING, array_slot(backing, i), backing->stride);
        const char *data = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&value, &data, &length) && length == 3 && !memcmp(data, "A\0B", 3));
    }
    xr_xir_value_drop(&out); xr_xir_value_drop(&alias); xr_xir_value_drop(&source); xr_xir_value_drop(&text);
    capacity_core_free(&f); return count;
}
static void capacity_core_retain_failure(void) {
    CapacityCore f = capacity_core_new(65536, 65536, 65536);
    XrXirValue text = {0}, source = {0}, out = {0};
    CHECK(xr_xir_string_new(f.domain, "owned", 5, &text) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType)257, &text, 1, &f.admission, &source) == XR_XIR_VALUE_OK);
    XirObject *object = object_pointer(&text); uint32_t count = atomic_load(&object->references);
    size_t physical = live; uint64_t bytes = xr_xir_domain_stats(f.domain).live_bytes;
    atomic_store(&object->references, UINT32_MAX);
    CHECK(xr_xir_array_reserve(&source, 31, &f.admission, &out) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!out.type && !out.payload && live == physical && xr_xir_domain_stats(f.domain).live_bytes == bytes);
    CHECK(atomic_load(&object->references) == UINT32_MAX);
    atomic_store(&object->references, count);
    xr_xir_value_drop(&source); xr_xir_value_drop(&text); capacity_core_free(&f);
}
static void capacity_core_ledgers(void) {
    CapacityCore probe = capacity_core_new(65536, 65536, 65536);
    XrXirDomainBudgetStats base = xr_xir_domain_budget_stats(probe.domain); capacity_core_free(&probe);
    uint64_t bytes = sizeof(XirArray) + 7 * sizeof(int64_t);
    for (unsigned test = 0; test < 5; ++test) {
        uint64_t requested = base.requested_bytes + bytes - (test == 1 ? 1u : 0u);
        uint64_t work = base.work + 3 - (test == 2 ? 1u : 0u);
        uint64_t limit = sizeof(XrXirDomain) + bytes - (test == 3 ? 1u : 0u);
        CapacityCore f = capacity_core_new(limit, requested, work); XrXirValue out = {0};
        size_t start = calls; fail_at = test == 4 ? calls + 1 : SIZE_MAX;
        XrXirValueStatus status = xr_xir_array_with_capacity((XrXirType)256, 7, &f.admission, &out);
        fail_at = SIZE_MAX;
        if (!test) {
            CHECK(status == XR_XIR_VALUE_OK && calls == start + 2);
            CHECK(capacity_core_capacity(&f, &out) >= 7);
            XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(f.domain);
            CHECK(spent.requested_bytes == requested && spent.work == work);
        } else {
            CHECK(status == (test == 4 ? XR_XIR_VALUE_OOM : XR_XIR_VALUE_LIMIT));
            CHECK(!out.type && !out.reserved && !out.payload && xr_xir_domain_stats(f.domain).live_bytes == sizeof(XrXirDomain));
            if (test == 4) {
                XrXirDomainBudgetStats spent = xr_xir_domain_budget_stats(f.domain);
                CHECK(spent.requested_bytes == requested && spent.work == work);
                size_t attempts = calls;
                CHECK(xr_xir_array_with_capacity((XrXirType)256, 7, &f.admission, &out) == XR_XIR_VALUE_LIMIT);
                CHECK(calls == attempts && xr_xir_domain_budget_stats(f.domain).requested_bytes == requested);
            }
        }
        xr_xir_value_drop(&out); capacity_core_free(&f);
    }
}
int main(int argc, char **argv) {
    fail_at = SIZE_MAX;
    capacity_core_no_growth(); capacity_core_no_growth_admission();
    if (argc == 2 && !strcmp(argv[1], "--cow")) return 0;
    CHECK(argc == 1);
    capacity_core_owners(); capacity_core_nested(); capacity_core_empty_element(); capacity_core_rejections();
    for (unsigned mode = 0; mode < 4; ++mode) {
        size_t count = capacity_core_fault(mode, SIZE_MAX);
        CHECK(mode == 2 ? count == 0 : count > 0);
        printf("capacity allocation mode%u sites=%zu\n", mode, count);
        for (size_t site = 0; site < count; ++site) (void)capacity_core_fault(mode, site);
    }
    capacity_core_retain_failure(); capacity_core_ledgers(); CHECK(original_value_allocation_main(1, argv) == 0);
    puts("Array capacity owned/cross-domain/nested/zero-sized/all-OOM/retain/exact-minus1/charged-retry physical0 PASS");
    return 0;
}
