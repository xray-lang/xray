/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_view_origin_pin_consumer.c - Physical identity without logical permission
 *
 * KEY CONCEPT:
 *   A retained origin body cannot keep an ended mutable or shared loan active.
 */
#include "xir/xxir_view_value_internal.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_array.h"
#include <stdio.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);return 1;}} while (0)
typedef struct PinScope {bool active;XrXirCellAuthority authority;} PinScope;
static XrXirValueStatus pin_authorize(void *context,const XrXirValue *value,bool write,
    XrXirValueAdmission *admission,XrXirCellAuthority *output) {
    PinScope *scope=context;(void)value;(void)write;(void)admission;
    if (!scope->active) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=scope->authority;return XR_XIR_VALUE_OK;
}
int main(void) {
    const XrCompileResourceLimits limits={1048576,1048576,1000000};XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);c.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats initial={0};CHECK(xr_compile_resources_stats(c.resources,&initial)==XR_COMPILE_RESOURCE_OK);
    const XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8}
    };
    const XrXirTypes types={nodes,5,NULL,NULL};XrXirTypeArena *arena=NULL;XrXirDomain *domain=NULL;
    CHECK(xr_xir_compile_type_arena_new(&c,&types,&arena)==XR_XIR_VALUE_OK && xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={arena,domain,NULL,NULL,100000,65536};PinScope scope={true,{domain,arena,73}};
    XrXirViewAccess access={&admission,pin_authorize,&scope};XrXirViewOriginPin pin={0};XrXirViewOriginInfo info={0};
    XrXirValue array={0},cell={0},view={0},read={0},text={0},replacement={0};XrXirFaultDetail fault={0};
    const XrXirValue item={XR_XIR_I64,0,9};
    CHECK(xr_xir_array_new((XrXirType)256,&item,1,&admission,&array)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)257,&array,&admission,&cell)==XR_XIR_VALUE_OK);
    XrXirViewStorageSpec spec={scope.authority,0,1,XR_XIR_TYPE_MUT_SLICE};
    CHECK(xr_xir_view_new((XrXirType)258,&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_view_origin_pin(&view,&access,&pin)==XR_XIR_VALUE_OK);
    XrXirValue shadow=view;xr_xir_value_drop(&view);scope.active=false;
    CHECK(xr_xir_cell_unborrowed(&cell) && xr_xir_cell_read(&cell,&read)==XR_XIR_VALUE_OK);xr_xir_value_drop(&read);
    CHECK(xr_xir_view_origin_info(&pin,&admission,&info)==XR_XIR_VALUE_OK && !info.logical_active && info.length==1);
    CHECK(xr_xir_view_get(&shadow,0,&access,&read,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !read.type);
    CHECK(xr_xir_cell_write(&cell,&array,&admission)==XR_XIR_VALUE_OK);
    xr_xir_view_origin_unpin(&pin);xr_xir_value_drop(&cell);xr_xir_value_drop(&array);scope.active=true;
    CHECK(xr_xir_string_new(domain,"x\0y",3,&text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)259,&text,&admission,&cell)==XR_XIR_VALUE_OK);
    spec=(XrXirViewStorageSpec){scope.authority,0,3,XR_XIR_TYPE_SLICE};
    CHECK(xr_xir_view_new((XrXirType)260,&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_view_origin_pin(&view,&access,&pin)==XR_XIR_VALUE_OK);shadow=view;xr_xir_value_drop(&view);
    CHECK(xr_xir_string_new(domain,"q",1,&replacement)==XR_XIR_VALUE_OK && xr_xir_cell_write(&cell,&replacement,&admission)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_view_origin_info(&pin,&admission,&info)==XR_XIR_VALUE_OK && !info.logical_active && info.length==3 && xr_xir_value_valid(&shadow));
    xr_xir_view_origin_unpin(&pin);xr_xir_view_origin_unpin(&pin);
    xr_xir_value_drop(&replacement);xr_xir_value_drop(&text);xr_xir_value_drop(&cell);xr_xir_domain_drop(domain);xr_xir_compile_type_arena_drop(arena);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(c.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==initial.live_bytes);
    xr_compile_resources_release(c.resources);puts("view-origin-pin-sdk-ok");return 0;
}
