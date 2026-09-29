/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_allocation_cases.h - Compact ownership and physical failure rollback
 */
#ifndef XIR_ARRAY_ALLOCATION_CASES_H
#define XIR_ARRAY_ALLOCATION_CASES_H
static XrXirTypeArena *array_allocation_arena(XrXirDomain *domain,
                                            const XrXirTypeNode *nodes, uint32_t count) {
    XrXirTypes types = {nodes, count, NULL}; XrXirTypeArena *arena = NULL;
    XrXirBudget budget = {0}; budget.work = UINT64_MAX; budget.metadata_bytes = 1024 * 1024;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static void array_compact_payloads(void) {
    const XrXirValue values[] = {
        {XR_XIR_BOOL, 0, 1}, {XR_XIR_I8, 0, -128}, {XR_XIR_I16, 0, -32768},
        {XR_XIR_I32, 0, INT32_MIN}, {XR_XIR_I64, 0, INT64_MIN},
        {XR_XIR_U8, 0, 255}, {XR_XIR_U16, 0, 65535}, {XR_XIR_U32, 0, UINT32_MAX},
        {XR_XIR_U64, 0, -1}, {XR_XIR_F32, 0, 0x80000000},
        {XR_XIR_F32, 0, 0x7fc00000}, {XR_XIR_F64, 0, INT64_MIN},
        {XR_XIR_F64, 0, INT64_C(0x7ff8000000000000)}
    };
    const uint32_t widths[] = {1, 1, 2, 4, 8, 1, 2, 4, 8, 4, 4, 8, 8};
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); ++i) {
        XrXirDomain *domain = NULL; fail_at = SIZE_MAX;
        CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
        XrXirTypeNode node = {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType) values[i].type};
        XrXirTypeArena *arena = array_allocation_arena(domain, &node, 1);
        XrXirValueAdmission admission = {arena, domain, NULL, NULL, 10000, 0};
        XrXirValue array = {0}, read = {0}; XrXirFaultDetail fault = {0};
        uint64_t bytes = domain->stats.live_bytes;
        CHECK(xr_xir_array_new((XrXirType) 256, values + i, 1, &admission, &array) == XR_XIR_VALUE_OK);
        XirArray *backing = (XirArray *) object_pointer(&array);
        CHECK(backing->stride == widths[i] && backing->length == 1);
        CHECK(domain->stats.live_bytes == bytes + sizeof(XirArray) + backing->capacity * widths[i]);
        CHECK(xr_xir_array_get(&array, 0, &admission, &read, &fault) == XR_XIR_VALUE_OK);
        CHECK(read.type == values[i].type && read.payload == values[i].payload && !read.reserved);
        xr_xir_value_drop(&read); xr_xir_value_drop(&array);
        xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain); CHECK(!live);
    }
}
static size_t array_allocation_failure(unsigned mode, size_t offset) {
    fail_at = SIZE_MAX;
    XrXirDomain *domain = NULL, *receiver = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &receiver) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = allocation_arena(domain);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, UINT64_MAX, 65536};
    XrXirValue string = {0}, array = {0}, copy = {0}, output = {0};
    CHECK(xr_xir_string_new(domain, "alias", 5, &string) == XR_XIR_VALUE_OK);
    XrXirValue inputs[] = {string, string, string, string};
    size_t initial_count = mode == 4 ? 0 : mode >= 2 ? 4 : 1;
    CHECK(xr_xir_array_new((XrXirType) 260, inputs, initial_count, &admission, &array) == XR_XIR_VALUE_OK);
    if (mode == 1) CHECK(xr_xir_value_copy(&array, &copy) == XR_XIR_VALUE_OK);
    if (mode == 3) admission.domain = receiver;
    XirArray *original = (XirArray *) object_pointer(&array);
    XrXirValuePlace place = {(XrXirType) 260, &array.payload};
    size_t start = calls, physical = live, old_length = original->length, old_capacity = original->capacity;
    uint64_t bytes = domain->stats.live_bytes, receiver_bytes = receiver->stats.live_bytes;
    uint32_t references = atomic_load(&object_pointer(&string)->references);
    fail_at = offset == SIZE_MAX ? SIZE_MAX : start + offset;
    XrXirValueStatus status = mode == 0 ? xr_xir_array_new((XrXirType) 260, inputs, 4, &admission, &output) :
        xr_xir_array_push(&place, &string, &admission);
    size_t count = calls - start; fail_at = SIZE_MAX;
    if (offset == SIZE_MAX) {
        CHECK(status == XR_XIR_VALUE_OK);
        if (mode) CHECK(((XirArray *) object_pointer(&array))->length == old_length + 1);
    } else {
        CHECK(status == XR_XIR_VALUE_OOM && !output.type);
        CHECK(object_pointer(&array) == &original->object && original->length == old_length && original->capacity == old_capacity);
        CHECK(live == physical && domain->stats.live_bytes == bytes && receiver->stats.live_bytes == receiver_bytes);
        CHECK(atomic_load(&object_pointer(&string)->references) == references);
        for (size_t i = 0; i < original->length; ++i)
            CHECK(storage_leaf_value(original->element, original->data + i * original->stride, original->stride).payload == string.payload);
    }
    xr_xir_value_drop(&output); xr_xir_value_drop(&copy); xr_xir_value_drop(&array); xr_xir_value_drop(&string);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain); xr_xir_domain_drop(receiver); CHECK(!live);
    return count;
}
static void array_allocation_failures(void) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        size_t count = array_allocation_failure(mode, SIZE_MAX);
        CHECK(count == (mode == 2 || mode == 4 ? 1u : 2u));
        for (size_t offset = 0; offset < count; ++offset) (void) array_allocation_failure(mode, offset);
    }
}
static void array_cross_domain_accounting(void) {
    fail_at = SIZE_MAX;
    XrXirDomain *origin = NULL, *receiver = NULL;
    CHECK(xr_xir_domain_new(65536, &origin) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &receiver) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = allocation_arena(origin);
    XrXirValueAdmission admission = {arena, origin, NULL, NULL, UINT64_MAX, 65536};
    XrXirValue string = {0}, array = {0}, copy = {0};
    CHECK(xr_xir_string_new(origin, "held", 4, &string) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType) 260, &string, 1, &admission, &array) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType) 260, &array.payload};
    XirArray *original = (XirArray *) object_pointer(&array);
    CHECK(original->stride == 8 && original->capacity == 4);
    admission.domain = receiver;
    origin->limit = origin->stats.live_bytes; receiver->limit = receiver->stats.live_bytes;
    size_t allocations = calls;
    for (unsigned i = 0; i < 3; ++i) CHECK(xr_xir_array_push(&place, &string, &admission) == XR_XIR_VALUE_OK);
    CHECK(calls == allocations && object_pointer(&array) == &original->object);
    uint64_t origin_bytes = origin->stats.live_bytes, receiver_bytes = receiver->stats.live_bytes;
    CHECK(xr_xir_array_push(&place, &string, &admission) == XR_XIR_VALUE_LIMIT);
    CHECK(original->length == 4 && origin->stats.live_bytes == origin_bytes && receiver->stats.live_bytes == receiver_bytes);
    receiver->limit = 65536;
    CHECK(xr_xir_array_push(&place, &string, &admission) == XR_XIR_VALUE_OK);
    XirArray *moved = (XirArray *) object_pointer(&array);
    CHECK(moved != original && moved->object.domain == receiver && moved->length == 5);
    CHECK(origin->stats.live_bytes == origin_bytes - sizeof(XirArray) - 4 * 8);
    CHECK(receiver->stats.live_bytes == receiver_bytes + sizeof(XirArray) + moved->capacity * 8);
    CHECK(xr_xir_value_copy(&array, &copy) == XR_XIR_VALUE_OK);
    admission.domain = origin; origin->limit = origin->stats.live_bytes;
    XrXirFaultDetail fault = {0}; allocations = calls;
    CHECK(xr_xir_array_set(&place, 0, &string, &admission, &fault) == XR_XIR_VALUE_LIMIT);
    CHECK(array.payload == copy.payload && calls == allocations);
    origin->limit = 65536;
    CHECK(xr_xir_array_set(&place, 0, &string, &admission, &fault) == XR_XIR_VALUE_OK);
    CHECK(array.payload != copy.payload && object_pointer(&array)->domain == origin);
    xr_xir_value_drop(&copy); xr_xir_value_drop(&array); xr_xir_value_drop(&string);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(origin); xr_xir_domain_drop(receiver); CHECK(!live);
}
static void array_retain_atomicity(void) {
    fail_at = SIZE_MAX; XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = allocation_arena(domain);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, UINT64_MAX, 65536};
    XrXirValue strings[2] = {{0}}, array = {0}, copy = {0}, rejected = {0};
    CHECK(xr_xir_string_new(domain, "first", 5, strings) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "second", 6, strings + 1) == XR_XIR_VALUE_OK);
    uint64_t bytes = domain->stats.live_bytes; size_t physical = live;
    atomic_store(&object_pointer(strings + 1)->references, UINT32_MAX);
    CHECK(xr_xir_array_new((XrXirType) 260, strings, 2, &admission, &rejected) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!rejected.type && live == physical && domain->stats.live_bytes == bytes);
    CHECK(atomic_load(&object_pointer(strings)->references) == 1);
    atomic_store(&object_pointer(strings + 1)->references, 1);
    CHECK(xr_xir_array_new((XrXirType) 260, strings, 2, &admission, &array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&array, &copy) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType) 260, &array.payload}; XrXirFaultDetail fault = {0};
    bytes = domain->stats.live_bytes; physical = live;
    atomic_store(&object_pointer(strings + 1)->references, UINT32_MAX);
    CHECK(xr_xir_array_push(&place, strings, &admission) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(array.payload == copy.payload && live == physical && domain->stats.live_bytes == bytes);
    CHECK(atomic_load(&object_pointer(strings)->references) == 2);
    CHECK(xr_xir_array_set(&place, 0, strings, &admission, &fault) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(array.payload == copy.payload && live == physical && domain->stats.live_bytes == bytes);
    CHECK(atomic_load(&object_pointer(strings)->references) == 2 && xr_xir_fault_empty(fault));
    CHECK(xr_xir_array_set(&place, 1, strings, &admission, &fault) == XR_XIR_VALUE_OK);
    CHECK(array.payload != copy.payload);
    atomic_store(&object_pointer(strings + 1)->references, 2);
    xr_xir_value_drop(&array); xr_xir_value_drop(&copy);
    atomic_store(&arena->references, UINT32_MAX);
    CHECK(xr_xir_array_new((XrXirType) 260, strings, 1, &admission, &rejected) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!rejected.type); atomic_store(&arena->references, 1);
    uint32_t domain_references = atomic_load(&domain->references);
    atomic_store(&domain->references, UINT32_MAX);
    CHECK(xr_xir_array_new((XrXirType) 260, strings, 1, &admission, &rejected) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(!rejected.type); atomic_store(&domain->references, domain_references);
    xr_xir_value_drop(strings); xr_xir_value_drop(strings + 1);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain); CHECK(!live);
}
static void array_nested_gate_scratch(void) {
    fail_at = SIZE_MAX; XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType) 256},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType) 257},
    };
    XrXirTypeArena *arena = array_allocation_arena(domain, nodes, 3); size_t releases = 0;
    XrXirValueAdmission admission = allocation_admission(domain, arena, &releases);
    admission.scratch_bytes = 65536;
    XrXirFunctionBinding binding = {&releases, capture_release, 0, NULL, 0};
    XrXirValue function = {0}, array = {0}, nested = {0};
    CHECK(xr_xir_function_new(domain, arena, (XrXirType) 256, &binding, &admission, &function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType) 257, &function, 1, &admission, &array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType) 258, &array, 1, &admission, &nested) == XR_XIR_VALUE_OK);
    size_t physical = live; uint64_t bytes = domain->stats.live_bytes, scratch = admission.scratch_bytes;
    fail_at = calls;
    CHECK(xr_xir_value_admit(&nested, (XrXirType) 258, &admission) == XR_XIR_VALUE_OOM);
    fail_at = SIZE_MAX;
    CHECK(admission.scratch_bytes == scratch && live == physical && domain->stats.live_bytes == bytes);
    admission.scratch_bytes = sizeof(ValueAdmissionFrame) - 1;
    size_t allocations = calls;
    CHECK(xr_xir_value_admit(&nested, (XrXirType) 258, &admission) == XR_XIR_VALUE_LIMIT && calls == allocations);
    admission.scratch_bytes = 3 * sizeof(ValueAdmissionFrame) - 1;
    CHECK(xr_xir_value_admit(&nested, (XrXirType) 258, &admission) == XR_XIR_VALUE_LIMIT && calls == allocations + 1);
    CHECK(admission.scratch_bytes == 3 * sizeof(ValueAdmissionFrame) - 1 && live == physical && domain->stats.live_bytes == bytes);
    admission.scratch_bytes = scratch;
    admission.work = 5;
    CHECK(xr_xir_value_admit(&nested, (XrXirType) 258, &admission) == XR_XIR_VALUE_LIMIT);
    CHECK(admission.scratch_bytes == scratch && live == physical && domain->stats.live_bytes == bytes);
    admission.work = 100;
    CHECK(xr_xir_value_admit(&nested, (XrXirType) 258, &admission) == XR_XIR_VALUE_OK);
    CHECK(admission.work < 100 && admission.scratch_bytes == scratch && live == physical && domain->stats.live_bytes == bytes);
    atomic_store(&object_pointer(&nested)->references, UINT32_MAX);
    int64_t length = -1;
    CHECK(xr_xir_array_len(&nested, &admission, &length) == XR_XIR_VALUE_OK && length == 1);
    atomic_store(&object_pointer(&nested)->references, 1);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
    xr_xir_value_drop(&function); xr_xir_value_drop(&array); xr_xir_value_drop(&nested);
    CHECK(releases == 1 && !live);
}
static void array_copy_work_budget(void) {
    fail_at = SIZE_MAX; XrXirDomain *domain = NULL, *receiver = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &receiver) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = allocation_arena(domain);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, UINT64_MAX, 65536};
    XrXirValue string = {0}, array = {0}, copy = {0}, inputs[64];
    CHECK(xr_xir_string_new(domain, "work", 4, &string) == XR_XIR_VALUE_OK);
    for (size_t i = 0; i < 64; ++i) inputs[i] = string;
    for (unsigned mode = 0; mode < 3; ++mode) {
        admission.domain = domain; admission.work = UINT64_MAX;
        CHECK(xr_xir_array_new((XrXirType) 260, inputs, 64, &admission, &array) == XR_XIR_VALUE_OK);
        if (!mode) CHECK(xr_xir_value_copy(&array, &copy) == XR_XIR_VALUE_OK);
        else if (mode == 1) admission.domain = receiver;
        XrXirValuePlace place = {(XrXirType) 260, &array.payload};
        int64_t payload = array.payload; size_t physical = live, allocations = calls;
        uint64_t bytes = domain->stats.live_bytes, receiver_bytes = receiver->stats.live_bytes;
        XrXirDomainStats before = domain->stats, receiving_before = receiver->stats;
        uint32_t references = atomic_load(&object_pointer(&string)->references);
        /* Receiver + two descriptor nodes + RHS + 64 copied/moved elements. */
        admission.work = 67;
        CHECK(xr_xir_array_push(&place, &string, &admission) == XR_XIR_VALUE_LIMIT);
        CHECK(array.payload == payload && live == physical && calls == allocations);
        CHECK(domain->stats.live_bytes == bytes && receiver->stats.live_bytes == receiver_bytes);
        CHECK(!memcmp(&before, &domain->stats, sizeof(before)) &&
              !memcmp(&receiving_before, &receiver->stats, sizeof(receiving_before)));
        CHECK(atomic_load(&object_pointer(&string)->references) == references);
        admission.work = 68;
        CHECK(xr_xir_array_push(&place, &string, &admission) == XR_XIR_VALUE_OK);
        CHECK(!admission.work && ((XirArray *) object_pointer(&array))->length == 65);
        if (mode == 2) CHECK(domain->stats.reallocations == before.reallocations + 1);
        xr_xir_value_drop(&array); xr_xir_value_drop(&copy);
    }
    xr_xir_value_drop(&string); xr_xir_type_arena_drop(arena);
    xr_xir_domain_drop(domain); xr_xir_domain_drop(receiver); CHECK(!live);
}
static void array_deep_release(void) {
    fail_at = SIZE_MAX; const uint32_t count = 2048;
    XrXirTypeNode *nodes = counted_calloc(count, sizeof(*nodes)); CHECK(nodes);
    for (uint32_t i = 0; i < count; ++i) {
        nodes[i].kind = XR_XIR_TYPE_ARRAY;
        nodes[i].element = i ? (XrXirType) (256 + i - 1) : XR_XIR_STRING;
    }
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(1024 * 1024, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = array_allocation_arena(domain, nodes, count); counted_free(nodes);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, UINT64_MAX, 0};
    XrXirValue previous = {0};
    CHECK(xr_xir_string_new(domain, "leaf", 4, &previous) == XR_XIR_VALUE_OK);
    for (uint32_t i = 0; i < count; ++i) {
        XrXirValue next = {0};
        CHECK(xr_xir_array_new((XrXirType) (256 + i), &previous, 1, &admission, &next) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&previous); previous = next;
    }
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
    size_t allocations = calls; fail_at = calls;
    xr_xir_value_drop(&previous); CHECK(!live && calls == allocations); fail_at = SIZE_MAX;
    puts("Array cleanup: 2048 nested compact backings; zero cleanup allocations; zero live blocks");
}
static void array_allocation_cases(void) {
    array_compact_payloads(); array_allocation_failures(); array_cross_domain_accounting();
    array_retain_atomicity(); array_nested_gate_scratch(); array_copy_work_budget(); array_deep_release();
}
#endif // XIR_ARRAY_ALLOCATION_CASES_H
