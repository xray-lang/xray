/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_refinement.c - Conservative pre-bottom snapshot selection
 *
 * KEY CONCEPT:
 *   Live ground flow can omit a bound snapshot, while uncertain flow retains it.
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1); } } while (0)
#include "xir/xxir_effect_contract_internal.h"
static unsigned snapshots;
static XrXirStatus observed_snapshot(const XrXirCompileContext *context, XirEffectInput input,
    const struct XrXirEffects *effects) {
    ++snapshots;
    return xir_effects_refinement_capture_input(context,input,effects);
}
#define xir_effects_refinement_capture_input observed_snapshot
#include "xir/xxir_source.c"
#undef xir_effects_refinement_capture_input
static XrXirType constructed(uint32_t n) {return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n);}
static XrXirType parameter(uint32_t n) {return (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+n);}
static void refinement_snapshot_selection(void) {
    XrXirTypeNode nodes[3]={{.kind=XR_XIR_TYPE_CELL,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_ARRAY,.element=parameter(0),.parameter_span=1},
        {.kind=XR_XIR_TYPE_MAP,.element=XR_XIR_I64,.result=XR_XIR_STRING}};
    XrXirType parameters[]={XR_XIR_I64};
    XrXirInstruction instruction={.op=XR_XIR_CELL_NEW,.type=constructed(0)};
    XrXirFunction function={.parameters=parameters,.parameter_count=1,.result=XR_XIR_I64,
        .error_type=XR_XIR_NEVER,.instructions=&instruction,.instruction_count=1};
    SourceRootValue value={.declared=constructed(0)};
    bool capture=false;
    SourceFunction source={.root_values=&value,.root_captures=&capture,.root_capture_count=1};
    XrXirGeneric generic={0};
    XrXirSlot slot={.type=XR_XIR_I64};
    SourceContext ctx={.types={nodes,3,NULL,NULL},.bodies=&source,.functions=&function,
        .function_count=1,.slots=&slot,.slot_count=1,.has_generics=true,.generics=&generic};
    CHECK(!source_root_snapshot_needed(&ctx));
    source.type_parameter_count=1;CHECK(source_root_snapshot_needed(&ctx));source.type_parameter_count=0;
    generic.parameter_count=1;CHECK(source_root_snapshot_needed(&ctx));generic.parameter_count=0;
    source.infer_result=true;CHECK(source_root_snapshot_needed(&ctx));source.infer_result=false;
    capture=true;CHECK(source_root_snapshot_needed(&ctx));capture=false;
    value.declared=constructed(1);CHECK(source_root_snapshot_needed(&ctx));value.declared=constructed(0);
    parameters[0]=parameter(0);CHECK(source_root_snapshot_needed(&ctx));parameters[0]=XR_XIR_I64;
    instruction.type=(XrXirType)4;CHECK(source_root_snapshot_needed(&ctx));instruction.type=constructed(0);
    instruction.type=(XrXirType)18;CHECK(source_root_snapshot_needed(&ctx));instruction.type=constructed(0);
    function.result=parameter(0);CHECK(source_root_snapshot_needed(&ctx));function.result=XR_XIR_I64;
    function.error_type=(XrXirType)UINT32_MAX;CHECK(source_root_snapshot_needed(&ctx));function.error_type=XR_XIR_NEVER;
    slot.type=(XrXirType)UINT32_MAX;CHECK(source_root_snapshot_needed(&ctx));slot.type=XR_XIR_I64;
    nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .element=XR_XIR_NEVER,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    nodes[2].result=constructed(1);value.declared=constructed(2);
    /* The generic contains-callable walker does not visit Map.result. The
     * pool guard must still preserve the snapshot for this nested callable. */
    CHECK(!source_root_type(&ctx,value.declared));
    CHECK(source_root_snapshot_needed(&ctx));
    value.declared=constructed(0);CHECK(source_root_snapshot_needed(&ctx));
    nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=parameter(0),.parameter_span=1};
    nodes[2].result=XR_XIR_STRING;CHECK(!source_root_snapshot_needed(&ctx));
    XrCompileResourceLimits limits={0};
    CHECK(xr_compile_resources_new(&limits,&ctx.compile.resources)==XR_COMPILE_RESOURCE_OK);
    ctx.compile.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(ctx.compile.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrModuleGraph graph={0};ctx.graph=&graph;
    CHECK(source_root_prepare(&ctx) && ctx.root_flow && ctx.types.count==3);
    CHECK(!source_root_snapshot_needed(&ctx) && instruction.type==constructed(0) &&
        value.declared==constructed(0) && slot.type==XR_XIR_I64);
    while (ctx.memory) {SourceMemory *next=ctx.memory->next;xr_compile_resources_free(ctx.memory);ctx.memory=next;}
    CHECK(xr_compile_resources_stats(ctx.compile.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(ctx.compile.resources);
    puts("Source refinement snapshot: ground flow only; generic, capture, unknown and nested callable retain owner PASS");
}
static void actual_source(const char *text, bool snapshot) {
    XrCompileResourceLimits limits={0};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"refinement",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={.text=text,.length=strlen(text)};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    snapshots=0;
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"source %u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot && (snapshots!=0)==snapshot);
    xr_compile_session_free(session);
    xr_xir_compile_source_result_free(&result);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
    xr_compile_resources_release(context.resources);
    printf("Actual Source snapshot=%u complete checked+owned snapshot retired PASS\n",(unsigned)snapshot);
}
int main(void) {
    refinement_snapshot_selection();
    actual_source("export fn answer()->i64{return 42}\n",false);
    actual_source("fn id<T>(x:T)->T{return x}\nexport fn answer()->i64{return id(42)}\n",true);
    actual_source("export fn answer()->i64{const n=42;const f=fn()->i64{return n};return f()}\n",true);
    actual_source("fn value()->i64{return 42}\nexport fn answer()->i64{const items=[value];return items[0]()}\n",true);
    return 0;
}
