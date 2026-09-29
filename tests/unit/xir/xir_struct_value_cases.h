/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_struct_value_cases.h - Nominal snapshots and escaped field ownership
 */
#ifndef XIR_STRUCT_VALUE_CASES_H
#define XIR_STRUCT_VALUE_CASES_H
#include "xir/xxir_struct.h"
#include "xir_nominal_fixture.h"
static XrXirTypeArena *struct_value_arena(XrXirDomain *domain) {
    NominalIdentityFixture f; nominal_identity_fixture(&f);
    XrXirType fields[] = {XR_XIR_I64, XR_XIR_STRING, (XrXirType)256, XR_XIR_STRING};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, NULL, 0, fields, 2}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {1, NULL, 0, fields + 2, 2}}};
    XrXirTypes types = {nodes, 2, &f.table};
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.parameters = 100; budget.metadata_bytes = 65536; budget.work = 10000;
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    memset(&f, 0xCC, sizeof(f)); memset(nodes, 0xCC, sizeof(nodes));
    return arena;
}
static XrXirValue struct_deep_value(void) {
    enum { DEPTH = 160 };
    XrXirNominalIdentity identities[DEPTH]; XrXirNominalFieldIdentity field = {{"item", 4}, XR_XIR_FIELD_MUTABLE};
    char names[DEPTH][16]; XrXirType fields[DEPTH]; XrXirTypeNode nodes[DEPTH];
    for (uint32_t i = 0; i < DEPTH; ++i) {
        int length = snprintf(names[i], sizeof(names[i]), "Nested%u", i); CHECK(length > 0);
        identities[i] = (XrXirNominalIdentity) {{"module", 6}, {names[i], (uint32_t) length}, 1, 0, &field, 1, XR_XIR_NOMINAL_STRUCT, NULL, 0};
        fields[i] = i + 1 == DEPTH ? XR_XIR_I64 : (XrXirType) (257 + i);
        nodes[i] = (XrXirTypeNode) {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0,
            {i, NULL, 0, &fields[i], 1}};
    }
    XrXirNominalTable table = {NULL, DEPTH, identities}; XrXirTypes types = {nodes, DEPTH, &table};
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = NULL;
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.parameters = 10000; budget.metadata_bytes = 1048576; budget.work = 1000000;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 1000000, 65536};
    XrXirValue child = {XR_XIR_I64, 0, 71};
    for (uint32_t i = DEPTH; i; --i) {
        XrXirValue parent = {0};
        CHECK(xr_xir_struct_new((XrXirType)(255 + i), &child, 1, &admission, &parent) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&child); child = parent;
    }
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain);
    return child;
}
typedef struct StructGate { bool active; uint32_t releases; } StructGate;
static void struct_gate_release(void *owner) { ++((StructGate *) owner)->releases; }
static XrXirValueStatus struct_gate_admit(void *context, const XrXirFunctionBinding *binding,
    XrXirType type, uint64_t *work) {
    if (!*work) return XR_XIR_VALUE_LIMIT;
    --*work;
    return ((StructGate *) context)->active && binding->owner == context && type == (XrXirType)256 ?
        XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static void struct_function_gate(void) {
    XrXirDomain *domain = NULL, *other = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &other) == XR_XIR_VALUE_OK);
    NominalIdentityFixture f; nominal_identity_fixture(&f);
    f.identities[0].field_count = f.identities[1].field_count = 1;
    XrXirType fields[] = {(XrXirType)256, (XrXirType)257};
    XrXirTypeNode nodes[] = {
        {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT, NULL, 0, XR_XIR_I64, 0, 0, {0}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {0, NULL, 0, fields, 1}},
        {XR_XIR_TYPE_NOMINAL, XR_XIR_UNIT, NULL, 0, XR_XIR_UNIT, 0, 0, {1, NULL, 0, fields + 1, 1}}};
    XrXirTypes types = {nodes, 3, &f.table};
    XrXirBudget budget = {0}; budget.scratch_bytes = 1048576; budget.parameters = 100; budget.metadata_bytes = 65536; budget.work = 10000;
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_type_arena_new(domain, &types, &budget, &arena) == XR_XIR_VALUE_OK);
    StructGate gate = {true, 0};
    XrXirFunctionBinding binding = {&gate, struct_gate_release, 0, NULL, 0};
    XrXirValueAdmission admission = {arena, domain, struct_gate_admit, &gate, 100000, 65536};
    XrXirValue function = {0}, leaf = {0}, parent = {0}, rejected = {0};
    CHECK(xr_xir_function_new(domain, arena, (XrXirType)256, &binding, &admission, &function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_new((XrXirType)257, &function, 1, &admission, &leaf) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_new((XrXirType)258, &leaf, 1, &admission, &parent) == XR_XIR_VALUE_OK);
    admission.domain = other;
    CHECK(xr_xir_value_admit(&parent, (XrXirType)258, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    admission.domain = domain; gate.active = false;
    CHECK(xr_xir_struct_get(&parent, 0, &admission, &rejected) == XR_XIR_VALUE_BAD_ARGUMENT && !rejected.type);
    CHECK(xr_xir_struct_new((XrXirType)258, &leaf, 1, &admission, &rejected) == XR_XIR_VALUE_BAD_ARGUMENT && !rejected.type);
    admission.work = 0;
    CHECK(xr_xir_value_admit(&parent, (XrXirType)258, &admission) == XR_XIR_VALUE_LIMIT);
    xr_xir_type_arena_drop(arena); xr_xir_domain_drop(domain); xr_xir_domain_drop(other);
    xr_xir_value_drop(&function); xr_xir_value_drop(&leaf); CHECK(!gate.releases);
    xr_xir_value_drop(&parent); CHECK(gate.releases == 1);
}
static void struct_value_cases(void) {
    struct_function_gate();
    XrXirValue deep = struct_deep_value(); xr_xir_value_drop(&deep);
    XrXirDomain *domain = NULL, *other = NULL;
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &other) == XR_XIR_VALUE_OK);
    uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
    XrXirTypeArena *arena = struct_value_arena(domain), *foreign = struct_value_arena(other);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 100000, 65536};
    XrXirValue fields[2] = {{XR_XIR_I64, 0, 7}, {0}}, leaf = {0}, parent = {0}, copy = {0}, escaped = {0};
    CHECK(xr_xir_string_new(domain, "label", 5, &fields[1]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_new((XrXirType)256, fields, 2, &admission, &leaf) == XR_XIR_VALUE_OK);
    XrXirValue outer[] = {leaf, fields[1]};
    CHECK(xr_xir_struct_new((XrXirType)257, outer, 2, &admission, &parent) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&parent, &copy) == XR_XIR_VALUE_OK);
    XrXirValue replacement = {XR_XIR_I64, 0, 19};
    CHECK(xr_xir_struct_set(&(XrXirValuePlace) {(XrXirType) leaf.type, &leaf.payload}, 0, &replacement, &admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_set(&(XrXirValuePlace) {(XrXirType) leaf.type, &leaf.payload}, 1, &fields[1], &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_struct_set(&(XrXirValuePlace) {(XrXirType) leaf.type, &leaf.payload}, 0, &fields[1], &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_struct_set(&(XrXirValuePlace) {(XrXirType) parent.type, &parent.payload}, 0, &leaf, &admission) == XR_XIR_VALUE_OK);
    XrXirValue old_leaf = {0}, old_number = {0}, new_leaf = {0}, new_number = {0};
    CHECK(xr_xir_struct_get(&copy, 0, &admission, &old_leaf) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_get(&old_leaf, 0, &admission, &old_number) == XR_XIR_VALUE_OK);
    CHECK(old_number.type == XR_XIR_I64 && old_number.payload == 7);
    CHECK(xr_xir_struct_get(&parent, 0, &admission, &new_leaf) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_get(&new_leaf, 0, &admission, &new_number) == XR_XIR_VALUE_OK);
    CHECK(new_number.type == XR_XIR_I64 && new_number.payload == 19);
    CHECK(xr_xir_struct_get(&old_leaf, 1, &admission, &escaped) == XR_XIR_VALUE_OK);
    admission.arena = foreign;
    CHECK(xr_xir_value_admit(&parent, (XrXirType)257, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    admission.arena = arena; admission.domain = other;
    CHECK(xr_xir_value_admit(&parent, (XrXirType)257, &admission) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&old_leaf); xr_xir_value_drop(&new_leaf);
    xr_xir_value_drop(&old_number); xr_xir_value_drop(&new_number);
    xr_xir_value_drop(&parent); xr_xir_value_drop(&copy); xr_xir_value_drop(&leaf); xr_xir_value_drop(&fields[1]);
    xr_xir_type_arena_drop(arena); xr_xir_type_arena_drop(foreign); xr_xir_domain_drop(other);
    CHECK(xr_xir_domain_stats(domain).live_bytes > baseline);
    xr_xir_domain_drop(domain);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&escaped, &bytes, &length) && length == 5 && !memcmp(bytes, "label", 5));
    xr_xir_value_drop(&escaped);
}
#endif // XIR_STRUCT_VALUE_CASES_H
