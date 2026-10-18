/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_quiescent_storage.c - Private inventory transition and cleanup
 *
 * KEY CONCEPT:
 *   Storage-only rows exercise relocation and later ordinal reads. They do
 *   not admit executable IR or replace independent complete Source checks.
 */
#define main source_retirement_original_checks
#include "test_source_query_retirement.c"
#undef main

static void quiescent_fixture(SourceContext *ctx,XrCompileResources *resources) {
    *ctx=(SourceContext){0};ctx->compile=(XrXirCompileContext){resources,xr_xir_compile_default_limits()};
    ctx->function_count=3;ctx->function_capacity=8;ctx->module_count=1;
    CHECK(source_declaration_storage(ctx,0,0));
    const char *name=source_owned_text(ctx,"owner");CHECK(name);
    XrXirType *parameters=source_alloc(ctx,1,sizeof(*parameters));CHECK(parameters);parameters[0]=XR_XIR_I64;
    XrXirSourceType *query_parameters=source_alloc(ctx,1,sizeof(*query_parameters));CHECK(query_parameters);
    query_parameters[0]=(XrXirSourceType){XR_XIR_I64,0,true};
    for (uint32_t i=0;i<3;++i) {
        ctx->functions[i]=(XrXirFunction){.name=name,.name_length=5,.parameters=parameters,.parameter_count=1,.result=XR_XIR_I64};
        ctx->bodies[i].parameters=parameters;ctx->bodies[i].declaration=i+1;ctx->bodies[i].infer_result=true;
        ctx->identities[i].module=0;
        XrXirSourceDeclaration *d=source_query_append(ctx,ctx->query.declarations,
            &ctx->query.declaration_count,&ctx->declaration_capacity,sizeof(*d));CHECK(d);ctx->query.declarations=d;
        d[i]=(XrXirSourceDeclaration){.id=i+1,.kind=XR_XIR_SOURCE_FUNCTION,.name=name,
            .range={0,2+(int)i,3,2+(int)i,8},.type={XR_XIR_BOOL,0,true},.parameters=query_parameters,.parameter_count=1};
        XrXirSourceReference *r=source_query_append(ctx,ctx->query.references,
            &ctx->query.reference_count,&ctx->reference_capacity,sizeof(*r));CHECK(r);ctx->query.references=r;
        r[i]=(XrXirSourceReference){.range={0,5+(int)i,3,5+(int)i,8},.declaration=i+1,.target=i+1,.access=XR_XIR_SOURCE_READ};
        XrXirSourceExpression *e=source_query_append(ctx,ctx->query.expressions,
            &ctx->query.expression_count,&ctx->expression_capacity,sizeof(*e));CHECK(e);ctx->query.expressions=e;
        e[i]=(XrXirSourceExpression){.node=i+1,.range={0,5+(int)i,3,5+(int)i,8},.type={XR_XIR_BOOL,0,true}};
        CHECK(source_query_syntax_declaration(ctx,d[i].range));
        CHECK(source_query_syntax_reference(ctx,r[i].range));
        XrXirSourceMarker *m=source_query_append(ctx,ctx->syntax.markers,
            &ctx->syntax.marker_count,&ctx->syntax_marker_capacity,sizeof(*m));CHECK(m);ctx->syntax.markers=m;
        m[i]=(XrXirSourceMarker){.range=r[i].range,.declaration=i+1,.role=XR_XIR_SOURCE_MARKER_REF};
    }
    XrXirSourceQueryModule *modules=source_alloc(ctx,4,sizeof(*modules));CHECK(modules);
    modules[0]=(XrXirSourceQueryModule){.identity=name,.path=name};
    ctx->query.modules=modules;ctx->query.module_count=1;ctx->query_module_capacity=4;
    ctx->function=0;CHECK(source_root_query_record(ctx,0,0,false,false));
    ctx->function=1;CHECK(source_root_query_record(ctx,0,0,true,false));ctx->function=0;
}

static void quiescent_rows(const SourceContext *ctx) {
    CHECK(ctx->function_count==3 && ctx->query.declaration_count==3 && ctx->query.expression_count==3);
    for (uint32_t i=0;i<3;++i) {
        CHECK(ctx->functions[i].name_length==5 && !memcmp(ctx->functions[i].name,"owner",5));
        CHECK(ctx->functions[i].parameter_count==1 && ctx->functions[i].parameters[0]==XR_XIR_I64);
        CHECK(ctx->bodies[i].parameters==ctx->functions[i].parameters && ctx->bodies[i].declaration==i+1);
        CHECK(ctx->query.declarations[i].id==i+1 && !memcmp(ctx->query.declarations[i].name,"owner",5));
        CHECK(ctx->query.declarations[i].parameters[0].type==XR_XIR_I64 && ctx->query.references[i].target==i+1);
        CHECK(ctx->syntax.declarations[i].name.line==2+(int)i && ctx->syntax.markers[i].declaration==i+1);
    }
}

static XrCompileResourceStats quiescent_trial(size_t fail,bool comptime,XrCompileResourceLimits caps,
    XrXirStatus expected,size_t *positions,bool reads) {
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&caps,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    SourceContext ctx;quiescent_fixture(&ctx,resources);quiescent_rows(&ctx);
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t begin=source_fixture_compile_attempts;
    source_fixture_compile_fail_at=fail==SIZE_MAX?SIZE_MAX:begin+fail;
    bool compacted=source_quiescent_storage(&ctx);*positions=source_fixture_compile_attempts-begin;
    source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(compacted==(expected==XR_XIR_OK) && ctx.diagnostic.status==expected);
    quiescent_rows(&ctx);
    XrCompileResourceStats measured={0};CHECK(xr_compile_resources_stats(resources,&measured)==XR_COMPILE_RESOURCE_OK);
    if (expected==XR_XIR_OK) {
        CHECK(measured.live_bytes<before.live_bytes && measured.allocated_bytes>before.allocated_bytes && measured.work>before.work);
        CHECK(ctx.function_capacity==3 && ctx.declaration_capacity==3 && ctx.reference_capacity==3 && ctx.expression_capacity==3);
        CHECK(ctx.query_module_capacity==1 && ctx.syntax_declaration_capacity==3 && ctx.syntax_reference_capacity==3 && ctx.syntax_marker_capacity==3);
        if (reads) {
            CHECK(source_root_query_sync(&ctx));
            for (uint32_t i=0;i<3;++i) CHECK(ctx.query.declarations[i].type.type==XR_XIR_I64);
            CHECK(ctx.query.expressions[0].type.type==XR_XIR_I64 && ctx.query.expressions[1].type.type==XR_XIR_BOOL);
            XrCompileResourceStats stable={0},again={0};CHECK(xr_compile_resources_stats(resources,&stable)==XR_COMPILE_RESOURCE_OK);
            CHECK(source_quiescent_storage(&ctx));CHECK(xr_compile_resources_stats(resources,&again)==XR_COMPILE_RESOURCE_OK);
            CHECK(again.allocated_bytes==stable.allocated_bytes && again.live_bytes==stable.live_bytes && again.work>stable.work);
        }
    } else if (fail!=SIZE_MAX) CHECK(source_fixture_compile_injected && *positions==fail+1);
    if (comptime) source_comptime_context_free(&ctx);else source_core_dispose(&ctx);
    XrCompileResourceStats cleaned={0};CHECK(xr_compile_resources_stats(resources,&cleaned)==XR_COMPILE_RESOURCE_OK);
    CHECK(cleaned.live_bytes==baseline.live_bytes && !ctx.memory && !ctx.retired_arrays);
    xr_compile_resources_release(resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    source_fixture_compile_injected=false;return measured;
}

static void quiescent_controls(void) {
    for (unsigned mode=0;mode<2;++mode) {
        XrCompileResourceLimits caps={1048576,1048576,1048576};XrCompileResources *resources=NULL;
        CHECK(xr_compile_resources_new(&caps,&resources)==XR_COMPILE_RESOURCE_OK);
        SourceContext ctx;quiescent_fixture(&ctx,resources);
        if (!mode) ctx.root_flow=source_alloc(&ctx,1,sizeof(*ctx.root_flow));
        else ctx.next_closure=1;
        CHECK(!source_quiescent_storage(&ctx) && ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE);
        quiescent_rows(&ctx);source_core_dispose(&ctx);xr_compile_resources_release(resources);
        CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
}

static void quiescent_empty_inventory(void) {
    XrCompileResourceLimits caps={1048576,1048576,1048576};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&caps,&resources)==XR_COMPILE_RESOURCE_OK);
    SourceContext ctx={0};ctx.compile=(XrXirCompileContext){resources,xr_xir_compile_default_limits()};
    ctx.query.references=source_alloc(&ctx,8,sizeof(*ctx.query.references));CHECK(ctx.query.references);
    ctx.reference_capacity=8;
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t attempts=source_fixture_compile_attempts;
    CHECK(source_quiescent_storage(&ctx));
    CHECK(!ctx.query.references && !ctx.reference_capacity && !ctx.memory);
    CHECK(source_fixture_compile_attempts==attempts);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes<before.live_bytes && after.allocated_bytes==before.allocated_bytes && after.work>before.work);
    source_core_dispose(&ctx);xr_compile_resources_release(resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}

int main(int argc,char **argv) {
    bool full=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || full);
    CHECK(source_retirement_original_checks(argc,argv)==0);quiescent_controls();quiescent_empty_inventory();
    XrCompileResourceLimits caps={1048576,1048576,1048576};
    for (unsigned mode=0;mode<2;++mode) {
        size_t sites=0;XrCompileResourceStats measured=quiescent_trial(SIZE_MAX,mode!=0,caps,XR_XIR_OK,&sites,true);CHECK(sites==11);
        for (size_t point=0;point<sites;++point) {
            if (!full && point && point+1!=sites) continue;
            size_t seen=0;(void)quiescent_trial(point,mode!=0,caps,XR_XIR_OUT_OF_MEMORY,&seen,false);
        }
        XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,measured.work};size_t seen=0;
        (void)quiescent_trial(SIZE_MAX,mode!=0,exact,XR_XIR_OK,&seen,false);
        for (unsigned axis=0;axis<3;++axis) {
            XrCompileResourceLimits smaller=exact;
            if (!axis) --smaller.allocated_bytes;else if (axis==1) --smaller.live_bytes;else --smaller.work;
            (void)quiescent_trial(SIZE_MAX,mode!=0,smaller,XR_XIR_BUDGET,&seen,false);
        }
        printf("Quiescent source storage cleanup=%u sites=%zu injection=%s PASS\n",mode,sites,full?"ALL":"FIRST_LAST_ONLY");
        CHECK(measured.allocated_bytes>measured.live_bytes);
    }
    return 0;
}
