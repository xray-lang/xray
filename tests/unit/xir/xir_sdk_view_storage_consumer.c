/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_view_storage_consumer.c - Installed runtime ranges and root retirement
 */
#include "xir/xxir_view_storage_internal.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) return __LINE__;} while (0)
int main(void) {
    const XrCompileResourceLimits limits={1048576,1048576,4000000};
    XrXirCompileContext c={0};CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256}
    };
    XrXirTypes types={nodes,2,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&c,&types,&arena)==XR_XIR_VALUE_OK);
    memset(nodes,0xcc,sizeof(nodes));CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,100000,65536};
    XrXirValue array={0},root={0},result={0},copied={0};
    const XrXirValue inputs[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22},{XR_XIR_I64,0,33}};
    CHECK(xr_xir_array_new((XrXirType)256,inputs,3,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)257,&array,&admission,&root)==XR_XIR_VALUE_OK);
    const XrXirCellAuthority a={domain,arena,19};
    XrXirViewStorage view={0},shared={0};XrXirFaultDetail fault={0};
    XrXirViewStorageSpec spec={a,1,2,XR_XIR_TYPE_MUT_SLICE};
    CHECK(xr_xir_view_storage_begin(&root,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_read(&root,&result)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    XrXirValue number={XR_XIR_I64,0,99};
    CHECK(xr_xir_view_storage_set(&view,&a,1,&number,&admission,&fault)==XR_XIR_VALUE_OK);
    spec=(XrXirViewStorageSpec){a,0,2,XR_XIR_TYPE_SLICE};
    CHECK(xr_xir_view_storage_reborrow(&view,&a,&spec,&admission,&shared)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_view_storage_set(&view,&a,0,&number,&admission,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_view_storage_get(&shared,&a,1,&admission,&result,&fault)==XR_XIR_VALUE_OK && result.payload==99);
    xr_xir_value_drop(&result);xr_xir_view_storage_end(&shared);
    CHECK(xr_xir_view_storage_copy((XrXirType)256,&view,&a,&admission,&copied)==XR_XIR_VALUE_OK);
    xr_xir_view_storage_end(&view);
    CHECK(xr_xir_array_get(&array,2,&admission,&result,&fault)==XR_XIR_VALUE_OK && result.payload==33);
    xr_xir_value_drop(&result);xr_xir_value_drop(&root);xr_xir_value_drop(&array);
    xr_xir_compile_type_arena_drop(arena);xr_xir_domain_drop(domain);xr_xir_value_drop(&copied);
    xr_compile_resources_release(c.resources);puts("view-storage-sdk-ok");return 0;
}
