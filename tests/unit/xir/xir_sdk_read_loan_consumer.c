/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_read_loan_consumer.c - Admitted archives enforce shared root lifetimes
 *
 * KEY CONCEPT:
 *   Consumer records and private runtime storage agree across C providers.
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir/xxir_compile_context.h"
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"
#include "xir/xxir_cell_read_loan_internal.h"

int main(void) {
    const XrCompileResourceLimits limits={1048576,1048576,4194304};
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},final={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    const XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256}};
    const XrXirTypes types={nodes,2,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&context,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(context.resources,65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,100000,65536};
    const XrXirValue items[]={{XR_XIR_I64,0,1},{XR_XIR_I64,0,2}};
    XrXirValue array={0},cell={0},number={XR_XIR_I64,0,9},read={0};
    CHECK(xr_xir_array_new((XrXirType)256,items,2,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)257,&array,&admission,&cell)==XR_XIR_VALUE_OK);
    const XrXirCellAuthority authority={context.resources,arena,7};XrXirCellReadLoan first={0},second={0};
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,NULL,&admission,&first)==XR_XIR_VALUE_OK);
    xr_xir_cell_read_loan_commit(&cell,&first,&authority,NULL);
    CHECK(xr_xir_cell_read_loan_prepare(&cell,&authority,NULL,&admission,&second)==XR_XIR_VALUE_OK);
    xr_xir_cell_read_loan_commit(&cell,&second,&authority,NULL);
    const XrXirValuePathStep step={XR_XIR_PATH_INDEX,(XrXirType)256,0};
    const XrXirValuePath path={&step,1};XrXirFaultDetail fault={0};
    CHECK(xr_xir_cell_authorized_path_write(&cell,&authority,&path,&number,&admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_cell_authorized_path_read(&cell,&authority,&path,&admission,&read,&fault)==XR_XIR_VALUE_OK && read.payload==1);
    xr_xir_value_drop(&read);xr_xir_cell_read_loan_release(&first);
    CHECK(xr_xir_cell_authorized_path_write(&cell,&authority,&path,&number,&admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_cell_read_loan_release(&second);
    CHECK(xr_xir_cell_authorized_path_write(&cell,&authority,&path,&number,&admission,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_authorized_path_read(&cell,&authority,&path,&admission,&read,&fault)==XR_XIR_VALUE_OK && read.payload==9);
    xr_xir_value_drop(&read);
    CHECK(xr_xir_array_get(&array,0,&admission,&read,&fault)==XR_XIR_VALUE_OK && read.payload==1);
    xr_xir_value_drop(&read);xr_xir_value_drop(&cell);xr_xir_value_drop(&array);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);
    CHECK(xr_compile_resources_stats(context.resources,&final)==XR_COMPILE_RESOURCE_OK && final.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    puts("read-loan-sdk-ok");return 0;
}
