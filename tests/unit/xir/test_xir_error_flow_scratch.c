/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_flow_scratch.c - Fresh error facts and operation-owned bytes
 *
 * KEY CONCEPT:
 *   Reused storage does not transfer reachable roots or escaping facts to the
 *   next function. Every actual allocation failure releases compiler owners.
 */
#include "xir/xxir_effects.h"
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
static void *error_malloc(size_t bytes) {
    if (attempts++==fail_at) { injected=true; return NULL; }
    void *pointer=xr_malloc(bytes); if (!pointer) return NULL;
    CHECK(live<1024 && bytes<=SIZE_MAX-live_bytes);
    physical[live++]=(Physical){pointer,bytes};live_bytes+=bytes;return pointer;
}
static void error_free(void *pointer) {
    if (!pointer) return;
    size_t i=0;while (i<live && physical[i].pointer!=pointer) ++i;
    CHECK(i<live && live_bytes>=physical[i].bytes);
    live_bytes-=physical[i].bytes;physical[i]=physical[--live];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) error_malloc(bytes)
#define xr_free(pointer) error_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

/* The real production implementation exposes private storage assertions only
 * in this TU; all inference and flow bodies come from the canonical source. */
#include "xir/xxir_effects.c"
typedef struct ErrorFixture {
    XrXirType error_parameter;
    XrXirInstruction init,throw_op,pure[33],handled[4],rethrow[4];
    XrXirBlock init_block,throw_block,pure_block,branches[3];
    XrXirFunction functions[5];
    XrXirSourceModule source;
    XrXirFunctionIdentity identities[5];
    XrXirDeclarations declarations;
    uint32_t argument;
    XrXirModule module;
} ErrorFixture;
static void fixture(ErrorFixture *f) {
    memset(f,0,sizeof(*f));f->error_parameter=XR_XIR_ERROR;
    f->init=(XrXirInstruction){.op=XR_XIR_RETURN,.type=XR_XIR_UNIT};
    f->throw_op=(XrXirInstruction){.op=XR_XIR_THROW,.type=XR_XIR_UNIT,.args={0}};
    f->init_block=f->throw_block=(XrXirBlock){.first=0,.count=1};
    for(uint32_t i=0;i<32;++i)f->pure[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=i};
    f->pure[32]=f->init;f->pure_block=(XrXirBlock){.first=0,.count=33};
    f->handled[0]=(XrXirInstruction){.op=XR_XIR_INVOKE,.type=XR_XIR_UNIT,.args={0,1},.targets={1,2},.immediate=1};
    f->handled[1]=f->init;
    f->handled[2]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR,.immediate=0};
    f->handled[3]=f->init;memcpy(f->rethrow,f->handled,sizeof(f->handled));
    f->rethrow[3]=(XrXirInstruction){.op=XR_XIR_THROW,.type=XR_XIR_UNIT,.args={3}};
    f->branches[0]=(XrXirBlock){.first=0,.count=1};
    f->branches[1]=(XrXirBlock){.first=1,.count=1};
    f->branches[2]=(XrXirBlock){.first=2,.count=2};
    f->functions[0]=(XrXirFunction){.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&f->init_block,.block_count=1,.instructions=&f->init,.instruction_count=1};
    f->functions[1]=(XrXirFunction){.name="thrower",.name_length=7,.parameters=&f->error_parameter,.parameter_count=1,.result=XR_XIR_UNIT,.blocks=&f->throw_block,.block_count=1,.instructions=&f->throw_op,.instruction_count=1};
    f->functions[2]=(XrXirFunction){.name="pure",.name_length=4,.result=XR_XIR_UNIT,.blocks=&f->pure_block,.block_count=1,.instructions=f->pure,.instruction_count=33};
    f->functions[3]=(XrXirFunction){.name="handled",.name_length=7,.parameters=&f->error_parameter,.parameter_count=1,.result=XR_XIR_UNIT,.blocks=f->branches,.block_count=3,.instructions=f->handled,.instruction_count=4,.operands=&f->argument,.operand_count=1};
    f->functions[4]=f->functions[3];f->functions[4].name="rethrow";f->functions[4].instructions=f->rethrow;
    f->source=(XrXirSourceModule){"errors",6,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_CHECKED,.functions=f->functions,.function_count=5,.declarations=&f->declarations,.linkage_kind=XR_XIR_LIBRARY};
}
static XrCompileResourceLimits caps(void){return (XrCompileResourceLimits){2097152,1048576,2000000};}
static XrXirCompileContext owner_new(XrCompileResourceLimits limits){
    CHECK(!live && !live_bytes);XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);c.limits=xr_xir_compile_default_limits();return c;
}
static XrCompileResourceStats stats(const XrXirCompileContext *c){
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}
static void owner_free(XrXirCompileContext *c,uint64_t baseline){
    CHECK(stats(c).live_bytes==baseline);xr_compile_resources_release(c->resources);*c=(XrXirCompileContext){0};CHECK(!live && !live_bytes);
}
static void golden(const XrXirEffects *e){
    static const XrXirEffect expected[]={XR_XIR_EFFECT_NONE,XR_XIR_EFFECT_MAY,XR_XIR_EFFECT_NONE,XR_XIR_EFFECT_NONE,XR_XIR_EFFECT_MAY};
    for(uint32_t f=0;f<5;++f){const XrXirFunctionEffects *v=xr_xir_effects_function(e,f);CHECK(v && v->suspend==XR_XIR_EFFECT_NONE && v->throws==expected[f]);
        CHECK(!xr_xir_effects_error_unknown(e,f));CHECK(xr_xir_effects_error_unidentified(e,f)==(f==1 || f==4));}
}
static XrXirStatus whole(const XrXirCompileContext *context){
    ErrorFixture f;fixture(&f);XrXirStatus status=xr_xir_compile_verify(context,&f.module,NULL);XrXirEffects *effects=NULL;
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_infer_verified(context,&f.module,&effects);
    if(status==XR_XIR_OK){CHECK(effects);golden(effects);}else CHECK(!effects);
    xr_xir_compile_effects_free(effects);return status;
}
static XrCompileResourceStats measured(XrCompileResourceLimits limits,XrXirStatus expected){
    XrXirCompileContext c=owner_new(limits);uint64_t baseline=stats(&c).live_bytes;
    CHECK(whole(&c)==expected);XrCompileResourceStats s=stats(&c);owner_free(&c,baseline);return s;
}
static void faults(void){
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass){XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
        attempts=0;injected=false;fail_at=pass?pass-1:SIZE_MAX;XrXirStatus status=whole(&c);size_t actual=attempts;fail_at=SIZE_MAX;
        if(!pass){CHECK(status==XR_XIR_OK);sites=actual;CHECK(sites && sites<512);}else CHECK(injected && actual==pass && status==XR_XIR_OUT_OF_MEMORY);
        owner_free(&c,baseline);
    }
    printf("error flow full current verifier+inference OOM sites=%zu physical=0/0\n",sites);
}
static void limits(void){
    XrCompileResourceStats a=measured(caps(),XR_XIR_OK);XrCompileResourceLimits exact={a.allocated_bytes,a.peak_bytes,a.work};
    XrCompileResourceStats b=measured(exact,XR_XIR_OK);CHECK(a.allocated_bytes==b.allocated_bytes && a.peak_bytes==b.peak_bytes && a.work==b.work);
    XrCompileResourceLimits less=exact;--less.allocated_bytes;(void)measured(less,XR_XIR_BUDGET);
    less=exact;--less.live_bytes;(void)measured(less,XR_XIR_BUDGET);less=exact;--less.work;(void)measured(less,XR_XIR_BUDGET);
}
static void storage_freshness(void){
    ErrorFixture f;fixture(&f);XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(xr_xir_compile_verify(&c,&f.module,NULL)==XR_XIR_OK);
    XrXirFunctionEffects functions[5]={{0}};uint64_t errors[5]={0};XrXirEffects effects={0};
    effects.count=5;effects.words=1;effects.functions=functions;effects.errors=errors;
    EffectTerms terms={0};terms.remaining=&c;ErrorFlow flow={0};flow.module=&f.module;flow.effects=&effects;flow.remaining=&c;flow.terms=&terms;
    attempts=0;CHECK(error_function(&flow,1)==XR_XIR_OK && errors[1]==2);CHECK(attempts==1);
    size_t small_capacity=flow.storage_capacity;
    CHECK(error_function(&flow,2)==XR_XIR_OK && !errors[2]);CHECK(attempts==2 && flow.storage_capacity>small_capacity);
    uint8_t *large=flow.storage;size_t capacity=flow.storage_capacity;
    memset(flow.storage,0xff,capacity);
    CHECK(error_function(&flow,1)==XR_XIR_OK && errors[1]==2 && flow.storage==large && flow.storage_capacity==capacity && attempts==2);
    CHECK(error_function(&flow,2)==XR_XIR_OK && !errors[2] && flow.storage==large && attempts==2);
    error_storage_free(&flow);effect_terms_free(&terms);CHECK(!flow.storage && !flow.storage_capacity && !flow.states && !flow.roots && !flow.reachable);
    owner_free(&c,baseline);
}
static void negative_body(void){
    ErrorFixture f;fixture(&f);XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    CHECK(xr_xir_compile_verify(&c,&f.module,NULL)==XR_XIR_OK);
    f.throw_op.op=(XrXirOp)UINT32_MAX;CHECK(xr_xir_compile_verify(&c,&f.module,NULL)==XR_XIR_BAD_STRUCTURE);
    f.throw_op.op=XR_XIR_THROW;CHECK(xr_xir_compile_verify(&c,&f.module,NULL)==XR_XIR_OK);
    owner_free(&c,baseline);
}
int main(void){storage_freshness();negative_body();faults();limits();puts("error flow fresh facts, owned scratch, actual OOM and three finite axes PASS");return 0;}
