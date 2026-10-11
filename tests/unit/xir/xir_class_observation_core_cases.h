/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_observation_core_cases.h - Independent physical lifetime record controls
 *
 * KEY CONCEPT:
 *   Fixed event goldens cover escaped owners, partial storage and real graph retirement.
 */
#ifndef XIR_CLASS_OBSERVATION_CORE_CASES_H
#define XIR_CLASS_OBSERVATION_CORE_CASES_H
static XrXirDomain *observation_core_domain(uint64_t work) {
    XrXirDomainBudgetControls controls = {65536, 65536, work, 65536, 65536};
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new_budgeted(65536, &controls, &domain) == XR_XIR_VALUE_OK);
    return domain;
}
static void observation_core_event(const XrXirClassEvent *event, uint64_t serial,
    uint32_t kind, uint32_t flags, uint32_t initialized, XrXirValueStatus status) {
    CHECK(event->serial == serial && event->type == PREFIX_CLASS_T && event->declaration == 0);
    CHECK(event->kind == kind && event->flags == flags && event->initialized_fields == initialized);
    CHECK(event->status == (uint32_t)status);
}
static uint32_t observation_core_drain(XrXirClassObservation *observation,
    uint64_t *cursor, XrXirClassEvent *events, bool expected_overflow) {
    uint32_t count = 0; bool overflow = false;
    CHECK(xr_xir_class_observation_drain(observation, cursor, events, 128, &count, &overflow) == XR_XIR_VALUE_OK);
    CHECK(overflow == expected_overflow); return count;
}
static void observation_core_bad_cursor(XrXirClassObservation *observation, uint64_t cursor) {
    XrXirClassEvent events[2], before[2]; memset(events, 0x5A, sizeof(events)); memcpy(before, events, sizeof(events));
    uint64_t wrong = cursor + 1; uint32_t count = 0; bool overflow = false;
    CHECK(xr_xir_class_observation_drain(observation, &wrong, events, 2, &count, &overflow) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(wrong == cursor + 1 && !count && !overflow && !memcmp(events, before, sizeof(events)));
    CHECK(xr_xir_class_observation_drain(observation, &cursor, events, 2,
        (uint32_t *)&cursor, &overflow) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!cursor && !overflow && !memcmp(events, before, sizeof(events)));
}
static void observation_core_escape(XrXirTypeArena *arena) {
    XrXirDomain *domain = observation_core_domain(65536);
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirClassObservation *observation = NULL, *second = NULL;
    CHECK(xr_xir_domain_class_observation_open(domain, 128, &observation) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_class_observation_open(domain, 128, &second) == XR_XIR_VALUE_BAD_ARGUMENT && !second);
    observation_core_bad_cursor(observation, 0);
    CHECK(xr_xir_class_observation_retain(observation) == XR_XIR_VALUE_OK); second = observation;
    xr_xir_class_observation_drop(&observation); CHECK(!observation);
    XrXirValue text = {0}, object = {0}, alias = {0};
    CHECK(xr_xir_string_new(domain, "kept", 4, &text) == XR_XIR_VALUE_OK);
    XrXirValue fields[] = {{XR_XIR_I64, 0, 40}, text};
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    CHECK(xr_xir_class_new((XrXirType)PREFIX_CLASS_T, fields, 2, &admission, &object) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&object, &alias) == XR_XIR_VALUE_OK);
    XrXirClassEvent events[128]; uint64_t cursor = 0;
    CHECK(observation_core_drain(second, &cursor, events, false) == 1);
    observation_core_event(events, 1, XR_XIR_CLASS_CONSTRUCT, 0, 0, XR_XIR_VALUE_OK);
    xr_xir_value_drop(&object); xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(observation_core_drain(second, &cursor, events, false) == 0);
    CHECK(xr_xir_value_valid(&alias));
    xr_xir_value_drop(&alias);
    CHECK(observation_core_drain(second, &cursor, events, false) == 2);
    observation_core_event(events, 1, XR_XIR_CLASS_FINALIZE, 0, 0, XR_XIR_VALUE_OK);
    observation_core_event(events + 1, 1, XR_XIR_CLASS_RECLAIM, 0, 0, XR_XIR_VALUE_OK);
    xr_xir_value_drop(&text); xr_xir_class_observation_drop(&second);
    CHECK(cell_owner_live + 1 == blocks && cell_owner_bytes + sizeof(XrXirDomain) == bytes);
}
static uint64_t observation_core_construction(XrXirTypeArena *arena, uint64_t work, bool partial, uint32_t capacity) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirDomain *domain = observation_core_domain(work); XrXirClassObservation *observation = NULL;
    CHECK(xr_xir_domain_class_observation_open(domain, capacity, &observation) == XR_XIR_VALUE_OK);
    XrXirValue text = {0}, object = {0};
    CHECK(xr_xir_string_new(domain, "kept", 4, &text) == XR_XIR_VALUE_OK);
    XrXirValue fields[] = {{XR_XIR_I64, 0, 40}, text};
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    CHECK(xr_xir_class_new((XrXirType)PREFIX_CLASS_T, fields, 2, &admission, &object) ==
        (partial ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_OK));
    uint64_t cost = xr_xir_domain_budget_stats(domain).work;
    if (partial) CHECK(!object.type && !object.reserved && !object.payload);
    xr_xir_value_drop(&object);
    XrXirClassEvent events[128]; uint64_t cursor = 0;
    uint32_t count = observation_core_drain(observation, &cursor, events, capacity == 1);
    if (partial) {
        CHECK(count == 1);
        observation_core_event(events, 1, XR_XIR_CLASS_RECLAIM, XR_XIR_CLASS_EVENT_PARTIAL, 1, XR_XIR_VALUE_LIMIT);
    } else {
        CHECK(count == (capacity == 1 ? 1u : 3u));
        observation_core_event(events, 1, XR_XIR_CLASS_CONSTRUCT, 0, 0, XR_XIR_VALUE_OK);
        if (capacity != 1) {
            observation_core_event(events + 1, 1, XR_XIR_CLASS_FINALIZE, 0, 0, XR_XIR_VALUE_OK);
            observation_core_event(events + 2, 1, XR_XIR_CLASS_RECLAIM, 0, 0, XR_XIR_VALUE_OK);
        }
    }
    CHECK(xr_xir_domain_budget_stats(domain).work == cost);
    if (work == cost) {
        count = 0; bool overflow = false; uint64_t before = cursor;
        CHECK(xr_xir_class_observation_drain(observation, &cursor, events, 128, &count, &overflow) == XR_XIR_VALUE_LIMIT);
        CHECK(cursor == before && !count && !overflow);
    }
    xr_xir_value_drop(&text); xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    xr_xir_class_observation_drop(&observation);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes); return cost;
}
static XrXirTypeArena *observation_core_cycle_arena(ValueCompileOwner *compiler) {
    XrXirNominalFieldIdentity field = {{"next", 4}, XR_XIR_FIELD_MUTABLE};
    XrXirNominalIdentity identity = {.module = {"root", 4}, .name = {"Node", 4},
        .fields = &field, .field_count = 1, .kind = XR_XIR_NOMINAL_CLASS, .flags = XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable table = {NULL, 1, &identity}; XrXirType field_type = (XrXirType)257;
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0, NULL, 0, &field_type, 1}},
        {.kind = XR_XIR_TYPE_NULLABLE, .element = (XrXirType)256}};
    XrXirTypes types = {nodes, 2, &table, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context, &types, &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static void observation_core_cycle(ValueCompileOwner *compiler) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirTypeArena *arena = observation_core_cycle_arena(compiler);
    XrXirDomain *domain = observation_core_domain(65536); XrXirClassObservation *observation = NULL;
    CHECK(xr_xir_domain_class_observation_open(domain, 128, &observation) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    XrXirValue none = {0}, object = {0}, some = {0};
    CHECK(xr_xir_nullable_new((XrXirType)257, NULL, &admission, &none) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_new((XrXirType)256, &none, 1, &admission, &object) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257, &object, &admission, &some) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_set(&object, 0, &some, &admission) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&some); xr_xir_value_drop(&none); xr_xir_value_drop(&object);
    XrXirClassEvent events[128]; uint64_t cursor = 0;
    CHECK(observation_core_drain(observation, &cursor, events, false) == 1);
    observation_core_event(events, 1, XR_XIR_CLASS_CONSTRUCT, 0, 0, XR_XIR_VALUE_OK);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(observation_core_drain(observation, &cursor, events, false) == 2);
    observation_core_event(events, 1, XR_XIR_CLASS_FINALIZE, XR_XIR_CLASS_EVENT_RESIDUAL, 0, XR_XIR_VALUE_OK);
    observation_core_event(events + 1, 1, XR_XIR_CLASS_RECLAIM, XR_XIR_CLASS_EVENT_RESIDUAL, 0, XR_XIR_VALUE_OK);
    xr_xir_class_observation_drop(&observation);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes);
}
static void observation_core_cases(ValueCompileOwner *compiler, XrXirTypeArena *arena) {
    observation_core_escape(arena);
    uint64_t cost = observation_core_construction(arena, 65536, false, 128);
    CHECK(cost && cost < 65536);
    CHECK(observation_core_construction(arena, cost, false, 128) == cost);
    CHECK(observation_core_construction(arena, cost - 1, true, 128) == cost - 1);
    observation_core_construction(arena, 65536, false, 1);
    observation_core_cycle(compiler);
}
#endif // XIR_CLASS_OBSERVATION_CORE_CASES_H
