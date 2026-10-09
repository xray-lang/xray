/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cell_owner_values.c - Cell storage and private loan primitive regression
 *
 * KEY CONCEPT:
 *   Primitive restoration is checked separately from full call-view authority.
 */
#include "xir/xxir_cell_owner_internal.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
#include "xir/xxir_compile_context.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_value_compile_owner.h"
#include "cell_owner_value_physical.h"

static void cell_owner_values(void) {
    XrXirTypeNode nodes[] = {{.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_UNIT},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64}};
    XrXirTypes types = {nodes,2,NULL,NULL};
    XrCompileResourceLimits limits = {1048576,1048576,1048576};
    XrXirCompileContext compiler = {0}; compiler.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&compiler.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = {0};
    CHECK(xr_compile_resources_stats(compiler.resources,&baseline) == XR_COMPILE_RESOURCE_OK);
    XrXirTypeArena *arena = NULL;
    CHECK(xr_xir_compile_type_arena_new(&compiler,&types,&arena) == XR_XIR_VALUE_OK);
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    XrXirDomainStats domain_baseline = xr_xir_domain_stats(domain);
    CHECK(domain_baseline.live_bytes && cell_owner_live == 1);
    XrXirValueAdmission admission = {arena,domain,NULL,NULL,65536,65536};
    XrXirValue unit = {0}, unit_cell = {0}, cell = {0}, alias = {0}, output = {0};
    XrXirValue initial = {XR_XIR_I64,0,7}, replacement = {XR_XIR_I64,0,9};
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)256,&unit,&admission,&unit_cell) == XR_XIR_VALUE_OK);
    CHECK(unit_cell.type == 256 && unit_cell.payload && xr_xir_value_valid(&unit_cell));
    CHECK(xr_xir_value_admit(&unit_cell,(XrXirType)256,&admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_read(&unit_cell,&output) == XR_XIR_VALUE_OK && !output.type && !output.payload);
    CHECK(xr_xir_cell_write(&unit_cell,&unit,&admission) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_write(&unit_cell,&initial,&admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)257,&initial,&admission,&cell) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&cell,&alias) == XR_XIR_VALUE_OK && xr_xir_cell_same_owner(&cell,&alias));
    CHECK(!xr_xir_cell_same_owner(&unit_cell,&alias));

    /* These private primitive tokens model already authenticated driver frames. */
    unsigned activation = 0, parent_frame = 0, child_frame = 0;
    XrXirCellAuthority parent = {&activation,&parent_frame,1}, child = {&activation,&child_frame,2};
    XrXirCellAuthority wrong = parent; wrong.epoch = 2;
    XrXirCellLoan parent_loan = {0}, child_loan = {0};
    CHECK(xr_xir_cell_loan_prepare(&cell,NULL) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&cell,&parent_loan,&parent);
    CHECK(!xr_xir_cell_unborrowed(&cell));
    CHECK(xr_xir_cell_read(&alias,&output) == XR_XIR_VALUE_BAD_ARGUMENT && !output.type);
    CHECK(xr_xir_cell_write(&alias,&replacement,&admission) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_read(&alias,&wrong,&output) == XR_XIR_VALUE_BAD_ARGUMENT && !output.type);
    CHECK(xr_xir_cell_loan_prepare(&alias,&wrong) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_loan_prepare(&alias,&parent) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_commit(&alias,&child_loan,&child);
    CHECK(xr_xir_cell_authorized_read(&cell,&parent,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_write(&cell,&child,&replacement,&admission) == XR_XIR_VALUE_OK);
    xr_xir_cell_loan_release(&child_loan);
    CHECK(xr_xir_cell_authorized_read(&cell,&parent,&output) == XR_XIR_VALUE_OK && output.payload == 9);
    xr_xir_value_drop(&output);
    xr_xir_cell_loan_release(&parent_loan);
    CHECK(xr_xir_cell_unborrowed(&cell) && xr_xir_cell_read(&alias,&output) == XR_XIR_VALUE_OK && output.payload == 9);
    xr_xir_value_drop(&output);

    XrXirCellPublication publication = {&activation,domain,3};
    CHECK(xr_xir_cell_publication_prepare(&unit_cell,&publication) == XR_XIR_VALUE_OK);
    CHECK(!xr_xir_cell_module_storage(&unit_cell));
    xr_xir_cell_publication_commit(&unit_cell,&publication);
    CHECK(xr_xir_cell_publication_matches(&unit_cell,&publication));
    CHECK(xr_xir_cell_publication_prepare(&unit_cell,&publication) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_read(&unit_cell,&output) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_read(&unit_cell,&parent,&output) == XR_XIR_VALUE_OK && !output.type);
    ++publication.slot;
    CHECK(!xr_xir_cell_publication_matches(&unit_cell,&publication));
    xr_xir_value_drop(&alias); xr_xir_value_drop(&cell); xr_xir_value_drop(&unit_cell);
    XrXirDomainStats domain_final = xr_xir_domain_stats(domain);
    CHECK(domain_final.live_bytes == domain_baseline.live_bytes &&
        domain_final.allocations == domain_final.frees+1);
    xr_xir_domain_drop(domain); xr_xir_compile_type_arena_drop(arena);
    CHECK(!cell_owner_live && !cell_owner_bytes);
    XrCompileResourceStats final = {0};
    CHECK(xr_compile_resources_stats(compiler.resources,&final) == XR_COMPILE_RESOURCE_OK && final.live_bytes == baseline.live_bytes);
    xr_compile_resources_release(compiler.resources);
    CHECK(!value_compile_live && !value_compile_bytes);
}
int main(void) {
    cell_owner_values();
    puts("Cell Unit initialized storage, alias denial, nested primitive loan restoration and fixed publication owner passed");
    return 0;
}
