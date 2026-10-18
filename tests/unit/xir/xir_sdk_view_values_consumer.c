/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_view_values_consumer.c - Scoped handles through installed runtime archives
 */
#include "xir/xxir_view_value_internal.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_compile_domain.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) return __LINE__;} while (0)
typedef struct SdkViewScope {
    XrXirCellAuthority authority;
    int64_t payloads[2];
    bool active;
} SdkViewScope;
static XrXirValueStatus sdk_view_authorize(void *context,const XrXirValue *value,bool writable,
    XrXirValueAdmission *admission,XrXirCellAuthority *output) {
    SdkViewScope *scope=context;(void)writable;(void)admission;
    if (!scope->active || (value->payload!=scope->payloads[0] && value->payload!=scope->payloads[1]))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=scope->authority;return XR_XIR_VALUE_OK;
}
int main(void) {
    _Static_assert(XR_XIR_VALUE_ABI_VERSION==27,"scoped view Value ABI");
    const XrCompileResourceLimits limits={1048576,1048576,4000000};
    XrXirCompileContext c={0};CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();XrCompileResourceStats initial={0},final={0};
    CHECK(xr_compile_resources_stats(c.resources,&initial)==XR_COMPILE_RESOURCE_OK);
    const XrXirCallableParameter fields[]={{(XrXirType)259,0},{XR_XIR_I64,0}};
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)260}
    };
    const XrXirTypes types={nodes,6,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&c,&types,&arena)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_domain_new(c.resources,65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,100000,65536};
    SdkViewScope scope={.authority={domain,arena,19},.active=true};
    XrXirViewAccess access={&admission,sdk_view_authorize,&scope};
    XrXirValue array={0},root={0},view={0},shared={0},tuple={0},some={0},result={0},copied={0};
    XrXirValue members[2]={{0}};
    const XrXirValue items[]={{XR_XIR_I64,0,11},{XR_XIR_I64,0,22},{XR_XIR_I64,0,33}};
    CHECK(xr_xir_array_new((XrXirType)256,items,3,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)257,&array,&admission,&root)==XR_XIR_VALUE_OK);
    XrXirViewStorageSpec read_spec={scope.authority,0,3,XR_XIR_TYPE_SLICE};
    CHECK(xr_xir_view_new((XrXirType)258,&root,NULL,&read_spec,&admission,&view)==XR_XIR_VALUE_OK);
    scope.payloads[0]=view.payload;read_spec.offset=1;read_spec.length=1;
    CHECK(xr_xir_view_reborrow((XrXirType)258,&view,&read_spec,&access,&shared)==XR_XIR_VALUE_OK);
    scope.payloads[1]=shared.payload;xr_xir_value_drop(&view);
    XrXirFaultDetail read_fault={0};
    CHECK(xr_xir_view_get(&shared,0,&access,&result,&read_fault)==XR_XIR_VALUE_OK && result.payload==22);
    xr_xir_value_drop(&result);xr_xir_value_drop(&shared);CHECK(xr_xir_cell_unborrowed(&root));
    XrXirViewStorageSpec spec={scope.authority,1,2,XR_XIR_TYPE_MUT_SLICE};
    CHECK(xr_xir_view_new((XrXirType)259,&root,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    scope.payloads[0]=view.payload;
    CHECK(xr_xir_value_copy(&view,&result)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    CHECK(xr_xir_value_admit(&view,(XrXirType)259,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirFaultDetail fault={0};XrXirValue number={XR_XIR_I64,0,99};
    CHECK(xr_xir_view_set(&view,1,&number,&access,&fault)==XR_XIR_VALUE_OK);
    spec=(XrXirViewStorageSpec){scope.authority,0,2,XR_XIR_TYPE_SLICE};
    CHECK(xr_xir_view_reborrow((XrXirType)258,&view,&spec,&access,&shared)==XR_XIR_VALUE_OK);
    scope.payloads[1]=shared.payload;
    CHECK(xr_xir_view_set(&view,0,&number,&access,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xr_xir_value_move(&view,&members[0])==XR_XIR_VALUE_OK && !view.type);
    members[1]=(XrXirValue){XR_XIR_I64,0,55};
    CHECK(xr_xir_borrowed_tuple_new_move((XrXirType)260,members,2,&access,&tuple)==XR_XIR_VALUE_OK);
    CHECK(!members[0].type && !members[1].type);
    CHECK(xr_xir_borrowed_some_new_move((XrXirType)261,&tuple,&access,&some)==XR_XIR_VALUE_OK && !tuple.type);
    CHECK(xr_xir_value_copy(&some,&result)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    CHECK(xr_xir_borrowed_some_take(&some,&access,&tuple)==XR_XIR_VALUE_OK && !some.type);
    CHECK(xr_xir_borrowed_tuple_unpack_move(&tuple,members,2,&access)==XR_XIR_VALUE_OK && !tuple.type);
    xr_xir_value_drop(&members[0]);xr_xir_value_drop(&members[1]);
    scope.active=false;CHECK(xr_xir_value_valid(&shared));
    CHECK(xr_xir_view_get(&shared,1,&access,&result,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !result.type);
    scope.active=true;
    CHECK(xr_xir_view_get(&shared,1,&access,&result,&fault)==XR_XIR_VALUE_OK && result.payload==99);
    xr_xir_value_drop(&result);
    CHECK(xr_xir_view_copy((XrXirType)256,&shared,&access,&copied)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&shared);CHECK(xr_xir_cell_unborrowed(&root));
    CHECK(xr_xir_array_get(&array,2,&admission,&result,&fault)==XR_XIR_VALUE_OK && result.payload==33);
    xr_xir_value_drop(&result);xr_xir_value_drop(&copied);xr_xir_value_drop(&root);xr_xir_value_drop(&array);
    xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    CHECK(xr_compile_resources_stats(c.resources,&final)==XR_COMPILE_RESOURCE_OK && final.live_bytes==initial.live_bytes);
    xr_compile_resources_release(c.resources);puts("view-values-sdk-ok");return 0;
}
