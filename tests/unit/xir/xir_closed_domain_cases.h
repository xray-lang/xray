/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_closed_domain_cases.h - Fixed ownership oracles at two domain boundaries
 *
 * KEY CONCEPT:
 *   Open cycles remain; execution close detaches roots; final host ownership ends.
 */
#ifndef XIR_CLOSED_DOMAIN_CASES_H
#define XIR_CLOSED_DOMAIN_CASES_H
typedef struct ClosedRing {
    XrXirDomain *domain;
    XrXirTypeArena *arena;
    XrXirValueAdmission admission;
    XrXirValue cell, function, empty;
    size_t releases;
} ClosedRing;

static void closed_ring_new(ClosedRing *r, unsigned edges, bool keep_empty) {
    *r = (ClosedRing){0}; CHECK(edges == 1 || edges == 2);
    CHECK(xr_xir_domain_new(65536, &r->domain) == XR_XIR_VALUE_OK);
    r->arena = allocation_arena(r->domain);
    r->admission = allocation_admission(r->domain, r->arena, &r->releases);
    XrXirFunctionBinding binding = {&r->releases,capture_release,0,NULL,0};
    CHECK(xr_xir_function_new(r->domain,r->arena,(XrXirType)256,&binding,&r->admission,&r->empty) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(r->domain,r->arena,(XrXirType)258,&r->empty,&r->admission,&r->cell) == XR_XIR_VALUE_OK);
    XrXirValue captures[] = {r->cell,r->cell};
    binding.captures = captures; binding.capture_count = edges;
    CHECK(xr_xir_function_new(r->domain,r->arena,(XrXirType)256,&binding,&r->admission,&r->function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_write(&r->cell,&r->function,&r->admission) == XR_XIR_VALUE_OK);
    if (!keep_empty) { xr_xir_value_drop(&r->empty); CHECK(r->releases == 1); r->releases = 0; }
}
static void closed_ring_owners_drop(ClosedRing *r) {
    xr_xir_compile_type_arena_drop(r->arena); r->arena = NULL;
    xr_xir_domain_drop(r->domain); r->domain = NULL;
}
static void closed_no_alloc_begin(size_t *before) {
    CHECK(!closed_fail_all); *before = calls; closed_fail_all = true;
}
static void closed_no_alloc_end(size_t before) {
    CHECK(calls == before); closed_fail_all = false;
}
static void closed_string(const XrXirValue *value) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value,&bytes,&length) && length == 4 && !memcmp(bytes,"kept",4));
}

static void closed_no_roots(void) {
    for (unsigned edges = 1; edges <= 2; ++edges) {
        ClosedRing r; closed_ring_new(&r,edges,false);
        XirObject *cell = object_pointer(&r.cell), *function = object_pointer(&r.function);
        xr_xir_value_drop(&r.cell); xr_xir_value_drop(&r.function);
        CHECK(atomic_load(&cell->references) == edges && atomic_load(&function->references) == 1);
        CHECK(!r.releases);
        size_t blocks = live, before; closed_no_alloc_begin(&before);
        xr_xir_domain_close(r.domain);
        CHECK(r.releases == 1 && live + 2 == blocks);
        xr_xir_domain_close(r.domain); CHECK(r.releases == 1 && live + 2 == blocks);
        closed_ring_owners_drop(&r); CHECK(!live);
        closed_no_alloc_end(before);
    }
}

static void closed_owned_aliases(bool mutate) {
    ClosedRing r; closed_ring_new(&r,1,mutate);
    XrXirValue alias = {0}, observed = {0}, copy = {0};
    CHECK(xr_xir_value_copy(&r.cell,&alias) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&r.function);
    CHECK(atomic_load(&object_pointer(&r.cell)->references) == 3);
    size_t before; closed_no_alloc_begin(&before);
    xr_xir_domain_close(r.domain); CHECK(!r.releases);
    CHECK(xr_xir_cell_read(&alias,&observed) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&observed,&copy) == XR_XIR_VALUE_OK && copy.payload == observed.payload);
    if (mutate) {
        CHECK(xr_xir_cell_write(&r.cell,&r.empty,&r.admission) == XR_XIR_VALUE_OK);
        XrXirValue latest = {0};
        CHECK(xr_xir_cell_read(&alias,&latest) == XR_XIR_VALUE_OK && latest.payload == r.empty.payload);
        xr_xir_value_drop(&latest);
    }
    xr_xir_value_drop(&r.cell); xr_xir_value_drop(&alias);
    xr_xir_value_drop(&observed); CHECK(!r.releases);
    xr_xir_value_drop(&copy); CHECK(r.releases == 1);
    if (mutate) { CHECK(xr_xir_value_valid(&r.empty)); xr_xir_value_drop(&r.empty); CHECK(r.releases == 2); }
    closed_ring_owners_drop(&r); CHECK(!live); closed_no_alloc_end(before);
}

static void closed_string_and_residue(bool cycle_root_at_close) {
    ClosedRing r; closed_ring_new(&r,1,false);
    XrXirValue text = {0}, alias = {0};
    CHECK(xr_xir_string_new(r.domain,"kept",4,&text) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&r.function);
    if (!cycle_root_at_close) xr_xir_value_drop(&r.cell);
    size_t before; closed_no_alloc_begin(&before);
    xr_xir_domain_close(r.domain);
    CHECK(r.releases == (cycle_root_at_close ? 0u : 1u)); closed_string(&text);
    if (cycle_root_at_close) {
        /* The host residual owner still has text. Becoming unreachable after
         * the execution boundary does not start partial cycle collection. */
        xr_xir_value_drop(&r.cell); CHECK(!r.releases);
        xr_xir_domain_close(r.domain); CHECK(!r.releases);
    }
    CHECK(xr_xir_value_copy(&text,&alias) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&text); closed_string(&alias);
    CHECK(r.releases == (cycle_root_at_close ? 0u : 1u));
    xr_xir_value_drop(&alias); CHECK(r.releases == 1);
    closed_ring_owners_drop(&r); CHECK(!live); closed_no_alloc_end(before);
}

static void closed_mutation_new_cycle(void) {
    ClosedRing r; closed_ring_new(&r,1,true);
    /* Undo the backedge while both physical objects have external roots. */
    CHECK(xr_xir_cell_write(&r.cell,&r.empty,&r.admission) == XR_XIR_VALUE_OK);
    size_t before; closed_no_alloc_begin(&before);
    xr_xir_domain_close(r.domain); CHECK(!r.releases);
    CHECK(xr_xir_cell_write(&r.cell,&r.function,&r.admission) == XR_XIR_VALUE_OK);
    XrXirValue observed = {0};
    CHECK(xr_xir_cell_read(&r.cell,&observed) == XR_XIR_VALUE_OK && observed.payload == r.function.payload);
    xr_xir_value_drop(&observed); xr_xir_value_drop(&r.empty); CHECK(r.releases == 1);
    xr_xir_value_drop(&r.cell); CHECK(r.releases == 1);
    xr_xir_value_drop(&r.function); CHECK(r.releases == 2);
    closed_ring_owners_drop(&r); CHECK(!live); closed_no_alloc_end(before);
}

static void closed_two_domains(void) {
    for (unsigned order = 0; order < 2; ++order) {
        ClosedRing r[2]; closed_ring_new(&r[0],1,false); closed_ring_new(&r[1],2,false);
        CHECK(!xr_xir_cell_in_domain(&r[0].cell,r[1].domain) && !xr_xir_cell_in_domain(&r[1].cell,r[0].domain));
        xr_xir_value_drop(&r[0].cell); xr_xir_value_drop(&r[0].function);
        xr_xir_value_drop(&r[1].function);
        size_t before; closed_no_alloc_begin(&before);
        xr_xir_domain_close(r[order].domain); xr_xir_domain_close(r[1u-order].domain);
        CHECK(r[0].releases == 1 && !r[1].releases);
        XrXirValue observed = {0};
        CHECK(xr_xir_cell_read(&r[1].cell,&observed) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&observed); CHECK(!r[1].releases);
        closed_ring_owners_drop(&r[0]);
        xr_xir_value_drop(&r[1].cell); CHECK(r[1].releases == 1);
        closed_ring_owners_drop(&r[1]); CHECK(!live); closed_no_alloc_end(before);
    }
}

static void closed_deep_ring(void) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(64u*1024u*1024u,&domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = allocation_arena(domain); size_t releases = 0;
    XrXirValueAdmission admission = allocation_admission(domain,arena,&releases);
    XrXirFunctionBinding binding = {&releases,capture_release,0,NULL,0};
    XrXirValue previous = {0}, first = {0};
    CHECK(xr_xir_function_new(domain,arena,(XrXirType)256,&binding,&admission,&previous) == XR_XIR_VALUE_OK);
    for (uint32_t i = 0; i < 100000; ++i) {
        XrXirValue cell = {0}, next = {0};
        CHECK(xr_xir_cell_new(domain,arena,(XrXirType)258,&previous,&admission,&cell) == XR_XIR_VALUE_OK);
        if (!i) CHECK(xr_xir_value_copy(&cell,&first) == XR_XIR_VALUE_OK);
        binding.captures = &cell; binding.capture_count = 1;
        CHECK(xr_xir_function_new(domain,arena,(XrXirType)256,&binding,&admission,&next) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&previous); xr_xir_value_drop(&cell); previous = next;
    }
    CHECK(xr_xir_cell_write(&first,&previous,&admission) == XR_XIR_VALUE_OK);
    CHECK(releases == 1); /* The empty seed is no longer in the ring. */
    xr_xir_value_drop(&first); xr_xir_value_drop(&previous); CHECK(releases == 1);
    size_t before; closed_no_alloc_begin(&before);
    xr_xir_domain_close(domain); CHECK(releases == 100001);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    CHECK(!live); closed_no_alloc_end(before);
}
static void closed_domain_cases(void) {
    CHECK(!live && !closed_fail_all); fail_at = SIZE_MAX;
    closed_no_roots(); closed_owned_aliases(false); closed_owned_aliases(true);
    closed_string_and_residue(false); closed_string_and_residue(true);
    closed_mutation_new_cycle(); closed_two_domains(); closed_deep_ring();
    puts("Closed domains: fixed edge counts, mutable escaped aliases, two ownership boundaries, 100000-ring, zero close allocations");
}
#endif // XIR_CLOSED_DOMAIN_CASES_H
