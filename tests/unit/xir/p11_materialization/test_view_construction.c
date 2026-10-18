/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_view_construction.c - Real construction admission after SSA insertion
 *
 * KEY CONCEPT:
 *   The Source relocation helper receives genuine materializer and CTFE owners.
 *   Complete admission rejects stale coordinates and accepts relocated owners.
 */
#define main materialization_existing_main
#include "test_materialization.c"
#undef main
#include "xir/xxir_view_normalize_internal.h"
#include "xir/xxir_compile_memory.h"
typedef struct SourceContext {
    XrXirCompileContext compile;XrXirTypes types;uint32_t function_count;
    const XrXirFunction *functions;XrXirConstruction *construction;
    void *allocations[1024];size_t count;XrXirStatus failure;
} SourceContext;
static bool source_fail(SourceContext *ctx,const void *token,XrXirStatus status,const char *message) {
    (void)token;(void)message;ctx->failure=status;return false;
}
static bool source_work(SourceContext *ctx,const void *token) {
    (void)token;return xir_compile_work(&ctx->compile,1) || source_fail(ctx,NULL,XR_XIR_BUDGET,"work exhausted");
}
static void *source_alloc(SourceContext *ctx,size_t count,size_t size) {
    CHECK(ctx->count<1024);void *memory=NULL;
    CHECK(xr_compile_resources_calloc(ctx->compile.resources,count,size,&memory)==XR_COMPILE_RESOURCE_OK);
    ctx->allocations[ctx->count++]=memory;return memory;
}
#include "xir/xxir_source_view_construction.inc.c"
static void shifted_body(SourceContext *ctx,const XrXirModule *module,
    XirViewNormalization *normalization,uint32_t f) {
    const XrXirFunction *old=module->functions+f;XrXirFunction *body=normalization->bodies+f;
    XirViewNormalizedFunction *map=normalization->functions+f;*body=*old;
    map->old_instructions=old->instruction_count;
    map->instructions=source_alloc(ctx,(size_t)old->instruction_count+1,sizeof(*map->instructions));
    map->blocks=source_alloc(ctx,old->block_count,sizeof(*map->blocks));
    map->values=source_alloc(ctx,(size_t)old->parameter_count+old->instruction_count,sizeof(*map->values));
    map->boundaries=source_alloc(ctx,(size_t)old->instruction_count+1,sizeof(*map->boundaries));
    map->operands=old->operand_count?source_alloc(ctx,old->operand_count,sizeof(*map->operands)):NULL;
    map->instructions[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    memcpy(map->instructions+1,old->instructions,(size_t)old->instruction_count*sizeof(*map->instructions));
    memcpy(map->blocks,old->blocks,(size_t)old->block_count*sizeof(*map->blocks));
    if (old->operand_count) memcpy(map->operands,old->operands,(size_t)old->operand_count*sizeof(*map->operands));
    for (uint32_t i=0;i<old->parameter_count;++i) map->values[i]=i;
    for (uint32_t i=0;i<old->instruction_count;++i) map->values[old->parameter_count+i]=old->parameter_count+i+1;
    for (uint32_t i=1;i<=old->instruction_count;++i) map->boundaries[i]=i+1;
    body->instructions=map->instructions;body->instruction_count++;
    body->blocks=map->blocks;body->operands=map->operands;
    XirViewRelocation relocation={module,old,body,map->values,map->boundaries,0,0};
    CHECK(xir_compile_view_relocate(&ctx->compile,&relocation)==XR_XIR_OK);
}
static void relocation_admission(const XrXirCompileContext *context,const XrXirModule *module,
    XrXirConstruction *old,bool comptime) {
    SourceContext ctx={.compile=*context,.types=module->types?*module->types:(XrXirTypes){0},.function_count=module->function_count,
        .functions=module->functions,.construction=old};
    XirViewNormalization n={.module=*module};
    n.bodies=source_alloc(&ctx,ctx.function_count,sizeof(*n.bodies));
    n.functions=source_alloc(&ctx,ctx.function_count,sizeof(*n.functions));
    for (uint32_t f=0;f<ctx.function_count;++f) shifted_body(&ctx,module,&n,f);
    XrXirModule replacement=*module;replacement.stage=XR_XIR_BUILT;replacement.provenance=NULL;replacement.functions=n.bodies;
    XrXirArtifact *artifact=NULL;
    CHECK(xr_xir_compile_check_v2(context,&replacement,old,&artifact,NULL)==XR_XIR_BAD_STRUCTURE && !artifact);
    XrXirConstruction *owner=NULL;CHECK(source_view_construction(&ctx,&n,&owner) && owner && owner!=old);
    if (comptime) {
        const XrXirComptimeTable *before=xr_xir_compile_construction_comptime(old),*after=xr_xir_compile_construction_comptime(owner);
        CHECK(before->site_count && after->site_count==before->site_count && after->binding_count==before->binding_count);
        for (uint32_t i=0;i<before->site_count;++i) CHECK(after->sites[i].instruction==before->sites[i].instruction+1);
    } else {
        uint32_t count=xr_xir_compile_construction_materialization_count(old);CHECK(count);
        for (uint32_t i=0;i<count;++i) {
            const XrXirMaterializationBinding *before=xr_xir_compile_construction_materialization(old,i),
                *after=xr_xir_compile_construction_materialization(owner,i);
            CHECK(after->instruction==before->instruction+1 && after->function==before->function && after->nominal==before->nominal);
        }
    }
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_check_v2(context,&replacement,owner,&artifact,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"relocated check status=%u at %u:%u\n",status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && artifact);xr_xir_compile_artifact_free(artifact);
    xr_xir_compile_construction_free(owner);
    for (size_t i=0;i<ctx.count;++i) xr_compile_resources_free(ctx.allocations[i]);
}
static void generic_sites(const XrXirCompileContext *context) {
    const char text[]="fn compute<T>()->i64{return comptime {const values:Array<T>=[];42}}\n"
        "export fn run()->i64{return compute<i64>()}\n";
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"construction-coordinate",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,sizeof(text)-1};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"generic source status=%u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked);xr_compile_session_free(session);
    relocation_admission(context,&result.checked->module,result.checked->construction,true);
    xr_xir_compile_source_result_free(&result);
}
int main(void) {
    XrXirCompileContext context;context_new(&context);PreparedMaterialization input;prepare(&context,0,&input);
    XrXirConstruction *owner=NULL;const XrXirConstruction *source=input.source.checked->construction;
    XrXirConstructionSpec spec={source->rows,source->count,input.edges,2,NULL};
    CHECK(xr_xir_compile_construction_new_v3(&context,input.module.types,&spec,&owner)==XR_XIR_OK);
    relocation_admission(&context,&input.module,owner,false);
    xr_xir_compile_construction_free(owner);prepared_free(&input);generic_sites(&context);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("Materializer and generic CTFE coordinates: stale owner rejected, fresh owner fully admitted; physical0 PASS");return 0;
}
