/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_record_view_types.c - Borrowed fields remain visible through records
 *
 * KEY CONCEPT:
 *   A named product cannot turn a borrowed leaf into ordinary owned storage.
 */
#include "xir/xxir_types.h"
#include "xir/xxir_view_types_internal.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
static XrXirType record_view_type(uint32_t i) {return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);}
static void record_view_rules(const XrXirCompileContext *context) {
    XrXirTypeNode nodes[6]={0};
    XrXirLiteral names[2]={{"a",1},{"b",1}};
    XrXirCallableParameter fields[2]={{record_view_type(0),0},{record_view_type(1),0}};
    XrXirCallableParameter input={record_view_type(2),XR_PARAM_READ};
    nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_RECORD,.parameters=fields,.parameter_count=1,.record={names}};
    nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=record_view_type(2)};
    nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&input,.parameter_count=1,
        .result=record_view_type(3),.flags=XR_XIR_CALLABLE_ROOT_NONE};
    XrXirTypes types={nodes,5,NULL,NULL};uint32_t flags=77,candidate=77;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_view_type_flags(context,&types,record_view_type(2),&flags)==XR_XIR_OK && flags==1);
    CHECK(xr_xir_compile_view_type_flags(context,&types,record_view_type(3),&flags)==XR_XIR_OK && flags==1);
    CHECK(xr_xir_compile_view_return_candidate(context,&types,record_view_type(4),&candidate)==XR_XIR_OK && !candidate);
    fields[0].type=record_view_type(1);
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_view_type_flags(context,&types,record_view_type(2),&flags)==XR_XIR_OK && flags==2);
    fields[0].type=record_view_type(0);nodes[2].parameter_count=2;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    candidate=77;
    CHECK(xr_xir_compile_view_return_candidate(context,&types,record_view_type(4),&candidate)==XR_XIR_BAD_TYPE && candidate==77);
    nodes[4].result=XR_XIR_I64;
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_view_type_flags(context,&types,record_view_type(2),&flags)==XR_XIR_OK && flags==3);
    types.count=6;nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=record_view_type(2)};
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    nodes[5].element=record_view_type(3);
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_BAD_TYPE);
    nodes[2].parameter_count=1;fields[0].type=XR_XIR_I64;nodes[5].element=record_view_type(2);
    CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_view_type_flags(context,&types,record_view_type(2),&flags)==XR_XIR_OK && !flags);
}
int main(void) {
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128)*1024*1024};
    XrCompileResources *resources=NULL;XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    const XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    record_view_rules(&context);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes);
    xr_compile_resources_release(resources);puts("record borrowed storage and return boundaries passed");return 0;
}
