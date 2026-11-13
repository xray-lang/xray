/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_closure_placement.inc.c - Publication-backed constructor storage facts
 */
static int closure_placement_compare(const void *left, const void *right) {
    return xir_closure_placement_compare(left, right);
}

static XirClosureStorage closure_storage_join(XirClosureStorage a, XirClosureStorage b) {
    if (a == XIR_CLOSURE_OWNED || b == XIR_CLOSURE_OWNED) return XIR_CLOSURE_OWNED;
    return a == XIR_CLOSURE_UNRESOLVED || b == XIR_CLOSURE_UNRESOLVED ? XIR_CLOSURE_UNRESOLVED : XIR_CLOSURE_FRAME;
}

static uint32_t closure_use_observation(HirRestrictionSolver *solver, HirEscapeUse use) {
    const XrHirEscapeBuilder *builder = solver->builder;
    HirEscapeObservation key = {use.function, use.instruction, use.value, use.point};
    uint32_t first = 0, end = builder->observation_count;
    while (first < end) {
        uint32_t middle = first + (end - first) / 2;
        int order = restriction_observation_compare(&builder->observations[middle], &key);
        if (order < 0) first = middle + 1;
        else end = middle;
    }
    return first < builder->observation_count &&
        !restriction_observation_compare(&builder->observations[first], &key) ? first : UINT32_MAX;
}

static bool closure_publication_storage(HirRestrictionSolver *solver, XirClosureStorage *points) {
    HirEscape *owner = &solver->builder->graph;
    for (uint32_t n = 0; n < solver->check_count && escape_poll(owner); ++n) {
        HirRestrictionCheck check = solver->checks[n];
        HirEscapeUse use = owner->uses[check.use];
        if (use.kind != HIR_ESCAPE_FUNCTION_VALUE) continue;
        uint32_t observation = closure_use_observation(solver, use);
        if (observation == UINT32_MAX) { owner->status = XR_XIR_BAD_STRUCTURE; return false; }
        XirClosureStorage storage = XIR_CLOSURE_OWNED;
        if (check.exposure != UINT32_MAX) {
            uint32_t index = restriction_node(solver, use.function, check.exposure, true);
            if (owner->status != XR_XIR_OK) return false;
            HirRestrictionNode exposure = solver->nodes[index];
            storage = exposure.constant ? XIR_CLOSURE_OWNED :
                exposure.incomplete || exposure.count ? XIR_CLOSURE_UNRESOLVED : XIR_CLOSURE_FRAME;
        }
        points[observation] = closure_storage_join(points[observation], storage);
    }
    return owner->status == XR_XIR_OK;
}

static bool closure_placements_publish(HirRestrictionSolver *solver, XrHirEscapeFacts *facts) {
    HirEscape *owner = &solver->builder->graph;
    if (!owner->constructor_count && !solver->placement_count) return true;
    uint64_t count = (uint64_t)owner->constructor_count + solver->placement_count;
    if (count > UINT32_MAX || count * sizeof(XirClosurePlacement) > SIZE_MAX) {
        owner->status = XR_XIR_OVERFLOW;
        return false;
    }
    XirClosurePlacement *rows = xir_compile_calloc(owner->memory, (size_t)count, sizeof(*rows), &owner->status);
    XirClosureStorage *points = xir_compile_calloc(owner->memory, solver->builder->observation_count,
        sizeof(*points), &owner->status);
    facts->view.placements = rows;
    if (owner->status != XR_XIR_OK) { xr_compile_resources_free(points); return false; }
    if (!closure_publication_storage(solver, points)) { xr_compile_resources_free(points); return false; }
    uint32_t cursor = 0;
    for (uint32_t n = 0; n < owner->constructor_count; ++n) {
        HirEscapeConstructor constructor = owner->constructors[n];
        rows[cursor++] = (XirClosurePlacement){constructor.function, constructor.value,
            XIR_EXECUTION_CURRENT_ENVIRONMENT, 1u, 0, XIR_CLOSURE_FRAME,
            UINT32_MAX, constructor.function, UINT32_MAX};
    }
    for (uint32_t n = 0; n < solver->placement_count; ++n) {
        HirClosureOccurrence occurrence = solver->placements[n];
        XirClosurePlacement row = occurrence.placement;
        row.storage = closure_storage_join(row.storage, points[occurrence.observation]);
        rows[cursor++] = row;
    }
    xr_compile_resources_free(points);
    qsort(rows, cursor, sizeof(*rows), closure_placement_compare);
    uint32_t used = 0;
    for (uint32_t n = 0; n < cursor && escape_poll(owner); ++n) {
        XirClosurePlacement row = rows[n];
        if (used && !closure_placement_compare(&rows[used - 1], &row)) {
            XirClosurePlacement *previous = &rows[used - 1];
            previous->ages |= row.ages;
            previous->remainder |= row.remainder;
            previous->storage = closure_storage_join(previous->storage, row.storage);
        } else rows[used++] = row;
    }
    facts->view.placement_count = used;
    return owner->status == XR_XIR_OK;
}
