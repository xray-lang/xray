/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_types.c - Channel type ownership and authentic element obligations
 */
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_type_description.h"
#include "xir/xxir_type_inference.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_generic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)

static void malformed(const XrXirCompileContext *context, const XrXirTypeNode *original) {
    for (int mutation=0; mutation<8; ++mutation) {
        XrXirTypeNode nodes[6]; memcpy(nodes,original,sizeof(nodes));
        XrXirCallableParameter parameter={XR_XIR_I64,0};
        switch (mutation) {
        case 0: nodes[0].element=(XrXirType)256; break;
        case 1: nodes[0].element=(XrXirType)257; break;
        case 2: nodes[0].element=(XrXirType)255; break;
        case 3: nodes[0].flags=1; break;
        case 4: nodes[0].parameter_span=1; break;
        case 5: nodes[0].result=XR_XIR_I64; break;
        case 6: nodes[0].parameters=&parameter; nodes[0].parameter_count=1; break;
        default: nodes[0].kind=UINT32_MAX; break;
        }
        XrXirTypes types={nodes,6,NULL,NULL}; XrXirTypeArena *arena=NULL;
        CHECK(xr_xir_compile_types_structure_verify(context,&types)!=XR_XIR_OK);
        types.count=5;
        CHECK(xr_xir_compile_type_arena_new(context,&types,&arena)!=XR_XIR_VALUE_OK && !arena);
    }
}
static void obligations(const XrXirCompileContext *context, const XrXirTypes *types) {
    XrXirType generic=(XrXirType)261;
    XrXirFunction function={.parameters=&generic,.parameter_count=1};
    XrXirConstraint fact={.markers=XR_XIR_CONSTRAINT_SENDABLE};
    XrXirGeneric declaration={.constraints=&fact,.parameter_count=1};
    XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,
        .generics=&declaration,.types=types};
    XrXirConstraint required={.markers=XR_XIR_CONSTRAINT_SENDABLE};
    CHECK(xr_xir_compile_type_constraints(context,&module,0,generic,required)==XR_XIR_OK);
    fact.markers=0;
    CHECK(xr_xir_compile_type_constraints(context,&module,0,generic,required)==XR_XIR_BAD_TYPE);
    fact.markers=XR_XIR_CONSTRAINT_SENDABLE;
    CHECK(xr_xir_compile_type_constraints(context,&module,0,(XrXirType)259,required)==XR_XIR_OK);
    CHECK(xr_xir_compile_type_expression_shape(context,types,generic,0)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_expression_shape(context,types,generic,1)==XR_XIR_OK);
    XrXirInferenceRequest request={.types=types,.own_count=1};
    XrXirInferenceState *state=NULL;
    CHECK(xr_xir_compile_inference_begin(context,&request,&state)==XR_XIR_OK);
    CHECK(xr_xir_compile_inference_observe(state,types,(XrXirInferencePair){generic,(XrXirType)257})==XR_XIR_OK);
    XrXirType argument=XR_XIR_UNIT;
    CHECK(xr_xir_compile_inference_finalize(state,types,&argument,1)==XR_XIR_OK && argument==XR_XIR_STRING);
    xr_xir_compile_inference_dispose(state);
}
static void ownership(const XrXirCompileContext *context, XrXirTypeNode *nodes, XrXirTypes types) {
    XrXirTypes *copy=NULL;
    CHECK(xr_xir_compile_types_clone(context,&types, NULL,&copy)==XR_XIR_OK);
    CHECK(copy && copy->nodes!=nodes && copy->nodes[5].element==(XrXirType)XR_XIR_TYPE_PARAMETER_BASE);
    XrXirTypeArena *arena=NULL; types.count=5;
    CHECK(xr_xir_compile_type_arena_new(context,&types,&arena)==XR_XIR_VALUE_OK);
    memset(nodes,0xcc,6*sizeof(*nodes));
    const XrXirTypes *owned=xr_xir_compile_type_arena_types(arena);
    CHECK(owned && owned->count==5 && xr_xir_type_is_channel(owned,(XrXirType)256));
    CHECK(xr_xir_channel_element(owned,(XrXirType)256)==XR_XIR_I64);
    CHECK(xr_xir_channel_element(owned,(XrXirType)257)==XR_XIR_STRING);
    XrXirTypeDescription description={0};
    CHECK(xr_xir_compile_type_description(context,owned,(XrXirType)259,&description)==XR_XIR_OK);
    CHECK(description.kind==XR_XIR_TYPE_CHANNEL && !description.field_count);
    XrXirLayout layout={0};
    CHECK(xr_xir_compile_type_arena_layout(arena,(XrXirType)256,&layout));
    CHECK(layout.size==8 && layout.alignment==8);
    CHECK(copy->nodes[0].kind==XR_XIR_TYPE_CHANNEL && copy->nodes[0].element==XR_XIR_I64);
    xr_xir_compile_types_free(copy); xr_xir_compile_type_arena_drop(arena);
}
int main(void) {
    XrCompileResourceLimits limits={8388608,4194304,128000000};
    XrXirCompileContext context={.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirTypeNode nodes[6]={
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)257},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=(XrXirType)258},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_UNIT},
        {.kind=XR_XIR_TYPE_CHANNEL,.element=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,.parameter_span=1}};
    XrXirTypes types={nodes,6,NULL,NULL};
    CHECK(xr_xir_compile_types_structure_verify(&context,&types)==XR_XIR_OK);
    CHECK(xr_xir_type_is_owned(&types,(XrXirType)256));
    malformed(&context,nodes); obligations(&context,&types); ownership(&context,nodes,types);
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    puts("Channel type candidate: structure/reject/context/Sendable/inference/owned metadata PASS; execution not covered");
    return 0;
}
