/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_value_cases.h - Independent Array contents, bounds and authority
 */
#ifndef XIR_ARRAY_VALUE_CASES_H
#define XIR_ARRAY_VALUE_CASES_H
#include "xir/xxir_array.h"

static XrXirTypeArena *array_value_arena(XrXirDomain *domain) {
    const XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_CALLABLE, .result = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_STRING},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType) 257},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType) 256},
        {.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType) 259},
        {.kind = XR_XIR_TYPE_CELL, .element = (XrXirType) 257},
    };
    XrXirTypes types = {nodes, 6, NULL, NULL};
    XrXirBudget budget = {0}; budget.metadata_bytes = 65536; budget.work = 10000;
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static void array_expected_string(const XrXirValue *array, int64_t index,
                                  const char *text, XrXirValueAdmission *admission) {
    XrXirValue result = {0}; XrXirFaultDetail fault = {99, 1, 2, 3};
    CHECK(xr_xir_array_get(array, index, admission, &result, &fault) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_fault_empty(fault)); bytes_equal(&result, text, strlen(text));
    xr_xir_value_drop(&result);
}
static void array_strings_and_bounds(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = array_value_arena(domain);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 10000, 65536};
    XrXirValue elements[3] = {{0}}, array = {0}, copy = {0}, read = {0}, empty = {0};
    CHECK(xr_xir_string_new(domain, "red", 3, &elements[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "blue", 4, &elements[1]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "green", 5, &elements[2]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType) 257, elements, 2, &admission, &array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&array, &copy) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType) 257, &array.payload};
    XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_set(&place, 1, elements + 2, &admission, &fault) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_fault_empty(fault) && array.payload != copy.payload);
    CHECK(xr_xir_array_get(&array, 0, &admission, &read, &fault) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_push(&place, &read, &admission) == XR_XIR_VALUE_OK);
    array_expected_string(&array, 0, "red", &admission);
    array_expected_string(&array, 1, "green", &admission);
    array_expected_string(&array, 2, "red", &admission);
    array_expected_string(&copy, 0, "red", &admission);
    array_expected_string(&copy, 1, "blue", &admission);
    int64_t length = -1;
    CHECK(xr_xir_array_len(&array, &admission, &length) == XR_XIR_VALUE_OK && length == 3);
    const int64_t invalid[] = {-1, INT64_MIN, 3, INT64_MAX};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        XrXirValue rejected = {0}; int64_t original = array.payload;
        CHECK(xr_xir_array_get(&array, invalid[i], &admission, &rejected, &fault) == XR_XIR_VALUE_BOUNDS);
        CHECK(!rejected.type && xr_xir_fault_bounds_valid(fault) && fault.index == invalid[i] && fault.length == 3);
        CHECK(xr_xir_array_set(&place, invalid[i], elements, &admission, &fault) == XR_XIR_VALUE_BOUNDS);
        CHECK(array.payload == original && fault.code == 430 && !fault.reserved && fault.index == invalid[i] && fault.length == 3);
    }
    CHECK(xr_xir_array_new((XrXirType) 257, NULL, 0, &admission, &empty) == XR_XIR_VALUE_OK);
    XrXirValue rejected = {0};
    CHECK(xr_xir_array_get(&empty, 0, &admission, &rejected, &fault) == XR_XIR_VALUE_BOUNDS);
    CHECK(!fault.index && !fault.length && fault.code == 430 && !rejected.type);
    admission.work = 0;
    CHECK(xr_xir_array_get(&array, 0, &admission, &rejected, &fault) == XR_XIR_VALUE_LIMIT);
    CHECK(xr_xir_fault_empty(fault) && !rejected.type);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
    for (size_t i = 0; i < 3; ++i) xr_xir_value_drop(&elements[i]);
    xr_xir_value_drop(&array); xr_xir_value_drop(&copy); xr_xir_value_drop(&empty);
    bytes_equal(&read, "red", 3); xr_xir_value_drop(&read);
}
static void array_arena_domain_and_cell(void) {
    XrXirDomain *origin = NULL, *receiver = NULL;
    CHECK(xr_xir_domain_new(65536, &origin) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &receiver) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = array_value_arena(origin), *foreign = array_value_arena(receiver);
    XrXirValueAdmission admission = {arena, origin, NULL, NULL, 10000, 65536};
    XrXirValue string = {0}, array = {0}, copy = {0}, cell = {0}, nested = {0};
    CHECK(xr_xir_string_new(origin, "survives", 8, &string) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType) 257, &string, 1, &admission, &array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType) 258, &array, 1, &admission, &nested) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(origin, arena, (XrXirType) 261, &array, &admission, &cell) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {0};
    CHECK(xr_xir_cell_value_place(&cell, &admission, &place) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_push(&place, &string, &admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_read(&cell, &copy) == XR_XIR_VALUE_OK);
    int64_t length = -1;
    CHECK(xr_xir_array_len(&copy, &admission, &length) == XR_XIR_VALUE_OK && length == 2);
    CHECK(xr_xir_array_len(&array, &admission, &length) == XR_XIR_VALUE_OK && length == 1);
    admission.domain = receiver;
    CHECK(xr_xir_value_admit(&array, (XrXirType) 257, &admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_value_place(&cell, &admission, &place) == XR_XIR_VALUE_BAD_ARGUMENT);
    admission.arena = foreign;
    CHECK(xr_xir_value_admit(&array, (XrXirType) 257, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    admission.arena = arena; admission.domain = NULL;
    xr_xir_type_arena_drop(foreign); xr_xir_domain_drop(receiver);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(origin);
    xr_xir_value_drop(&string); xr_xir_value_drop(&cell); xr_xir_value_drop(&copy);
    xr_xir_value_drop(&array);
    XrXirValue child = {0}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_get(&nested, 0, &admission, &child, &fault) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&nested);
    array_expected_string(&child, 0, "survives", &admission);
    xr_xir_value_drop(&child);
}
static void array_function_gates(void) {
    XrXirDomain *domain = NULL, *receiver = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &receiver) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = array_value_arena(domain);
    ValueGate gate = {true, 0, 0};
    XrXirFunctionBinding binding = {&gate, value_gate_release, 3, NULL, 0};
    XrXirValueAdmission admission = {arena, domain, value_gate_admit, &gate, 10000, 65536};
    XrXirValue function = {0}, array = {0}, nested = {0}, empty = {0}, output = {0}, received = {0};
    CHECK(xr_xir_function_new(domain, arena, (XrXirType) 256, &binding, &admission, &function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType) 259, &function, 1, &admission, &array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType) 260, &array, 1, &admission, &nested) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_new((XrXirType) 259, NULL, 0, &admission, &empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&nested, (XrXirType) 260, &admission) == XR_XIR_VALUE_OK);
    admission.domain = receiver;
    CHECK(xr_xir_value_admit(&empty, (XrXirType) 259, &admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&array, (XrXirType) 259, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_value_admit(&nested, (XrXirType) 260, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    ValueGate receiving_gate = {true, 0, 0};
    XrXirFunctionBinding receiving_binding = {&receiving_gate, value_gate_release, 3, NULL, 0};
    admission.context = &receiving_gate;
    CHECK(xr_xir_function_new(receiver, arena, (XrXirType) 256, &receiving_binding,
        &admission, &received) == XR_XIR_VALUE_OK);
    XrXirValuePlace place = {(XrXirType) 259, &empty.payload};
    CHECK(xr_xir_array_push(&place, &received, &admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_admit(&empty, (XrXirType) 259, &admission) == XR_XIR_VALUE_OK);
    admission.domain = domain; admission.context = &gate;
    CHECK(xr_xir_value_admit(&empty, (XrXirType) 259, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    admission.domain = domain; gate.active = false;
    CHECK(xr_xir_value_admit(&nested, (XrXirType) 260, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    uint32_t admitted = gate.admissions;
    XrXirDomainStats before = xr_xir_domain_stats(domain);
    admission.domain = NULL; admission.work = 1; admission.scratch_bytes = 0;
    int64_t length = -1;
    CHECK(xr_xir_array_len(&nested, &admission, &length) == XR_XIR_VALUE_OK && length == 1);
    CHECK(!admission.work && gate.admissions == admitted);
    CHECK(xr_xir_domain_stats(domain).allocations == before.allocations);
    CHECK(xr_xir_value_copy(&nested, &output) == XR_XIR_VALUE_OK);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain); xr_xir_domain_drop(receiver);
    xr_xir_value_drop(&function); xr_xir_value_drop(&array); xr_xir_value_drop(&nested);
    xr_xir_value_drop(&empty); CHECK(!gate.releases && !receiving_gate.releases);
    xr_xir_value_drop(&received); CHECK(receiving_gate.releases == 1);
    xr_xir_value_drop(&output); CHECK(gate.releases == 1);
}
static void array_value_cases(void) {
    array_strings_and_bounds(); array_arena_domain_and_cell(); array_function_gates();
}
#endif // XIR_ARRAY_VALUE_CASES_H
