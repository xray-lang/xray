/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_dependency_worklist.c - Finite dependency-driven error facts
 *
 * KEY CONCEPT:
 *   Independent literal summaries cover storage order, recursion and task edges.
 *   Full checking and inference retain all resource failures and physical frees.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_declarations.h"
#include "xir/xxir_generic.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1); } } while (0)
typedef struct DependencyPhysical {void *pointer;size_t bytes;} DependencyPhysical;
static DependencyPhysical physical[4096];
static size_t live,live_bytes,attempts,fail_at=SIZE_MAX;
static bool injected;
static void *ed_malloc(size_t bytes){
    if(attempts++==fail_at){injected=true;return NULL;}
    void *pointer=xr_malloc(bytes);if(!pointer)return NULL;
    CHECK(live<4096 && bytes<=SIZE_MAX-live_bytes);
    physical[live++]=(DependencyPhysical){pointer,bytes};live_bytes+=bytes;return pointer;
}
static void ed_free(void *pointer){
    if(!pointer)return;
    size_t i=0;while(i<live && physical[i].pointer!=pointer)++i;
    CHECK(i<live && live_bytes>=physical[i].bytes);
    live_bytes-=physical[i].bytes;physical[i]=physical[--live];xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) ed_malloc(bytes)
#define xr_free(pointer) ed_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#include "xir/xxir_effects.c"
#include "xir_error_dependency_fixture.h"
static XrCompileResourceLimits ed_caps(void){return (XrCompileResourceLimits){16777216,4194304,16000000};}
static XrXirCompileContext ed_owner(XrCompileResourceLimits limits){
    CHECK(!live && !live_bytes);XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&limits,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();return c;
}
static XrCompileResourceStats ed_stats(const XrXirCompileContext *c){
    XrCompileResourceStats s={0};CHECK(xr_compile_resources_stats(c->resources,&s)==XR_COMPILE_RESOURCE_OK);return s;
}
static void ed_release(XrXirCompileContext *c,uint64_t baseline){
    CHECK(ed_stats(c).live_bytes==baseline);xr_compile_resources_release(c->resources);*c=(XrXirCompileContext){0};
    CHECK(!live && !live_bytes);
}
static void ed_literals(const XrXirEffects *e,ErrorDependencyMode mode){
    CHECK(e && e->count==ED_COUNT && e->atom_count==2 && e->words==1);
    for(uint32_t f=0;f<ED_COUNT;++f){
        uint64_t expected=f<2?0:
            f==2 && mode==ED_UNKNOWN?UINT64_C(1):UINT64_C(12);
        CHECK(e->errors[f]==expected);
        CHECK(xr_xir_effects_error_unknown(e,f)==(expected==1));
        CHECK(!xr_xir_effects_error_unidentified(e,f));
        CHECK(xr_xir_effects_error(e,f,(XrXirType)ED_ENUM,0)==(expected==12));
        CHECK(xr_xir_effects_error(e,f,(XrXirType)ED_ENUM,1)==(expected==12));
        CHECK(e->functions[f].throws==(expected==1?XR_XIR_EFFECT_UNKNOWN:expected?XR_XIR_EFFECT_MAY:XR_XIR_EFFECT_NONE));
        CHECK(e->root[f].requires_root==(f==0) && e->root[f].unresolved==(f==2 && mode==ED_UNKNOWN));
        CHECK(e->functions[f].suspend==(f==2 && mode==ED_GO?XR_XIR_EFFECT_MAY:
            f==2 && mode==ED_UNKNOWN?XR_XIR_EFFECT_UNKNOWN:XR_XIR_EFFECT_NONE));
        CHECK(e->task_errors[f]==(expected==1?XR_XIR_BAD_TYPE:XR_XIR_OK));
    }
}
static XrXirStatus ed_whole(const XrXirCompileContext *c,ErrorDependencyMode mode,bool large){
    ErrorDependencyFixture f;ed_fixture(&f,mode,large);XrXirDiagnostic d={0};
    XrXirStatus status=xir_fixture_verify(c, &f.module, &d);XrXirEffects *e=NULL;
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_infer_verified(c,&f.module,&e);
    if(status==XR_XIR_OK)ed_literals(e,mode);else CHECK(!e);
    if(status!=XR_XIR_OK && fail_at==SIZE_MAX)fprintf(stderr,"dependency mode%u status%u f%u b%u i%u\n",
        (unsigned)mode,(unsigned)status,d.function,d.block,d.instruction);
    xr_xir_compile_effects_free(e);return status;
}
static void ed_cost(void){
    ErrorDependencyFixture f;ed_fixture(&f,ED_DIRECT,true);XrXirCompileContext c=ed_owner(ed_caps());
    uint64_t baseline=ed_stats(&c).live_bytes;CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    uint64_t before=ed_stats(&c).work;XrXirEffects *e=NULL;
    CHECK(xr_xir_compile_effects_infer_verified(&c,&f.module,&e)==XR_XIR_OK);ed_literals(e,ED_DIRECT);
    uint64_t work=ed_stats(&c).work-before;
    CHECK(work<UINT64_C(400000));
    _Static_assert(UINT64_C(32)*513*4*8>UINT64_C(400000),"old global storage copies exceed literal ceiling");
    xr_xir_compile_effects_free(e);ed_release(&c,baseline);
    printf("dependency inference after full check: 32 reverse calls, 512 scalars, work%llu < literal400000\n",(unsigned long long)work);
}
static void ed_graph_isolation(void){
    ErrorDependencyFixture f;ed_fixture(&f,ED_GO,false);XrXirCompileContext c=ed_owner(ed_caps());uint64_t baseline=ed_stats(&c).live_bytes;
    CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_OK);
    XrXirEffects *e=NULL;CHECK(xr_xir_compile_effects_infer_verified(&c,&f.module,&e)==XR_XIR_OK);ed_literals(e,ED_GO);
    EffectGraph graph={0};CHECK(effect_graph_build(&f.module,e,&graph,&c)==XR_XIR_OK);
    for(uint32_t link=graph.heads[3];link!=UINT32_MAX;link=graph.edges[link].next)CHECK(graph.edges[link].caller!=2);
    CHECK(!graph.error_heads && !graph.error_edges && !e->root[2].requires_root && !e->root[2].unresolved);
    CHECK(error_graph_prepare(&f.module,&graph,&c)==XR_XIR_OK);
    CHECK(graph.error_heads && graph.error_edges && graph.error_heads[3]==0);
    CHECK(graph.error_edges[0].caller==2 && graph.error_edges[0].instruction==0 && graph.error_edges[0].next==UINT32_MAX);
    for(uint32_t link=graph.heads[3];link!=UINT32_MAX;link=graph.edges[link].next)CHECK(graph.edges[link].caller!=2);
    CHECK(!e->root[2].requires_root && !e->root[2].unresolved);
    effect_graph_free(&graph);xr_xir_compile_effects_free(e);ed_release(&c,baseline);
}
static void ed_negative(void){
    ErrorDependencyFixture f;ed_fixture(&f,ED_DIRECT,false);XrXirCompileContext c=ed_owner(ed_caps());uint64_t baseline=ed_stats(&c).live_bytes;
    f.scalar[0].op=(XrXirOp)UINT32_MAX;CHECK(xir_fixture_verify(&c, &f.module, NULL)==XR_XIR_BAD_STRUCTURE);
    f.scalar[0].op=XR_XIR_CONST_INT;CHECK(ed_whole(&c,ED_DIRECT,false)==XR_XIR_OK);
    /* Checked CFGs require every block to be reachable; never infer from a rejected graph. */
    ed_fixture(&f,ED_UNREACHABLE,false);XrXirDiagnostic diagnostic={0};
    CHECK(xir_fixture_verify(&c, &f.module, &diagnostic)==XR_XIR_BAD_STRUCTURE);
    CHECK(diagnostic.function==2 && diagnostic.block==1 && diagnostic.instruction==1);
    CHECK(ed_stats(&c).live_bytes==baseline);ed_release(&c,baseline);
}
/* Sixty-one enum atoms plus one symbolic parameter fill a one-word set.
 * Substitution creates atom 62 at bit 64, requiring both restart and resize. */
static XrXirStatus ed_restart_case(const XrXirCompileContext *c,bool payload){
    XrXirConstraint constraint={.markers=XR_XIR_CONSTRAINT_SENDABLE};
    XrXirType parameter=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE,concrete=XR_XIR_I64;
    XrXirNominalVariant variants[61];char names[61][4];
    for(uint32_t i=0;i<61;++i){
        names[i][0]='V';names[i][1]=(char)('0'+i/10);names[i][2]=(char)('0'+i%10);names[i][3]=0;
        variants[i]=(XrXirNominalVariant){{names[i],3},0,payload && i==60?1u:0u};
    }
    XrXirNominalField field={{"value",5},parameter,0};
    XrXirNominalDeclaration nominal={.module={"restart",7},.name={"Failure",7},.exported=1,
        .constraints=&constraint,.parameter_count=1,.fields=payload?&field:NULL,.field_count=payload?1u:0u,
        .kind=XR_XIR_NOMINAL_ENUM,.variants=variants,.variant_count=61};
    XrXirNominalTable nominals={.declarations=&nominal,.count=1};
    XrXirTypeNode node={.kind=XR_XIR_TYPE_NOMINAL,.parameter_span=1,.nominal={.arguments=&parameter,.argument_count=1}};
    XrXirTypes types={.nodes=&node,.count=1,.nominals=&nominals};
    XrXirInstruction init={.op=XR_XIR_RETURN};
    XrXirInstruction call[2]={{.op=XR_XIR_CALL,.args={0,1},.immediate=2,.type_arguments={0,1}},init};
    XrXirInstruction leaf[2]={{.op=XR_XIR_ENUM_NEW,.type=(XrXirType)256,.args={0,payload?1u:0u},.immediate=60},
        {.op=XR_XIR_THROW,.args={1}}};
    XrXirBlock one={.count=1},two={.count=2};uint32_t operand=0;
    XrXirFunction functions[3]={
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&one,.block_count=1,.instructions=&init,.instruction_count=1},
        {.name="apply",.name_length=5,.parameters=&concrete,.parameter_count=1,.result=XR_XIR_UNIT,
            .blocks=&two,.block_count=1,.instructions=call,.instruction_count=2,.operands=&operand,.operand_count=1},
        {.name="raise",.name_length=5,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_UNIT,
            .blocks=&two,.block_count=1,.instructions=leaf,.instruction_count=2,.operands=payload?&operand:NULL,.operand_count=payload?1u:0u}};
    XrXirGeneric generics[3]={{0},{.arguments=&concrete,.argument_count=1},{.constraints=&constraint,.parameter_count=1}};
    XrXirSourceModule source={.name="restart",.name_length=7,.initializer=0};XrXirFunctionIdentity identities[3]={{0}};
    XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirModule module={.stage=XR_XIR_CHECKED,.functions=functions,.function_count=3,.declarations=&declarations,
        .generics=generics,.types=&types,.linkage_kind=XR_XIR_LIBRARY};
    XrXirDiagnostic d={0};XrXirStatus status=xir_fixture_verify(c, &module, &d);XrXirEffects *e=NULL;
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_infer_verified(c,&module,&e);
    if(status==XR_XIR_OK){
        CHECK(e && e->count==3 && e->atom_count==63 && e->words==2 && e->source_type_count==1);
        CHECK(e->errors[0]==0 && e->errors[1]==0 && e->errors[2]==0 && e->errors[3]==1);
        CHECK(e->errors[4]==(UINT64_C(1)<<62) && e->errors[5]==0);
        CHECK(e->atoms[62].type==(XrXirType)257 && e->atoms[62].variant==60);
        for(uint32_t f=0;f<3;++f){
            CHECK(e->functions[f].throws==(f?XR_XIR_EFFECT_MAY:XR_XIR_EFFECT_NONE));
            CHECK(e->task_errors[f]==(payload && f?XR_XIR_BAD_TYPE:XR_XIR_OK));
            CHECK(e->root[f].requires_root==(f==0) && !e->root[f].unresolved);
            CHECK(!xr_xir_effects_error_unknown(e,f) && !xr_xir_effects_error_unidentified(e,f));
        }
        CHECK(xr_xir_effects_error(e,2,(XrXirType)256,60));
        CHECK(!xr_xir_effects_error(e,1,(XrXirType)257,60));
    }else CHECK(!e);
    if(status!=XR_XIR_OK && fail_at==SIZE_MAX)fprintf(stderr,"restart status%u f%u b%u i%u\n",
        (unsigned)status,d.function,d.block,d.instruction);
    xr_xir_compile_effects_free(e);return status;
}
/* Phantom parameters still affect identity. Payload enums lacking authentic
 * projections remain an independent worker-error qualification rejection. */
static XrXirStatus ed_restart_whole(const XrXirCompileContext *c){return ed_restart_case(c,false);}
#include "xir_error_dependency_resources.h"
int main(void){
    for(unsigned mode=ED_DIRECT;mode<=ED_UNKNOWN;++mode){
        XrXirCompileContext c=ed_owner(ed_caps());uint64_t baseline=ed_stats(&c).live_bytes;
        CHECK(ed_whole(&c,(ErrorDependencyMode)mode,false)==XR_XIR_OK);ed_release(&c,baseline);
    }
    {XrXirCompileContext c=ed_owner(ed_caps());uint64_t baseline=ed_stats(&c).live_bytes;
        CHECK(ed_restart_whole(&c)==XR_XIR_OK);ed_release(&c,baseline);}
    {XrXirCompileContext c=ed_owner(ed_caps());uint64_t baseline=ed_stats(&c).live_bytes;
        CHECK(ed_restart_case(&c,true)==XR_XIR_OK);ed_release(&c,baseline);}
    ed_cost();ed_graph_isolation();ed_negative();ed_faults();ed_limits();ed_occupied();
    puts("dependency error summaries, isolated GO edges, complete resource ownership PASS");return 0;
}
