/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_type_access_scratch.c - Current scope and physical pending ownership
 *
 * KEY CONCEPT:
 *   Reused bytes never retain naming permission or a different pool's proof.
 */
#include "xir/xxir_type_scratch_internal.h"
#include "xir/xxir_declarations.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1); } } while (0)
typedef struct Physical { void *pointer; size_t bytes; } Physical;
static Physical physical[1024];
static size_t live, live_bytes, attempts, fail_at=SIZE_MAX;
static bool injected;
static void *queue_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true; return NULL; }
    void *pointer=xr_malloc(bytes); if (!pointer) return NULL;
    CHECK(live<1024 && bytes<=SIZE_MAX-live_bytes);
    physical[live++]=(Physical){pointer,bytes};live_bytes+=bytes;return pointer;
}
static void queue_free(void *pointer) {
    if (!pointer) return;
    size_t i=0;while (i<live && physical[i].pointer!=pointer) ++i;
    CHECK(i<live && live_bytes>=physical[i].bytes);
    live_bytes-=physical[i].bytes;physical[i]=physical[--live];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) queue_malloc(bytes)
#define xr_free(pointer) queue_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

typedef struct AccessFixture {
    XrXirTypeNode nodes[33];
    XrXirTypes types;
    XrXirNominalDeclaration nominal;
    XrXirNominalTable nominals;
    XrXirSourceModule modules[2];
    XrXirFunctionIdentity identities[2];
    XrXirDeclarations declarations;
    XrXirFunction functions[2];
    XrXirInstruction returns[2];
    XrXirBlock blocks[2];
    XrXirModule module;
    uint32_t imported;
    XrXirType root;
} AccessFixture;
static XrXirType constructed(uint32_t i) { return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i); }
static void fixture(AccessFixture *f,uint32_t count) {
    CHECK(count>=2 && count<=33);memset(f,0,sizeof(*f));
    f->nominal=(XrXirNominalDeclaration){.module={"home",4},.name={"Hidden",6},.kind=XR_XIR_NOMINAL_STRUCT};
    f->nominals=(XrXirNominalTable){&f->nominal,1,NULL};
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={.declaration=0}};
    for (uint32_t i=1;i<count;++i) f->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=constructed(i-1)};
    f->types=(XrXirTypes){f->nodes,count,&f->nominals,NULL};f->root=constructed(count-1);
    f->modules[0]=(XrXirSourceModule){"home",4,NULL,0,0};
    f->modules[1]=(XrXirSourceModule){"away",4,NULL,0,1};
    f->identities[0].module=0;f->identities[1].module=1;
    f->declarations=(XrXirDeclarations){.modules=f->modules,.module_count=2,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    for (uint32_t i=0;i<2;++i) {
        f->returns[i]=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
        f->blocks[i]=(XrXirBlock){.first=0,.count=1};
        f->functions[i]=(XrXirFunction){.name=i?"away_init":"home_init",.name_length=9,.result=XR_XIR_UNIT,
            .blocks=&f->blocks[i],.block_count=1,.instructions=&f->returns[i],.instruction_count=1};
    }
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.functions=f->functions,.function_count=2,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_LIBRARY};
}
static XrCompileResourceLimits caps(void) { return (XrCompileResourceLimits){2097152,1048576,2000000}; }
static XrXirCompileContext owner_new(XrCompileResourceLimits limits) {
    CHECK(!live && !live_bytes);XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats result={0};CHECK(xr_compile_resources_stats(context->resources,&result)==XR_COMPILE_RESOURCE_OK);return result;
}
static void owner_free(XrXirCompileContext *context,uint64_t baseline) {
    CHECK(stats(context).live_bytes==baseline);xr_compile_resources_release(context->resources);
    *context=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
static XrXirStatus sequence(const XrXirCompileContext *context,AccessFixture *small,AccessFixture *large) {
    XrXirStatus status=xr_xir_compile_verify(context,&small->module,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_verify(context,&large->module,NULL);
    XirTypeScratch scratch={context->resources,NULL,0};
    if (status==XR_XIR_OK) status=xr_xir_compile_type_access_scratch(context,&small->module,0,small->root,&scratch);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_access_scratch(context,&large->module,0,large->root,&scratch);
    if (status==XR_XIR_OK) status=xr_xir_compile_type_access_scratch(context,&small->module,0,small->root,&scratch);
    xir_type_scratch_free(&scratch);return status;
}
static XrCompileResourceStats measured(XrCompileResourceLimits limits,XrXirStatus expected) {
    AccessFixture small,large;fixture(&small,2);fixture(&large,33);
    XrXirCompileContext context=owner_new(limits);uint64_t baseline=stats(&context).live_bytes;
    CHECK(sequence(&context,&small,&large)==expected);XrCompileResourceStats result=stats(&context);
    owner_free(&context,baseline);return result;
}
static void faults(void) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        AccessFixture small,large;fixture(&small,2);fixture(&large,33);
        XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=sequence(&context,&small,&large);size_t actual=attempts;fail_at=SIZE_MAX;
        if (!pass) { CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites && sites<200); }
        else CHECK(injected && actual==pass && status==XR_XIR_OUT_OF_MEMORY);
        owner_free(&context,baseline);
    }
    printf("type access scratch fresh OOM sites=%zu physical=0/0\n",sites);
}
static void limits(void) {
    XrCompileResourceStats base=measured(caps(),XR_XIR_OK);
    XrCompileResourceLimits exact={base.allocated_bytes,base.peak_bytes,base.work};
    XrCompileResourceStats actual=measured(exact,XR_XIR_OK);
    CHECK(actual.allocated_bytes==base.allocated_bytes && actual.peak_bytes==base.peak_bytes && actual.work==base.work);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)measured(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)measured(less,XR_XIR_BUDGET);
    less=exact;--less.work;(void)measured(less,XR_XIR_BUDGET);
}
static void authority(void) {
    AccessFixture f;fixture(&f,33);XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    CHECK(xr_xir_compile_verify(&context,&f.module,NULL)==XR_XIR_OK);
    XirTypeScratch scratch={context.resources,NULL,0};
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,0,f.root,&scratch)==XR_XIR_OK);
    uint64_t allocations=stats(&context).allocation_count;
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,1,f.root,&scratch)==XR_XIR_BAD_TYPE);
    f.nominal.exported=1;
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,1,f.root,&scratch)==XR_XIR_BAD_TYPE);
    f.imported=0;f.modules[1].dependencies=&f.imported;f.modules[1].dependency_count=1;
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,1,f.root,&scratch)==XR_XIR_OK);
    f.nominal.exported=0;
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,1,f.root,&scratch)==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,0,f.root,&scratch)==XR_XIR_OK);
    f.nodes[32].element=f.root;
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,0,f.root,&scratch)==XR_XIR_BAD_TYPE);
    f.nodes[32].element=constructed(31);
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,0,f.root,&scratch)==XR_XIR_OK);
    CHECK(stats(&context).allocation_count==allocations);
    XrXirCompileContext empty={0};CHECK(xr_xir_compile_type_access_scratch(&empty,&f.module,0,f.root,&scratch)==XR_XIR_BAD_STRUCTURE);
    XrCompileResources *foreign=NULL;XrCompileResourceLimits cap=caps();
    CHECK(xr_compile_resources_new(&cap,&foreign)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext other={.resources=foreign,.limits=context.limits};
    CHECK(xr_xir_compile_type_access_scratch(&other,&f.module,0,f.root,&scratch)==XR_XIR_BAD_STRUCTURE);
    xr_compile_resources_release(foreign);xir_type_scratch_free(&scratch);owner_free(&context,baseline);
}
static void sparse_same_byte_authority(void) {
    AccessFixture f;fixture(&f,33);
    for(uint32_t i=1;i<32;++i)f.nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f.nodes[7]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=constructed(0)};
    XrXirCallableParameter parameters[2]={{constructed(0),0},{constructed(0),0}};
    f.nodes[32]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=constructed(7),.parameters=parameters,.parameter_count=2};
    XrXirCompileContext context=owner_new(caps());uint64_t baseline=stats(&context).live_bytes;
    XirTypeScratch scratch={context.resources,NULL,0};
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,0,f.root,&scratch)==XR_XIR_OK);
    uint64_t allocated=stats(&context).allocation_count;
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,1,f.root,&scratch)==XR_XIR_BAD_TYPE);
    f.nominal.exported=1;f.imported=0;f.modules[1].dependencies=&f.imported;f.modules[1].dependency_count=1;
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,1,f.root,&scratch)==XR_XIR_OK);
    f.nominal.exported=0;
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,1,f.root,&scratch)==XR_XIR_BAD_TYPE);
    f.nodes[7].element=constructed(7);
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,0,f.root,&scratch)==XR_XIR_BAD_TYPE);
    f.nodes[7].element=constructed(0);
    CHECK(xr_xir_compile_type_access_scratch(&context,&f.module,0,f.root,&scratch)==XR_XIR_OK);
    CHECK(stats(&context).allocation_count==allocated);
    xir_type_scratch_free(&scratch);owner_free(&context,baseline);
}

int main(void) { sparse_same_byte_authority();faults();limits();authority();puts("type access scratch fresh authority, growth, fault and three-axis ownership PASS");return 0; }
