/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_query_coalloc_cases.h - Owned declaration payload boundaries
 *
 * KEY CONCEPT:
 *   Three copied views share one independently released snapshot allocation.
 */
static void query_coalloc_boundaries(void) {
    XrXirSourceDeclaration empty = {0};
    SourceQueryDeclarationLayout layout = {0};
    CHECK(query_declaration_extent(&empty, &layout) && !layout.bytes && !layout.work);
    layout.bytes = SIZE_MAX;
    CHECK(!query_declaration_extent(&empty, &layout));
    layout = (SourceQueryDeclarationLayout){SIZE_MAX - 8, 0}; empty.parameter_count = 1;
    CHECK(!query_declaration_extent(&empty, &layout));
    layout = (SourceQueryDeclarationLayout){0, UINT64_MAX};
    CHECK(!query_declaration_extent(&empty, &layout));
    char name[] = "map", signature[] = "(i64)->i64";
    XrXirSourceType parameters[] = {{XR_XIR_I64, 73, true}};
    XrXirSourceDeclaration declaration = {0};
    declaration.name = name; declaration.signature = signature;
    declaration.parameters = parameters; declaration.parameter_count = 1;
    XrXirSourceView view = {0}; view.declarations = &declaration; view.declaration_count = 1;
    XrXirBudget budget = {0}; budget.metadata_bytes = 65536; budget.work = 65536; budget.scratch_bytes = 91;
    XrXirSourceSnapshot *snapshot = NULL;
    size_t baseline = live, start = attempts; fail_at = SIZE_MAX;
    CHECK(xr_xir_source_snapshot_copy(&view, &budget, &snapshot) == XR_XIR_OK);
    size_t sites = attempts - start;
    CHECK(sites == 2 && budget.scratch_bytes == 91);
    uint64_t metadata = 65536 - budget.metadata_bytes, operations = 65536 - budget.work;
    CHECK(operations == 3 + sizeof(name) + sizeof(signature) + 1);
    CHECK(metadata == sizeof(XrXirSourceSnapshot) + sizeof(SourceQueryMemory) +
        sizeof(XrXirSourceDeclaration) + sizeof(parameters) + sizeof(name) + sizeof(signature));
    memset(name, 'x', sizeof(name) - 1); memset(signature, 'x', sizeof(signature) - 1);
    parameters[0].generic_owner = 99;
    const XrXirSourceDeclaration *copied = xr_xir_source_snapshot_view(snapshot)->declarations;
    CHECK(!strcmp(copied->name, "map") && !strcmp(copied->signature, "(i64)->i64"));
    CHECK(copied->parameters[0].generic_owner == 73 && copied->parameters != parameters);
    xr_xir_source_snapshot_free(snapshot); CHECK(live == baseline);
    for (unsigned axis = 0; axis < 2; ++axis) {
        for (int delta = -1; delta <= 1; ++delta) {
            budget.metadata_bytes = metadata; budget.work = operations; budget.scratch_bytes = 91;
            if (!axis) budget.metadata_bytes = (uint64_t)((int64_t)metadata + delta);
            else budget.work = (uint64_t)((int64_t)operations + delta);
            snapshot = (XrXirSourceSnapshot *)(uintptr_t)1;
            XrXirStatus status = xr_xir_source_snapshot_copy(&view, &budget, &snapshot);
            CHECK(status == (delta < 0 ? XR_XIR_BUDGET : XR_XIR_OK));
            CHECK(delta < 0 ? snapshot == NULL : snapshot != NULL);
            CHECK(budget.scratch_bytes == 91);
            if (delta < 0) {
                CHECK(budget.metadata_bytes == (axis ? metadata : metadata - 1) - sizeof(XrXirSourceSnapshot));
                CHECK(budget.work == (axis ? operations - 1 : operations) - 2);
            } else {
                CHECK(budget.metadata_bytes == (axis ? 0 : (uint64_t)delta));
                CHECK(budget.work == (axis ? (uint64_t)delta : 0));
            }
            xr_xir_source_snapshot_free(snapshot); CHECK(live == baseline);
        }
    }
    for (size_t i = 0; i < sites; ++i) {
        budget.metadata_bytes = metadata; budget.work = operations; budget.scratch_bytes = 91;
        fail_at = attempts + i; snapshot = (XrXirSourceSnapshot *)(uintptr_t)1;
        CHECK(xr_xir_source_snapshot_copy(&view, &budget, &snapshot) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!snapshot && live == baseline && budget.scratch_bytes == 91);
        CHECK(budget.metadata_bytes == metadata - (i ? sizeof(XrXirSourceSnapshot) : 0));
        CHECK(budget.work == operations - (i ? 2 : 0));
    }
    fail_at = SIZE_MAX;
    declaration.name = NULL; declaration.signature = NULL; declaration.parameters = NULL;
    budget.metadata_bytes = 65536; budget.work = 65536; budget.scratch_bytes = 0;
    CHECK(xr_xir_source_snapshot_copy(&view, &budget, &snapshot) == XR_XIR_OK);
    copied = xr_xir_source_snapshot_view(snapshot)->declarations;
    CHECK(!copied->name && !copied->signature && copied->parameters);
    CHECK(!copied->parameters[0].known && !copied->parameters[0].generic_owner);
    xr_xir_source_snapshot_free(snapshot); CHECK(live == baseline);
    declaration.parameter_count = 0; declaration.parameters = parameters;
    budget.metadata_bytes = 65536; budget.work = 65536; start = attempts;
    CHECK(xr_xir_source_snapshot_copy(&view, &budget, &snapshot) == XR_XIR_OK);
    CHECK(attempts - start == 2 && !xr_xir_source_snapshot_view(snapshot)->declarations[0].parameters);
    xr_xir_source_snapshot_free(snapshot); CHECK(live == baseline);
    XrXirConstraint constraint = {0};
    XrXirSourceDeclaration rows[2] = {0};
    rows[0].name = "x"; rows[0].signature = "odd!";
    rows[0].generic_constraints = &constraint; rows[0].generic_parameter_count = 1;
    rows[1].name = "y"; rows[1].parameters = parameters; rows[1].parameter_count = 1;
    view.declarations = rows; view.declaration_count = 2;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        budget.metadata_bytes = 65536; budget.work = 65536; budget.scratch_bytes = 13;
        fail_at = attempt ? attempts + attempt - 1 : SIZE_MAX;
        XrXirStatus status = xr_xir_source_snapshot_copy(&view, &budget, &snapshot);
        CHECK(status == (attempt ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
        if (!attempt) {
            copied = xr_xir_source_snapshot_view(snapshot)->declarations;
            CHECK((uintptr_t)copied[1].parameters % _Alignof(XrXirSourceType) == 0);
            CHECK(copied[0].generic_constraints != &constraint && copied[1].parameters != parameters);
            CHECK(copied[1].parameters[0].generic_owner == 99);
        } else CHECK(!snapshot);
        xr_xir_source_snapshot_free(snapshot); CHECK(live == baseline && budget.scratch_bytes == 13);
    }
    fail_at = SIZE_MAX;
}

static void query_coalloc_early_boundaries(void) {
    size_t baseline = live, initial_attempts = attempts;
    XrXirSourceSnapshot holder = {0};
    XrXirBudget budget = {0};
    XrXirSourceView view = {0}; view.declaration_count = 2;
    size_t table = 2 * sizeof(XrXirSourceDeclaration) + sizeof(SourceQueryMemory);
    view.declarations = (const XrXirSourceDeclaration *)(uintptr_t)1;
    budget.metadata_bytes = table - 1; budget.work = 100; budget.scratch_bytes = 19;
    SourceQueryCopy copy = {&holder, &budget, XR_XIR_OK};
    query_declarations(&copy, &view);
    CHECK(copy.status == XR_XIR_BUDGET && !holder.memory && !holder.view.declarations);
    CHECK(budget.metadata_bytes == table - 1 && budget.work == 100 && attempts == initial_attempts);
    budget.metadata_bytes = table; budget.work = 2; copy.status = XR_XIR_OK;
    query_declarations(&copy, &view);
    CHECK(copy.status == XR_XIR_BUDGET && budget.work == 2 && !holder.memory);
    XrXirSourceDeclaration rows[2] = {0};
    rows[0].name = "a"; rows[1].name = (const char *)(uintptr_t)1; view.declarations = rows;
    budget.metadata_bytes = table + 1; budget.work = 100; copy.status = XR_XIR_OK;
    query_declarations(&copy, &view);
    CHECK(copy.status == XR_XIR_BUDGET && budget.work == 99 && !holder.memory);
    CHECK(budget.metadata_bytes == table + 1 && attempts == initial_attempts);
    budget.metadata_bytes = 65536; budget.work = 5; copy.status = XR_XIR_OK;
    query_declarations(&copy, &view);
    CHECK(copy.status == XR_XIR_BUDGET && budget.work == 4 && !holder.memory);
    CHECK(budget.scratch_bytes == 19 && attempts == initial_attempts && live == baseline);
    /* Seven bytes precede the second zero-parameter row: no alignment is owed. */
    rows[0].name = "x"; rows[0].signature = "odd!"; rows[1].name = "y";
    budget.metadata_bytes = sizeof(XrXirSourceSnapshot) + table + 9;
    budget.work = 14; budget.scratch_bytes = 0;
    XrXirSourceSnapshot *snapshot = NULL;
    CHECK(xr_xir_source_snapshot_copy(&view, &budget, &snapshot) == XR_XIR_OK);
    CHECK(!budget.metadata_bytes && !budget.work && attempts == initial_attempts + 2);
    const XrXirSourceDeclaration *copied = xr_xir_source_snapshot_view(snapshot)->declarations;
    CHECK(!copied[0].parameters && !copied[1].parameters);
    CHECK(copied[1].name == copied[0].signature + 5 && !strcmp(copied[1].name, "y"));
    xr_xir_source_snapshot_free(snapshot); CHECK(live == baseline);
}
