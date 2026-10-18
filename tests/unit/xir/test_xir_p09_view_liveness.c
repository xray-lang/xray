/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_view_liveness.c - Independent borrowed SSA demand on real edges
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_view_liveness_internal.h"
#include "tuple_owner_observer.h"
#define LIVE_SHARED ((XrXirType)256)
#define LIVE_MUT ((XrXirType)257)
#define LIVE_OPTIONAL ((XrXirType)258)
#define LIVE_PAIR ((XrXirType)259)
typedef struct LiveFixture {
    XrXirTypeNode nodes[6];XrXirCallableParameter fields[2];XrXirTypes types;
    XrXirType parameters[4];XrXirInstruction ops[32],empty;XrXirBlock blocks[8],one;
    uint32_t operands[64];XrXirFunction functions[3];XrXirModule module;
    XrXirSourceModule source;XrXirFunctionIdentity identities[3];XrXirDeclarations declarations;
} LiveFixture;
typedef struct LiveObserved {
    uint64_t inventory;
    uint64_t before[32],after[32],entries[8],edge_before[8][8],edge_after[8][8];
    bool instructions[32],blocks[8],edges[8][8],panic[8][8];uint32_t events;
} LiveObserved;
typedef struct LiveLarge {
    LiveFixture fixture;XrXirTypeNode nodes[71];XrXirCallableParameter fields[69][2];
    XrXirType parameters[130];XrXirInstruction ops[131];XrXirBlock block;
} LiveLarge;
static uint64_t live_bit(uint32_t value) {CHECK(value<64);return UINT64_C(1)<<value;}
static void live_fixture(LiveFixture *f,uint32_t count) {
    *f=(LiveFixture){0};f->fields[0]=(XrXirCallableParameter){LIVE_SHARED,XR_PARAM_READ};f->fields[1]=f->fields[0];
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_I64};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=LIVE_SHARED};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=f->fields,.parameter_count=2};
    f->nodes[4]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    f->nodes[5]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)260};
    f->types=(XrXirTypes){f->nodes,6,NULL,NULL};
    f->parameters[0]=XR_XIR_BOOL;f->parameters[1]=LIVE_SHARED;f->parameters[2]=LIVE_MUT;f->parameters[3]=(XrXirType)261;
    f->empty=(XrXirInstruction){.op=XR_XIR_RETURN};f->ops[count-1]=f->empty;
    f->one=(XrXirBlock){.count=1};f->blocks[0]=(XrXirBlock){.count=count};
    f->functions[0]=(XrXirFunction){.name="flow",.name_length=4,.parameters=f->parameters,.parameter_count=4,
        .instructions=f->ops,.instruction_count=count,.blocks=f->blocks,.block_count=1};
    f->functions[1]=(XrXirFunction){.name="init",.name_length=4,.instructions=&f->empty,.instruction_count=1,.blocks=&f->one,.block_count=1};
    f->functions[2]=(XrXirFunction){.name="callee",.name_length=6,.parameters=f->parameters+1,.parameter_count=1,
        .instructions=&f->empty,.instruction_count=1,.blocks=&f->one,.block_count=1};
    f->source=(XrXirSourceModule){.name="liveness",.name_length=8,.initializer=1};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=3,
        .types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_LIBRARY};
}
static uint64_t live_observed_mask(const XrXirViewLiveSet *set) {
    uint64_t result=0;CHECK(set->words==set->count/64u+(set->count%64u!=0));
    for (uint32_t i=0;i<set->count;++i) if (set->bits[i/64u]&(UINT64_C(1)<<(i%64u))) result|=live_bit(set->values[i]);
    return result;
}
static XrXirStatus live_observe(void *user,const XrXirCompileContext *context,const XrXirViewLiveEvent *e) {
    if (e->function) return XR_XIR_OK;
    if (!xir_compile_work(context,(uint64_t)e->before.count*2u+1u)) return XR_XIR_BUDGET;
    LiveObserved *o=user;++o->events;CHECK(e->block<8);
    uint64_t before=live_observed_mask(&e->before),after=live_observed_mask(&e->after);
    if (e->kind==XR_XIR_VIEW_LIVE_ENTRY) {
        CHECK(!o->blocks[e->block]);o->blocks[e->block]=true;o->entries[e->block]=before;CHECK(before==after);
        for (uint32_t i=0;i<e->before.count;++i) o->inventory|=live_bit(e->before.values[i]);
    }
    else if (e->kind==XR_XIR_VIEW_LIVE_INSTRUCTION) {
        CHECK(e->instruction<32 && !o->instructions[e->instruction]);o->instructions[e->instruction]=true;
        o->before[e->instruction]=before;o->after[e->instruction]=after;
    } else {
        CHECK(e->kind==XR_XIR_VIEW_LIVE_EDGE && e->target<8);o->edges[e->block][e->target]=true;
        o->edge_before[e->block][e->target]=before;o->edge_after[e->block][e->target]=after;o->panic[e->block][e->target]=e->panic;
    }
    return XR_XIR_OK;
}
static XrXirStatus live_prove(const XrXirCompileContext *c,const LiveFixture *f,LiveObserved *output) {
    *output=(LiveObserved){0};return xir_compile_view_liveness_verified(c,&f->module,live_observe,output,NULL);
}
static void live_straight(LiveFixture *f) {
    live_fixture(f,4);
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=LIVE_SHARED,.args={1,0}};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={4,0}};
    f->ops[2]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={1,0}};
}
static void live_reborrow(LiveFixture *f) {
    live_fixture(f,7);f->functions[0].operand_count=6;
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2};
    f->ops[2]=(XrXirInstruction){.op=XR_XIR_VIEW_NEW,.type=LIVE_MUT,.args={0,3}};
    f->ops[3]=(XrXirInstruction){.op=XR_XIR_VIEW_REBORROW,.type=LIVE_SHARED,.args={3,3}};
    f->ops[4]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={7,0}};
    f->ops[5]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={6,0}};
    const uint32_t operands[]={3,4,5,6,4,5};memcpy(f->operands,operands,sizeof(operands));
}
static void live_wrappers(LiveFixture *f) {
    live_fixture(f,7);f->functions[0].operand_count=2;f->operands[0]=f->operands[1]=1;
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=LIVE_OPTIONAL,.args={1,0}};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_TUPLE_NEW,.type=LIVE_PAIR,.args={0,2}};
    f->ops[2]=(XrXirInstruction){.op=XR_XIR_NULLABLE_IS_SOME,.type=XR_XIR_BOOL,.args={4,0}};
    f->ops[3]=(XrXirInstruction){.op=XR_XIR_TUPLE_FIELD,.type=LIVE_SHARED,.args={5,0}};
    f->ops[4]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={7,0}};
    f->ops[5]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={2,0}};
}
static void live_diamond(LiveFixture *f) {
    live_fixture(f,8);f->functions[0].block_count=4;f->functions[0].operand_count=4;
    const XrXirBlock blocks[]={{.count=1},{.first=1,.count=2},{.first=3,.count=2},{.first=5,.count=3},{.first=8,.count=2}};
    memcpy(f->blocks,blocks,sizeof(blocks));
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0,0},.targets={1,2}};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=LIVE_SHARED,.args={1,0}};
    f->ops[2]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={3,0}};
    f->ops[3]=f->ops[1];f->ops[4]=f->ops[2];
    f->ops[5]=(XrXirInstruction){.op=XR_XIR_PHI,.type=LIVE_SHARED,.args={0,4}};
    const uint32_t operands[]={1,5,2,7};memcpy(f->operands,operands,sizeof(operands));
    f->ops[6]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={9,0}};
    f->ops[7]=f->empty;
}
static void live_branch(LiveFixture *f,bool invoke) {
    live_fixture(f,5);f->functions[0].block_count=3;
    f->blocks[0]=(XrXirBlock){.count=1};f->blocks[1]=(XrXirBlock){.first=1,.count=2};f->blocks[2]=(XrXirBlock){.first=3,.count=2};
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0,0},.targets={1,2}};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={1,0}};
    f->ops[2]=f->empty;f->ops[3]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={2,0}};
    if (invoke) {
        f->functions[0].instruction_count=6;f->functions[0].operand_count=1;f->operands[0]=1;f->blocks[2].count=3;
        f->ops[0]=(XrXirInstruction){.op=XR_XIR_INVOKE,.args={0,1},.targets={1,2},.immediate=2};
        f->ops[4]=f->ops[3];f->ops[3]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR};f->ops[5]=f->empty;
    }
}
static void live_loop(LiveFixture *f) {
    live_fixture(f,7);f->parameters[2]=LIVE_SHARED;f->functions[0].block_count=3;f->functions[0].operand_count=8;
    f->blocks[0]=(XrXirBlock){.count=1};f->blocks[1]=(XrXirBlock){.first=1,.count=4};f->blocks[2]=(XrXirBlock){.first=5,.count=2};
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1,0}};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_PHI,.type=LIVE_SHARED,.args={0,4}};
    f->ops[2]=f->ops[1];f->ops[2].args[0]=4;
    const uint32_t operands[]={0,1,1,6,0,2,1,5};memcpy(f->operands,operands,sizeof(operands));
    f->ops[3]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={5,0}};
    f->ops[4]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={0,0},.targets={1,2}};
    f->ops[5]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={6,0}};
}
static void live_panic(LiveFixture *f) {
    live_fixture(f,7);f->functions[0].block_count=3;
    f->blocks[0]=(XrXirBlock){.count=1};f->blocks[1]=(XrXirBlock){.first=1,.count=3,.panic=2};f->blocks[2]=(XrXirBlock){.first=4,.count=3};
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1,0}};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=LIVE_SHARED,.args={1,0}};
    f->ops[2]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={5,0}};f->ops[3]=f->empty;
    f->ops[4]=(XrXirInstruction){.op=XR_XIR_PANIC_CATCH,.type=XR_XIR_PANIC_INFO};
    f->ops[5]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={1,0}};
}
static void live_cleanup(LiveFixture *f) {
    live_fixture(f,3);f->functions[0].block_count=2;
    f->blocks[0]=(XrXirBlock){.count=1};f->blocks[1]=(XrXirBlock){.first=1,.count=2};
    f->ops[0]=(XrXirInstruction){.op=XR_XIR_CLEANUP_LEAVE,.targets={1,0}};
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={1,0}};
}
static void live_invoke_result(LiveFixture *f) {
    live_branch(f,true);f->functions[0].instruction_count=7;f->functions[2].result=LIVE_SHARED;
    f->blocks[1].count=3;f->blocks[2].first=4;f->ops[0].type=LIVE_SHARED;
    f->ops[1]=(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=LIVE_SHARED};
    f->ops[2]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={5,0}};f->ops[3]=f->empty;
    f->ops[4]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR};
    f->ops[5]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={2,0}};f->ops[6]=f->empty;
}
static void live_prepare(LiveFixture *f,uint32_t kind) {
    switch (kind) {
    case 0:live_straight(f);break;case 1:live_reborrow(f);break;case 2:live_wrappers(f);break;
    case 3:live_diamond(f);break;case 4:live_branch(f,false);break;case 5:live_loop(f);break;
    case 6:live_panic(f);break;case 7:live_cleanup(f);break;case 8:live_branch(f,true);break;
    case 9:
        live_diamond(f);f->functions[0].block_count=5;f->functions[0].instruction_count=10;
        f->ops[8]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={2,0}};f->ops[9]=f->empty;break;
    case 12:live_invoke_result(f);break;
    default:CHECK(false);
    }
    f->functions[0].operands=f->functions[0].operand_count ? f->operands : NULL;
}
static void live_assert_case(uint32_t kind,const LiveObserved *o) {
    uint64_t one=live_bit(1),two=live_bit(2);CHECK(o->events && o->blocks[0]);
    if (!kind) {CHECK(o->entries[0]==one && o->after[0]==(one|live_bit(4)) && o->after[1]==one && !o->after[2]);}
    else if (kind==1) {CHECK(!o->entries[0] && o->after[2]==live_bit(6) && o->after[3]==(live_bit(6)|live_bit(7)) && o->after[4]==live_bit(6) && !o->after[5]);}
    else if (kind==2) {CHECK(o->entries[0]==(one|two) && o->after[0]==(one|two|live_bit(4)) && o->after[1]==(two|live_bit(4)|live_bit(5)) && o->after[3]==(two|live_bit(7)) && !o->after[5]);}
    else if (kind==3) {
        CHECK(o->entries[0]==one && !o->blocks[4]);
        CHECK(o->edge_after[1][3]==live_bit(5) && o->edge_after[2][3]==live_bit(7));
        CHECK(!o->entries[3] && o->after[5]==live_bit(9) && !o->after[6]);
    } else if (kind==4 || kind==8) {
        CHECK(o->entries[0]==(one|two) && o->edge_before[0][1]==(one|two));
        CHECK(o->edge_after[0][1]==one && o->edge_after[0][2]==two);
    } else if (kind==5) {
        CHECK(o->entries[0]==(one|two) && !o->entries[1]);
        CHECK(o->edge_after[0][1]==(one|two) && o->edge_after[1][1]==(live_bit(5)|live_bit(6)) && o->edge_after[1][2]==live_bit(6));
    } else if (kind==6) {CHECK(o->entries[0]==one && o->after[1]==(one|live_bit(5)) && o->after[2]==one && o->after[3]==one && o->panic[1][2] && o->edge_after[1][2]==one);}
    else if (kind==12) {
        CHECK(o->inventory==(one|two|live_bit(5)) && o->entries[0]==(one|two));
        CHECK(!o->edge_after[0][1] && o->edge_after[0][2]==two && o->after[1]==live_bit(5));
    } else {CHECK(kind==7 && o->edge_after[0][1]==one && !o->after[1]);}
}
static void live_large(LiveLarge *large,bool dag) {
    *large=(LiveLarge){0};LiveFixture *f=&large->fixture;live_fixture(f,1);
    uint32_t parameters=dag?1u:130u,count=dag?2u:131u;
    for (uint32_t p=0;p<parameters;++p) {
        large->parameters[p]=LIVE_SHARED;
        large->ops[p]=(XrXirInstruction){.op=XR_XIR_VIEW_LEN,.type=XR_XIR_I64,.args={p,0}};
    }
    if (dag) {
        large->nodes[0]=f->nodes[0];
        for (uint32_t i=1;i<70;++i) {
            large->fields[i-1u][0]=(XrXirCallableParameter){(XrXirType)(255u+i),XR_PARAM_READ};large->fields[i-1u][1]=large->fields[i-1u][0];
            large->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,.parameters=large->fields[i-1u],.parameter_count=2};
        }
        large->nodes[70]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)325};
        f->types=(XrXirTypes){large->nodes,71,NULL,NULL};large->parameters[0]=(XrXirType)326;
        large->ops[0]=(XrXirInstruction){.op=XR_XIR_NULLABLE_IS_SOME,.type=XR_XIR_BOOL,.args={0,0}};
    }
    large->ops[count-1u]=f->empty;large->block=(XrXirBlock){.count=count};
    f->functions[0].parameters=large->parameters;f->functions[0].parameter_count=parameters;
    f->functions[0].instructions=large->ops;f->functions[0].instruction_count=count;f->functions[0].blocks=&large->block;
}
static uint32_t live_population(const XrXirCompileContext *c,const XrXirViewLiveSet *set) {
    uint32_t count=0;
    for (uint32_t w=0;w<set->words;++w) {
        CHECK(xir_compile_work(c,1));uint64_t bits=set->bits[w];
        while (bits) {CHECK(xir_compile_work(c,1));++count;bits&=bits-1u;}
    }
    return count;
}
static XrXirStatus live_dense_observe(void *user,const XrXirCompileContext *c,const XrXirViewLiveEvent *e) {
    if (e->function) return XR_XIR_OK;
    uint32_t *seen=user;CHECK(e->before.count==130 && e->before.words==3);
    if (e->kind==XR_XIR_VIEW_LIVE_ENTRY) CHECK(live_population(c,&e->before)==130);
    else if (e->kind==XR_XIR_VIEW_LIVE_INSTRUCTION) {
        ++*seen;uint32_t expected=e->instruction<130?130u-e->instruction:0;
        CHECK(live_population(c,&e->before)==expected && live_population(c,&e->after)==(expected?expected-1u:0));
    } else CHECK(false);
    return XR_XIR_OK;
}
static XrXirStatus live_stop(void *user,const XrXirCompileContext *c,const XrXirViewLiveEvent *e) {
    (void)c;CHECK(e->kind==XR_XIR_VIEW_LIVE_ENTRY && !e->function);++*(uint32_t *)user;return XR_XIR_BAD_VALUE;
}
static void live_normal(void) {
    XrXirCompileContext c=owner_new(caps());uint64_t baseline=stats(&c).live_bytes;
    for (uint32_t index=0;index<11;++index) {
        uint32_t kind=index==10?12u:index;
        LiveFixture f;LiveObserved o;live_prepare(&f,kind);
        CHECK(xr_xir_compile_types_structure_verify(&c,&f.types)==XR_XIR_OK);
        XrXirStatus expected=kind==9?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
        CHECK(live_prove(&c,&f,&o)==expected && stats(&c).live_bytes==baseline);
        if (kind!=9) live_assert_case(kind,&o);else CHECK(!o.events);
        XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
        XrXirStatus status=xir_fixture_check(&c,&f.module,&checked,&diagnostic);
        expected=kind==9?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE;
        if (status!=expected) fprintf(stderr,"live case=%u common=%u at %u/%u/%u\n",kind,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
        CHECK(status==expected && !checked && stats(&c).live_bytes==baseline);
    }
    for (uint32_t dag=0;dag<2;++dag) {
        LiveLarge large;live_large(&large,dag!=0);const XrXirModule *m=&large.fixture.module;uint32_t seen=0;
        CHECK(xr_xir_compile_types_structure_verify(&c,m->types)==XR_XIR_OK);
        CHECK(xir_compile_view_liveness_verified(&c,m,dag?NULL:live_dense_observe,&seen,NULL)==XR_XIR_OK);
        CHECK(dag || seen==131);uint64_t work=stats(&c).work;
        CHECK(xir_compile_view_liveness_verified(&c,m,NULL,NULL,NULL)==XR_XIR_OK && stats(&c).work-work<10000);
        XrXirArtifact *checked=NULL;CHECK(xir_fixture_check(&c,m,&checked,NULL)==XR_XIR_BAD_TYPE && !checked);
    }
    LiveFixture f;live_straight(&f);uint32_t stopped=0;XrXirDiagnostic diagnostic={0};
    CHECK(xir_compile_view_liveness_verified(&c,&f.module,live_stop,&stopped,&diagnostic)==XR_XIR_BAD_VALUE && stopped==1);
    CHECK(diagnostic.function==0 && diagnostic.block==0 && diagnostic.instruction==UINT32_MAX);
    owner_free(&c,baseline);puts("borrowed demand: Shared/Mut/wrappers/PHI edges/loop swaps/panic entry/cleanup/invoke/unreachable PASS");
}
static XrXirStatus live_resource_prove(const XrXirCompileContext *c,const LiveFixture *f,uint32_t kind,LiveObserved *o) {
    return kind<10 || kind==12?live_prove(c,f,o):xir_compile_view_liveness_verified(c,&f->module,NULL,NULL,NULL);
}
static void live_resources(uint32_t kind) {
    LiveFixture f;LiveLarge large;LiveObserved o;const LiveFixture *fixture=&f;
    if (kind<10 || kind==12) live_prepare(&f,kind);else {live_large(&large,kind==11);fixture=&large.fixture;}
    XrXirStatus expected=kind==9?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;XrXirCompileContext c=owner_new(caps());
    uint64_t baseline=stats(&c).live_bytes;size_t start=attempts;CHECK(live_resource_prove(&c,fixture,kind,&o)==expected);
    size_t count=attempts-start;XrCompileResourceStats measured=stats(&c);CHECK(count>6 && count<32);
    CHECK(live_resource_prove(&c,fixture,kind,&o)==expected && stats(&c).live_bytes==baseline);owner_free(&c,baseline);
    for (size_t point=0;point<count;++point) {
        c=owner_new(caps());baseline=stats(&c).live_bytes;injected=false;fail_at=attempts+point;
        CHECK(live_resource_prove(&c,fixture,kind,&o)==XR_XIR_OUT_OF_MEMORY && injected);fail_at=SIZE_MAX;owner_free(&c,baseline);
    }
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t adjustment=0;adjustment<3;++adjustment) {
        XrCompileResourceLimits limits=caps();uint64_t *limit=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        *limit=(axis==0?measured.allocated_bytes:axis==1?measured.peak_bytes:measured.work)+adjustment-1u;
        c=owner_new(limits);baseline=stats(&c).live_bytes;
        CHECK(live_resource_prove(&c,fixture,kind,&o)==(adjustment?expected:XR_XIR_BUDGET));owner_free(&c,baseline);
    }
    c=owner_new(caps());baseline=stats(&c).live_bytes;c.limits.frame_bytes=1;
    CHECK(live_resource_prove(&c,fixture,kind,&o)==XR_XIR_BUDGET);owner_free(&c,baseline);
    printf("borrowed demand case=%u: %zu complete allocation failures, same owner2/three axes exact+minus1+plus1/physical0 PASS\n",kind,count);
}
int main(int argc,char **argv) {
    bool normal=argc==2 && !strcmp(argv[1],"--normal");CHECK(argc==1 || normal);live_normal();
    if (!normal) for (uint32_t kind=0;kind<13;++kind) live_resources(kind);
    puts("Borrowed SSA demand is not a root/path/alias or executable NLL certificate PASS");return 0;
}
