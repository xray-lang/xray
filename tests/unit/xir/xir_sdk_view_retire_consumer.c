/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_view_retire_consumer.c - Logical retirement with surviving shared storage
 */
#include "xir/xxir_view_value_internal.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);return __LINE__;}} while (0)
typedef struct RetireScope {bool active;XrXirCellAuthority authority;} RetireScope;
static XrXirValueStatus retire_authorize(void *context,const XrXirValue *value,bool writable,
    XrXirValueAdmission *admission,XrXirCellAuthority *output) {
    RetireScope *scope=context;(void)value;(void)writable;(void)admission;
    if (!scope->active) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=scope->authority;return XR_XIR_VALUE_OK;
}
int main(void) {
    const XrCompileResourceLimits limits={1048576,1048576,1000000};XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial={0};CHECK(xr_compile_resources_stats(c.resources,&initial)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64}
    };
    const XrXirTypes types={nodes,4,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&c,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,100000,65536};
    RetireScope scope={true,{domain,arena,31}};XrXirViewAccess access={&admission,retire_authorize,&scope};
    const XrXirValue items[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22}};
    XrXirValue array={0},root={0},parent={0},child={0},read={0},tail={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_new((XrXirType)256,items,2,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)257,&array,&admission,&root)==XR_XIR_VALUE_OK);
    XrXirViewStorageSpec spec={scope.authority,0,2,XR_XIR_TYPE_MUT_SLICE};
    CHECK(xr_xir_view_new((XrXirType)259,&root,NULL,&spec,&admission,&parent)==XR_XIR_VALUE_OK);
    spec=(XrXirViewStorageSpec){scope.authority,1,1,XR_XIR_TYPE_SLICE};
    CHECK(xr_xir_view_reborrow((XrXirType)258,&parent,&spec,&access,&child)==XR_XIR_VALUE_OK);
    XrXirValue retired=parent;xr_xir_value_drop(&parent);
    CHECK(xr_xir_cell_read(&root,&read)==XR_XIR_VALUE_OK);xr_xir_value_drop(&read);
    CHECK(xr_xir_value_valid(&retired));
    CHECK(xr_xir_view_get(&retired,0,&access,&read,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type);
    CHECK(xr_xir_cell_write(&root,&array,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    spec=(XrXirViewStorageSpec){scope.authority,0,1,XR_XIR_TYPE_SLICE};
    CHECK(xr_xir_view_reborrow((XrXirType)258,&child,&spec,&access,&tail)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&child);
    CHECK(xr_xir_view_get(&tail,0,&access,&read,&fault)==XR_XIR_VALUE_OK && read.payload==22);
    xr_xir_value_drop(&read);scope.active=false;
    CHECK(xr_xir_view_get(&tail,0,&access,&read,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type);
    xr_xir_value_drop(&tail);CHECK(xr_xir_cell_unborrowed(&root));
    CHECK(xr_xir_cell_write(&root,&array,&admission)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&root);xr_xir_value_drop(&array);xr_xir_domain_drop(domain);
    xr_xir_compile_type_arena_drop(arena);XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(c.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.live_bytes==initial.live_bytes);
    xr_compile_resources_release(c.resources);puts("view-retire-sdk-ok");return 0;
}
