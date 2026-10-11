/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_publication_core_cases.h - Literal publication transactions and allocation faults
 *
 * KEY CONCEPT:
 *   Retained identities remain local until the one infallible publication commit.
 */
#ifndef XIR_CLASS_PUBLICATION_CORE_CASES_H
#define XIR_CLASS_PUBLICATION_CORE_CASES_H
static void publication_core_local(const XrXirValue *values, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) CHECK(xr_xir_value_valid(&values[i]) &&
        !xr_xir_class_module_storage(&values[i]));
}
static void publication_core_simple(XrXirTypeArena *arena, XrXirDomain *domain,
    XrXirValue *object) {
    XrXirValue text = {0}; CHECK(xr_xir_string_new(domain, "kept", 4, &text) == XR_XIR_VALUE_OK);
    XrXirValue fields[] = {{XR_XIR_I64, 0, 40}, text};
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    CHECK(xr_xir_class_new((XrXirType)PREFIX_CLASS_T, fields, 2, &admission, object) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&text); CHECK(!xr_xir_class_module_storage(object));
}
static void publication_core_occupied(XrXirTypeArena *arena) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirDomain *domain = observation_core_domain(65536);
    XrXirValue object = {0}; publication_core_simple(arena, domain, &object);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    XrXirClassPublication occupied = {.count = 1}, before;
    memcpy(&before, &occupied, sizeof(before));
    uint64_t work = admission.work, domain_work = xr_xir_domain_budget_stats(domain).work;
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &occupied) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied, &before, sizeof(occupied)) && admission.work == work &&
        xr_xir_domain_budget_stats(domain).work == domain_work && !xr_xir_class_module_storage(&object));
    XirClassObject *physical = (XirClassObject *)object_pointer(&object);
    atomic_store_explicit(&physical->publication_state, 8, memory_order_release);
    XrXirClassPublication publication = {0};
    CHECK(!xr_xir_value_valid(&object) && !xr_xir_class_module_storage(&object));
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(class_publication_empty(&publication) && admission.work == work &&
        xr_xir_domain_budget_stats(domain).work == domain_work);
    atomic_store_explicit(&physical->publication_state, 0, memory_order_release);
    CHECK(xr_xir_class_publication_prepare(NULL, 0, &admission, &publication) == XR_XIR_VALUE_OK);
    CHECK(publication.operation && !publication.count && !publication.values);
    xr_xir_class_publication_commit(&publication); xr_xir_class_publication_drop(&publication);
    CHECK(class_publication_empty(&publication) && !xr_xir_class_module_storage(&object));
    xr_xir_value_drop(&object); xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes);
}
static void publication_core_foreign(ValueCompileOwner *compiler, XrXirTypeArena *arena) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirDomain *domain = observation_core_domain(65536), *foreign = observation_core_domain(65536);
    XrXirValue object = {0}; publication_core_simple(arena, domain, &object);
    XrXirValueAdmission admission = {arena, foreign, NULL, NULL, 65536, 65536};
    XrXirClassPublication publication = {0};
    uint32_t references = atomic_load(&object_pointer(&object)->references);
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(class_publication_empty(&publication) && !xr_xir_class_module_storage(&object));
    CHECK(atomic_load(&object_pointer(&object)->references) == references);
    XrXirTypeArena *other = prefix_arena_new(compiler);
    admission = (XrXirValueAdmission){other, domain, NULL, NULL, 65536, 65536};
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(class_publication_empty(&publication) && !xr_xir_class_module_storage(&object) &&
        atomic_load(&object_pointer(&object)->references) == references);
    xr_xir_compile_type_arena_drop(other);
    xr_xir_value_drop(&object); xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    xr_xir_domain_close(foreign); xr_xir_domain_drop(foreign);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes);
}
static void publication_core_limits(XrXirTypeArena *arena) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirDomain *domain = observation_core_domain(65536);
    XrXirValue object = {0}; publication_core_simple(arena, domain, &object);
    uint64_t live = xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 4, 65536};
    XrXirClassPublication publication = {0};
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_LIMIT);
    CHECK(class_publication_empty(&publication) && !xr_xir_class_module_storage(&object));
    admission = (XrXirValueAdmission){arena, domain, NULL, NULL, 65536, 63};
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_LIMIT);
    CHECK(class_publication_empty(&publication) && !xr_xir_class_module_storage(&object) &&
        admission.scratch_bytes == 63 && xr_xir_domain_stats(domain).live_bytes == live);
    admission = (XrXirValueAdmission){arena, domain, NULL, NULL, 65536, 65536};
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_OK);
    CHECK(!xr_xir_class_module_storage(&object));
    xr_xir_class_publication_commit(&publication); xr_xir_class_publication_drop(&publication);
    CHECK(xr_xir_class_module_storage(&object));
    xr_xir_value_drop(&object); xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes);
}
static void publication_core_borrowed(XrXirTypeArena *arena) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirDomain *domain = observation_core_domain(65536);
    XrXirValue object = {0}, projection = {0}; publication_core_simple(arena, domain, &object);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    unsigned activation = 0, frame = 0;
    /* This models an already checked driver's actual loan, not a Program grant. */
    XrXirCellAuthority authority = {&activation, &frame, 1};
    XrXirValuePathStep step = {XR_XIR_PATH_FIELD, (XrXirType)PREFIX_CLASS_T, 0};
    XrXirValuePath path = {&step, 1};
    CHECK(xr_xir_cell_project_authorized((XrXirType)PREFIX_CELL_T, &object, &path,
        &authority, &admission, &projection) == XR_XIR_VALUE_OK);
    XrXirCellLoan loan = {0};
    CHECK(xr_xir_cell_loan_prepare(&projection, &authority, &loan) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&projection, &loan, &authority);
    XrXirClassPublication publication = {0};
    uint32_t references = atomic_load(&object_pointer(&object)->references);
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(class_publication_empty(&publication) && !xr_xir_class_module_storage(&object));
    CHECK(((const XirClassObject *)object_pointer(&object))->borrow_top == &loan &&
        atomic_load(&object_pointer(&object)->references) == references);
    xr_xir_cell_loan_release(&loan);
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_OK);
    xr_xir_class_publication_commit(&publication); xr_xir_class_publication_drop(&publication);
    CHECK(xr_xir_class_module_storage(&object) && xr_xir_class_module_storage(xr_xir_cell_canonical_root(&projection)));
    xr_xir_value_drop(&projection); xr_xir_value_drop(&object);
    xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes);
}
static void publication_core_reservation(XrXirTypeArena *arena) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirDomain *domain = observation_core_domain(65536);
    XrXirValue object = {0}, projection = {0}, output = {0};
    publication_core_simple(arena, domain, &object);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    unsigned activation = 0, parent_frame = 0, child_frame = 0;
    XrXirCellAuthority parent = {&activation, &parent_frame, 1};
    XrXirCellAuthority child = {&activation, &child_frame, 2};
    XrXirValuePathStep step = {XR_XIR_PATH_FIELD, (XrXirType)PREFIX_CLASS_T, 0};
    XrXirValuePath path = {&step, 1}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_cell_project_authorized((XrXirType)PREFIX_CELL_T, &object, &path,
        &parent, &admission, &projection) == XR_XIR_VALUE_OK);
    XirClassObject *physical = (XirClassObject *)object_pointer(&object);
    XrXirClassPublication publication = {0}; XrXirCellLoan loan = {0}, nested = {0};
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_OK);
    CHECK(class_origin_state(physical) == 2 && !physical->borrow_top);
    CHECK(xr_xir_cell_loan_prepare(&projection, &parent, &loan) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(cell_loan_empty(&loan) && xr_xir_value_valid(&object) && !xr_xir_class_module_storage(&object));
    CHECK(xr_xir_class_get(&object, 0, &admission, &output) == XR_XIR_VALUE_BAD_ARGUMENT && !output.type);
    XrXirValue replacement = {XR_XIR_I64, 0, 99};
    CHECK(xr_xir_class_set(&object, 0, &replacement, &admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_class_publication_drop(&publication);
    CHECK(class_origin_state(physical) == 0);
    CHECK(xr_xir_cell_loan_prepare_path(&projection, &parent, &admission, &fault, &loan) == XR_XIR_VALUE_OK);
    CHECK(class_origin_state(physical) == 2 && !physical->borrow_top);
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(class_publication_empty(&publication) && !xr_xir_class_module_storage(&object));
    XrXirCellLoan occupied = {.cell = physical}, saved;
    memcpy(&saved, &occupied, sizeof(saved));
    CHECK(xr_xir_cell_loan_prepare(&projection, &parent, &occupied) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied, &saved, sizeof(saved)));
    xr_xir_cell_loan_abort(&loan); CHECK(cell_loan_empty(&loan) && class_origin_state(physical) == 0);
    CHECK(xr_xir_cell_loan_prepare_path(&projection, &parent, &admission, &fault, &loan) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&projection, &loan, &parent);
    CHECK(class_origin_state(physical) == 4 && physical->borrow_top == &loan);
    CHECK(xr_xir_cell_loan_prepare_path(&projection, &parent, &admission, &fault, &nested) == XR_XIR_VALUE_OK);
    CHECK(class_origin_state(physical) == 6 && physical->borrow_top == &loan);
    CHECK(xr_xir_cell_authorized_path_read(&projection, &parent, &(XrXirValuePath){0},
        &admission, &output, &fault) == XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_cell_loan_abort(&nested); CHECK(class_origin_state(physical) == 4 && physical->borrow_top == &loan);
    CHECK(xr_xir_cell_loan_prepare_path(&projection, &parent, &admission, &fault, &nested) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&projection, &nested, &child);
    CHECK(class_origin_state(physical) == 4 && physical->borrow_top == &nested);
    xr_xir_cell_loan_release(&nested);
    CHECK(class_origin_state(physical) == 4 && physical->borrow_top == &loan);
    xr_xir_cell_loan_release(&loan);
    CHECK(class_origin_state(physical) == 0 && !physical->borrow_top);
    CHECK(xr_xir_class_get(&object, 0, &admission, &output) == XR_XIR_VALUE_OK && output.payload == 40);
    xr_xir_value_drop(&output);
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_OK);
    xr_xir_class_publication_commit(&publication); xr_xir_class_publication_drop(&publication);
    CHECK(class_origin_state(physical) == 1 && xr_xir_class_module_storage(&object));
    CHECK(xr_xir_cell_loan_prepare_path(&projection, &parent, &admission, &fault, &loan) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&projection, &loan, &parent); CHECK(class_origin_state(physical) == 5);
    xr_xir_cell_loan_release(&loan); CHECK(class_origin_state(physical) == 1);
    xr_xir_value_drop(&projection); xr_xir_value_drop(&object);
    xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes);
}
static XrXirTypeArena *publication_reservation_arena(ValueCompileOwner *compiler) {
    XrXirNominalFieldIdentity field = {{"items", 5}, XR_XIR_FIELD_MUTABLE};
    XrXirNominalIdentity identity = {.module = {"root", 4}, .name = {"Items", 5},
        .exported = 1, .fields = &field, .field_count = 1,
        .kind = XR_XIR_NOMINAL_CLASS, .flags = XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable table = {NULL, 1, &identity}; XrXirType field_type = (XrXirType)257;
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0, NULL, 0, &field_type, 1}},
        {.kind = XR_XIR_TYPE_ARRAY, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_CELL, .element = XR_XIR_I64},
        {.kind = XR_XIR_TYPE_CELL, .element = (XrXirType)257}};
    XrXirTypes types = {nodes, 4, &table, NULL}; XrXirTypeArena *arena = NULL;
    ValueCompileProbe probe = {.types = &types, .parameters = 100};
    value_compile_boundaries(&probe, (XrCompileResourceLimits){1048576, 1048576, 1048576});
    CHECK(xr_xir_compile_type_arena_new(&compiler->context, &types, &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static void publication_reservation_bounds(ValueCompileOwner *compiler) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    size_t compiler_blocks = value_compile_live, compiler_bytes = value_compile_bytes;
    XrXirTypeArena *arena = publication_reservation_arena(compiler);
    XrXirDomain *domain = observation_core_domain(65536);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    XrXirValue forty = {XR_XIR_I64, 0, 40}, array = {0}, object = {0};
    CHECK(xr_xir_array_new((XrXirType)257, &forty, 1, &admission, &array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_new((XrXirType)256, &array, 1, &admission, &object) == XR_XIR_VALUE_OK);
    unsigned activation = 0, parent_frame = 0;
    XrXirCellAuthority parent = {&activation, &parent_frame, 1};
    XrXirValuePathStep steps[] = {{XR_XIR_PATH_FIELD, (XrXirType)256, 0},
        {XR_XIR_PATH_INDEX, (XrXirType)257, 3}};
    XrXirValuePath bad_path = {steps, 2}, parent_path = {steps, 1};
    XrXirValue projection = {0}, parent_projection = {0}, output = {0};
    CHECK(xr_xir_cell_project_authorized((XrXirType)258, &object, &bad_path,
        &parent, &admission, &projection) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_project_authorized((XrXirType)259, &object, &parent_path,
        &parent, &admission, &parent_projection) == XR_XIR_VALUE_OK);
    XirClassObject *physical = (XirClassObject *)object_pointer(&object);
    XrXirCellLoan loan = {0}, rejected = {0}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_cell_loan_prepare_path(&projection, &parent, &admission, &fault, &rejected) == XR_XIR_VALUE_BOUNDS);
    CHECK(cell_loan_empty(&rejected) && class_origin_state(physical) == 0 && !physical->borrow_top);
    CHECK(fault.code == 430 && !fault.reserved && fault.index == 3 && fault.length == 1);
    XrXirValueAdmission limited = admission; limited.work = 2;
    CHECK(xr_xir_cell_loan_prepare_path(&projection, &parent, &limited, &fault, &rejected) == XR_XIR_VALUE_LIMIT);
    CHECK(cell_loan_empty(&rejected) && class_origin_state(physical) == 0 && !physical->borrow_top);
    CHECK(xr_xir_fault_empty(fault));
    XrXirClassPublication publication = {0};
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_OK);
    xr_xir_class_publication_drop(&publication); CHECK(class_origin_state(physical) == 0);
    CHECK(xr_xir_cell_loan_prepare_path(&parent_projection, &parent, &admission, &fault, &loan) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&parent_projection, &loan, &parent);
    CHECK(class_origin_state(physical) == 4 && physical->borrow_top == &loan);
    CHECK(xr_xir_cell_loan_prepare_path(&projection, &parent, &admission, &fault, &rejected) == XR_XIR_VALUE_BOUNDS);
    CHECK(cell_loan_empty(&rejected) && class_origin_state(physical) == 4 && physical->borrow_top == &loan);
    CHECK(fault.code == 430 && !fault.reserved && fault.index == 3 && fault.length == 1);
    steps[1].selector = 0; XrXirValuePath suffix = {&steps[1], 1};
    CHECK(xr_xir_cell_authorized_path_read(&parent_projection, &parent, &suffix,
        &admission, &output, &fault) == XR_XIR_VALUE_OK && output.type == XR_XIR_I64 && output.payload == 40);
    xr_xir_value_drop(&output); xr_xir_cell_loan_release(&loan);
    CHECK(class_origin_state(physical) == 0 && !physical->borrow_top && !xr_xir_class_module_storage(&object));
    xr_xir_value_drop(&parent_projection); xr_xir_value_drop(&projection);
    xr_xir_value_drop(&object); xr_xir_value_drop(&array);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(value_compile_live == compiler_blocks && value_compile_bytes == compiler_bytes);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes);
}
static void publication_core_cycle(ValueCompileOwner *compiler) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirTypeArena *arena = observation_core_cycle_arena(compiler);
    XrXirDomain *domain = observation_core_domain(65536);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    XrXirValue none = {0}, object = {0}, some = {0}, alias = {0};
    CHECK(xr_xir_nullable_new((XrXirType)257, NULL, &admission, &none) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_new((XrXirType)256, &none, 1, &admission, &object) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257, &object, &admission, &some) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_set(&object, 0, &some, &admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&object, &alias) == XR_XIR_VALUE_OK);
    XrXirClassPublication publication = {0};
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_OK);
    CHECK(publication.count == 1 && !xr_xir_class_module_storage(&object));
    xr_xir_class_publication_drop(&publication);
    CHECK(!xr_xir_class_module_storage(&alias));
    CHECK(xr_xir_class_publication_prepare(&object, 1, &admission, &publication) == XR_XIR_VALUE_OK);
    xr_xir_class_publication_commit(&publication); xr_xir_class_publication_drop(&publication);
    CHECK(xr_xir_class_module_storage(&object) && xr_xir_class_module_storage(&alias));
    xr_xir_value_drop(&some); xr_xir_value_drop(&none); xr_xir_value_drop(&object); xr_xir_value_drop(&alias);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes);
}
static size_t publication_core_operation(XrXirTypeArena *arena, size_t fail_at, bool injected) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirDomain *domain = observation_core_domain(65536); XrXirValue objects[5] = {{0}};
    for (uint32_t i = 0; i < 5; ++i) publication_core_simple(arena, domain, &objects[i]);
    size_t original_blocks = cell_owner_live, original_bytes = cell_owner_bytes;
    uint64_t original_live = xr_xir_domain_stats(domain).live_bytes;
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    XrXirClassPublication publication = {0};
    observation_runtime_fault_begin(fail_at); observation_runtime_phase(OBSERVATION_FAULT_CLASS);
    XrXirValueStatus status = xr_xir_class_publication_prepare(objects, 5, &admission, &publication);
    size_t sites = observation_runtime_fault.attempts;
    if (injected) {
        CHECK(status == XR_XIR_VALUE_OOM && observation_runtime_fault.hits == 1);
        CHECK(class_publication_empty(&publication)); publication_core_local(objects, 5);
    } else {
        CHECK(status == XR_XIR_VALUE_OK && !observation_runtime_fault.hits && publication.count == 5);
        publication_core_local(objects, 5); xr_xir_class_publication_drop(&publication);
    }
    observation_runtime_fault_end();
    CHECK(cell_owner_live == original_blocks && cell_owner_bytes == original_bytes &&
        xr_xir_domain_stats(domain).live_bytes == original_live);
    CHECK(admission.scratch_bytes == 65536 && class_publication_empty(&publication));
    CHECK(xr_xir_class_publication_prepare(objects, 5, &admission, &publication) == XR_XIR_VALUE_OK);
    publication_core_local(objects, 5);
    size_t attempts = observation_runtime_fault.attempts;
    xr_xir_class_publication_commit(&publication); xr_xir_class_publication_drop(&publication);
    CHECK(observation_runtime_fault.attempts == attempts);
    for (uint32_t i = 0; i < 5; ++i) { CHECK(xr_xir_class_module_storage(&objects[i])); xr_xir_value_drop(&objects[i]); }
    XrXirDomainBudgetStats stats = xr_xir_domain_budget_stats(domain);
    printf("Class publication FI ordinal=%zu sites=%zu work=%llu requested=%llu live=%llu peak=%llu\n",
        fail_at, sites, (unsigned long long)stats.work, (unsigned long long)stats.requested_bytes,
        (unsigned long long)xr_xir_domain_stats(domain).live_bytes,
        (unsigned long long)xr_xir_domain_stats(domain).peak_bytes);
    xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes); return sites;
}
static XrXirTypeArena *publication_cell_arena(ValueCompileOwner *compiler) {
    XrXirNominalFieldIdentity field = {{"value", 5}, XR_XIR_FIELD_MUTABLE};
    XrXirNominalIdentity identity = {.module = {"root", 4}, .name = {"CellHolder", 10},
        .exported = 1, .fields = &field, .field_count = 1,
        .kind = XR_XIR_NOMINAL_CLASS, .flags = XR_XIR_NOMINAL_FINAL};
    XrXirNominalTable table = {NULL, 1, &identity};
    XrXirType field_type = XR_XIR_I64;
    XrXirTypeNode nodes[] = {
        {.kind = XR_XIR_TYPE_NOMINAL, .nominal = {0, NULL, 0, &field_type, 1}},
        {.kind = XR_XIR_TYPE_CELL, .element = (XrXirType)256}};
    XrXirTypes types = {nodes, 2, &table, NULL}; XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler->context, &types, &arena) == XR_XIR_VALUE_OK);
    return arena;
}
static size_t publication_cell_operation(XrXirTypeArena *arena, size_t ordinal, bool injected) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    XrXirDomain *domain = observation_core_domain(65536);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    XrXirValue initial = {XR_XIR_I64, 0, 40}, object = {0}, cell = {0}, alias = {0};
    CHECK(xr_xir_class_new((XrXirType)256, &initial, 1, &admission, &object) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType)257, &object, &admission, &cell) == XR_XIR_VALUE_OK);
    CHECK(!xr_xir_class_module_storage(&object) && !xr_xir_cell_module_storage(&cell));
    uint64_t live = xr_xir_domain_stats(domain).live_bytes;
    size_t original_blocks = cell_owner_live, original_bytes = cell_owner_bytes;
    uint32_t object_refs = atomic_load(&object_pointer(&object)->references);
    uint32_t cell_refs = atomic_load(&object_pointer(&cell)->references);
    XrXirClassPublication publication = {0};
    observation_runtime_fault_begin(ordinal); observation_runtime_phase(OBSERVATION_FAULT_CLASS);
    XrXirValueStatus status = xr_xir_class_publication_prepare(&cell, 1, &admission, &publication);
    size_t sites = observation_runtime_fault.attempts;
    if (injected) {
        CHECK(status == XR_XIR_VALUE_OOM && observation_runtime_fault.hits == 1);
        CHECK(class_publication_empty(&publication));
    } else {
        CHECK(status == XR_XIR_VALUE_OK && !observation_runtime_fault.hits && publication.count == 2);
        CHECK(!xr_xir_class_module_storage(&object));
        xr_xir_class_publication_drop(&publication);
    }
    observation_runtime_fault_end();
    CHECK(class_publication_empty(&publication) && !xr_xir_class_module_storage(&object));
    CHECK(!xr_xir_cell_module_storage(&cell) && xr_xir_domain_stats(domain).live_bytes == live);
    CHECK(cell_owner_live == original_blocks && cell_owner_bytes == original_bytes);
    CHECK(atomic_load(&object_pointer(&object)->references) == object_refs &&
        atomic_load(&object_pointer(&cell)->references) == cell_refs && admission.scratch_bytes == 65536);
    CHECK(xr_xir_class_publication_prepare(&cell, 1, &admission, &publication) == XR_XIR_VALUE_OK);
    CHECK(publication.count == 2 && !xr_xir_class_module_storage(&object));
    size_t attempts = observation_runtime_fault.attempts;
    xr_xir_class_publication_commit(&publication); xr_xir_class_publication_drop(&publication);
    CHECK(observation_runtime_fault.attempts == attempts && xr_xir_class_module_storage(&object));
    CHECK(!xr_xir_cell_module_storage(&cell));
    CHECK(xr_xir_cell_read(&cell, &alias) == XR_XIR_VALUE_OK && alias.payload == object.payload &&
        xr_xir_class_module_storage(&alias));
    xr_xir_value_drop(&cell); xr_xir_value_drop(&object);
    CHECK(xr_xir_value_valid(&alias) && xr_xir_class_module_storage(&alias));
    XrXirValue field = {0};
    CHECK(xr_xir_class_get(&alias, 0, &admission, &field) == XR_XIR_VALUE_OK);
    CHECK(field.type == XR_XIR_I64 && !field.reserved && field.payload == 40);
    xr_xir_value_drop(&field); xr_xir_value_drop(&alias);
    XrXirDomainBudgetStats stats = xr_xir_domain_budget_stats(domain);
    printf("Class Cell-root publication ordinal=%zu sites=%zu work=%llu requested=%llu live=%llu peak=%llu\n",
        ordinal, sites, (unsigned long long)stats.work, (unsigned long long)stats.requested_bytes,
        (unsigned long long)xr_xir_domain_stats(domain).live_bytes,
        (unsigned long long)xr_xir_domain_stats(domain).peak_bytes);
    xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes); return sites;
}
static void publication_cell_cases(ValueCompileOwner *compiler) {
    size_t blocks = value_compile_live, bytes = value_compile_bytes;
    XrXirTypeArena *arena = publication_cell_arena(compiler);
    size_t sites = publication_cell_operation(arena, SIZE_MAX, false); CHECK(sites);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) publication_cell_operation(arena, ordinal, true);
    xr_xir_compile_type_arena_drop(arena);
    CHECK(value_compile_live == blocks && value_compile_bytes == bytes);
}
static void publication_core_cases(ValueCompileOwner *compiler, XrXirTypeArena *arena) {
    publication_core_occupied(arena); publication_core_foreign(compiler, arena); publication_core_borrowed(arena);
    publication_core_limits(arena); publication_core_reservation(arena); publication_reservation_bounds(compiler);
    publication_core_cycle(compiler);
    publication_cell_cases(compiler);
    size_t sites = publication_core_operation(arena, SIZE_MAX, false); CHECK(sites);
    for (size_t ordinal = 0; ordinal < sites; ++ordinal) publication_core_operation(arena, ordinal, true);
}
#endif // XIR_CLASS_PUBLICATION_CORE_CASES_H
