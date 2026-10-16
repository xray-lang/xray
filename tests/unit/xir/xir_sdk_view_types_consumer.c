/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_view_types_consumer.c - Borrowed metadata through installed archives
 */
#include "xir/xxir_types.h"
#include "xir/xxir_view_types_internal.h"
#include "xir/xxir_type_arena.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) return __LINE__;} while (0)
int main(void) {
    const XrCompileResourceLimits limits={1048576,1048576,4000000};
    XrXirCompileContext c={0};CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();XrCompileResourceStats initial={0},final={0};
    CHECK(xr_compile_resources_stats(c.resources,&initial)==XR_COMPILE_RESOURCE_OK);
    XrXirCallableParameter fields[]={{(XrXirType)256,0},{(XrXirType)257,0}};
    XrXirCallableParameter parameter={(XrXirType)257,0};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_CALLABLE,.parameters=&parameter,.parameter_count=1,
         .result=(XrXirType)258,.flags=XR_XIR_CALLABLE_ROOT_NONE},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)258}
    };
    XrXirTypes types={nodes,5,NULL,NULL};XrXirTypes *owned=NULL;uint32_t value=99;
    CHECK(xr_xir_compile_types_structure_verify(&c,&types)==XR_XIR_BAD_TYPE);
    types.count=4;CHECK(xr_xir_compile_types_structure_verify(&c,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_types_clone(&c,&types,&owned)==XR_XIR_OK);
    memset(nodes,0xcc,sizeof(nodes));memset(fields,0xcc,sizeof(fields));memset(&parameter,0xcc,sizeof(parameter));
    CHECK(xr_xir_compile_types_structure_verify(&c,owned)==XR_XIR_OK);
    CHECK(xr_xir_compile_view_type_flags(&c,owned,(XrXirType)258,&value)==XR_XIR_OK && value==3);
    CHECK(xr_xir_compile_view_return_candidate(&c,owned,(XrXirType)259,&value)==XR_XIR_OK && !value);
    const XrXirType input=(XrXirType)257;
    const XrXirInstruction returned={.op=XR_XIR_RETURN,.args={0,0}};
    const XrXirBlock block={.count=1};
    const XrXirFunction function={.name="sdkOrigin",.name_length=9,.parameters=&input,.parameter_count=1,
        .result=input,.blocks=&block,.block_count=1,.instructions=&returned,.instruction_count=1};
    const XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.types=owned};
    CHECK(xir_compile_view_return_origins_verified(&c,&module,NULL)==XR_XIR_OK);
    XrXirTypeArena *arena=NULL;
    CHECK(xr_xir_compile_type_arena_new(&c,owned,&arena)==XR_XIR_VALUE_BAD_ARGUMENT && !arena);
    xr_xir_compile_types_free(owned);
    CHECK(xr_compile_resources_stats(c.resources,&final)==XR_COMPILE_RESOURCE_OK && final.live_bytes==initial.live_bytes);
    xr_compile_resources_release(c.resources);puts("view-types-sdk-ok");return 0;
}
