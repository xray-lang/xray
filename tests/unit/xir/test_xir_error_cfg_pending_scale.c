/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_cfg_pending_scale.c - Complete inference scale measurements
 *
 * KEY CONCEPT:
 *   Size axes change real typed rows while admission and literal facts remain.
 *   Measurements report one owner without replacing qualification budgets.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_effects.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1); } } while (0)

typedef struct PmPhysical { void *pointer; size_t bytes; } PmPhysical;
static PmPhysical pm_physical[2048];
static size_t pm_live, pm_bytes, pm_attempts;
static void *pm_malloc(size_t bytes) {
    ++pm_attempts; void *pointer=xr_malloc(bytes); if(!pointer)return NULL;
    CHECK(pm_live<2048 && bytes<=SIZE_MAX-pm_bytes);
    pm_physical[pm_live++]=(PmPhysical){pointer,bytes}; pm_bytes+=bytes; return pointer;
}
static void pm_free(void *pointer) {
    if(!pointer)return;
    size_t i=0; while(i<pm_live && pm_physical[i].pointer!=pointer)++i;
    CHECK(i<pm_live && pm_bytes>=pm_physical[i].bytes);
    pm_bytes-=pm_physical[i].bytes; pm_physical[i]=pm_physical[--pm_live]; xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) pm_malloc(bytes)
#define xr_free(pointer) pm_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#include "xir/xxir_effects.c"
#include "xir_error_cfg_pending_fixture.h"

typedef struct PmCase {
    uint32_t scalars, blocks, reverse;
    bool empty;
    uint32_t subject_instructions, module_instructions, functions, types, module_blocks;
    size_t fixture_bytes, scratch_bytes;
} PmCase;
enum { PM_SCALAR_MAX=1024, PM_INSTRUCTION_MAX=PM_SCALAR_MAX+EE_BLOCKS+1 };
typedef struct PmFixture {
    EdgeFixture base;
    XrXirInstruction subject[PM_INSTRUCTION_MAX], saved[PM_INSTRUCTION_MAX];
    uint32_t indices[PM_INSTRUCTION_MAX];
    XrXirBlock saved_blocks[EE_BLOCKS];
} PmFixture;
typedef struct PmPoint {
    XrCompileResourceStats stats;
    size_t attempts, physical_blocks, physical_bytes;
} PmPoint;
static const XrCompileResourceLimits pm_caps={8388608,4194304,8000000};
static PmPoint pm_point(const XrXirCompileContext *context) {
    PmPoint point={0};
    if(context->resources)
        CHECK(xr_compile_resources_stats(context->resources,&point.stats)==XR_COMPILE_RESOURCE_OK);
    point.attempts=pm_attempts; point.physical_blocks=pm_live; point.physical_bytes=pm_bytes;
    return point;
}
static void pm_identity(const PmCase *input) {
    printf("\"scalars\":%u,\"subject_blocks\":%u,\"subject_instructions\":%u,"
        "\"module_instructions\":%u,\"module_blocks\":%u,\"functions\":%u,\"types\":%u,"
        "\"fixture_bytes\":%zu,\"scratch_bytes\":%zu,\"reverse\":%u,\"empty\":%s",
        input->scalars,input->blocks,input->subject_instructions,
        input->module_instructions,input->module_blocks,input->functions,input->types,
        input->fixture_bytes,input->scratch_bytes,input->reverse,input->empty?"true":"false");
}
static void pm_phase(const PmCase *input,const char *name,const PmPoint *before,const PmPoint *after) {
    CHECK(after->stats.work>=before->stats.work && after->stats.allocated_bytes>=before->stats.allocated_bytes);
    CHECK(after->stats.allocation_count>=before->stats.allocation_count && after->attempts>=before->attempts);
    printf("{\"schema\":\"task324.pending.scale.v2\",\"event\":\"phase\",\"phase\":\"%s\",",name);
    pm_identity(input);
    printf(",\"status\":0,\"work_before\":%llu,\"work_after\":%llu,\"work_delta\":%llu,"
        "\"allocated_before\":%llu,\"allocated_after\":%llu,\"allocated_delta\":%llu,"
        "\"allocation_count_delta\":%llu,\"attempts_before\":%zu,\"attempts_after\":%zu,\"attempts_delta\":%zu,"
        "\"live_before\":%llu,\"live_after\":%llu,\"peak_before\":%llu,\"peak_after\":%llu,"
        "\"physical_blocks_before\":%zu,\"physical_blocks_after\":%zu,\"physical_bytes_before\":%zu,"
        "\"physical_bytes_after\":%zu}\n",
        (unsigned long long)before->stats.work,(unsigned long long)after->stats.work,
        (unsigned long long)(after->stats.work-before->stats.work),
        (unsigned long long)before->stats.allocated_bytes,(unsigned long long)after->stats.allocated_bytes,
        (unsigned long long)(after->stats.allocated_bytes-before->stats.allocated_bytes),
        (unsigned long long)(after->stats.allocation_count-before->stats.allocation_count),
        before->attempts,after->attempts,after->attempts-before->attempts,
        (unsigned long long)before->stats.live_bytes,(unsigned long long)after->stats.live_bytes,
        (unsigned long long)before->stats.peak_bytes,(unsigned long long)after->stats.peak_bytes,
        before->physical_blocks,after->physical_blocks,before->physical_bytes,after->physical_bytes);
}
static void pm_reverse(PmFixture *storage) {
    EdgeFixture *fixture=&storage->base;
    XrXirFunction *function=&fixture->functions[1];
    uint32_t blocks=function->block_count, instructions=function->instruction_count, at=0;
    CHECK(function->instructions==storage->subject && !function->parameter_count);
    CHECK(blocks<=EE_BLOCKS && instructions<=PM_INSTRUCTION_MAX);
    memcpy(storage->saved_blocks,fixture->blocks,(size_t)blocks*sizeof(*fixture->blocks));
    memcpy(storage->saved,storage->subject,(size_t)instructions*sizeof(*storage->subject));
    for(uint32_t b=0;b<blocks;++b) {
        uint32_t original=b?blocks-b:0;
        fixture->blocks[b]=storage->saved_blocks[original]; fixture->blocks[b].first=at;
        const XrXirBlock *source=&storage->saved_blocks[original];
        CHECK(source->first<=instructions && source->count<=instructions-source->first);
        for(uint32_t i=source->first;i<source->first+source->count;++i) {
            CHECK(at<instructions); storage->indices[i]=at;
            storage->subject[at++]=storage->saved[i];
        }
    }
    CHECK(at==instructions);
    for(uint32_t b=0;b<blocks;++b) {
        if(fixture->blocks[b].panic) {
            CHECK(fixture->blocks[b].panic<blocks); fixture->blocks[b].panic=blocks-fixture->blocks[b].panic;
        }
        if(fixture->blocks[b].frontier) {
            CHECK(fixture->blocks[b].frontier<=instructions);
            fixture->blocks[b].frontier=storage->indices[fixture->blocks[b].frontier-1]+1;
        }
    }
    for(uint32_t i=0;i<instructions;++i) {
        XrXirInstruction *op=&storage->subject[i];
        if(op->op==XR_XIR_JUMP) {
            CHECK(op->targets[0]<blocks);
            if(op->targets[0])op->targets[0]=blocks-op->targets[0];
        } else if(op->op==XR_XIR_THROW) {
            CHECK(op->args[0]<instructions); op->args[0]=storage->indices[op->args[0]];
        } else CHECK(op->op==XR_XIR_CONST_INT || op->op==XR_XIR_ENUM_NEW || op->op==XR_XIR_RETURN);
    }
}
static void pm_fixture(PmFixture *storage,PmCase *input) {
    CHECK(input->scalars && input->scalars<=PM_SCALAR_MAX && input->blocks>=2 && input->blocks<=EE_BLOCKS);
    CHECK(input->scalars<=PM_INSTRUCTION_MAX-input->blocks-1);
    EdgeFixture *fixture=&storage->base; pc_fixture(fixture,PC_FORWARD,false);
    for(uint32_t i=0;i<input->scalars;++i)
        storage->subject[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=i};
    uint32_t at=input->scalars;
    storage->subject[at++]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM};
    storage->subject[at++]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    fixture->blocks[0]=(XrXirBlock){.count=at};
    for(uint32_t b=1;b<input->blocks-1;++b) {
        fixture->blocks[b]=(XrXirBlock){.first=at,.count=1};
        storage->subject[at++]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={b+1}};
    }
    fixture->blocks[input->blocks-1]=(XrXirBlock){.first=at,.count=1};
    storage->subject[at++]=input->empty?(XrXirInstruction){.op=XR_XIR_RETURN}:
        (XrXirInstruction){.op=XR_XIR_THROW,.args={input->scalars}};
    XrXirFunction *function=&fixture->functions[1];
    function->parameters=NULL; function->parameter_count=0;
    function->blocks=fixture->blocks; function->block_count=input->blocks;
    function->instructions=storage->subject; function->instruction_count=at;
    function->operands=NULL; function->operand_count=0;
    if(input->reverse)pm_reverse(storage);
    fixture->module.stage=XR_XIR_BUILT;
    CHECK(at==input->scalars+input->blocks+1 && fixture->module.function_count==3 && fixture->types.count==2);
    input->subject_instructions=function->instruction_count; input->module_instructions=0; input->module_blocks=0;
    input->functions=fixture->module.function_count; input->types=fixture->types.count;
    for(uint32_t f=0;f<input->functions;++f) {
        input->module_instructions+=fixture->functions[f].instruction_count;
        input->module_blocks+=fixture->functions[f].block_count;
    }
    input->fixture_bytes=sizeof(*storage);
    input->scratch_bytes=sizeof(storage->saved)+sizeof(storage->indices)+sizeof(storage->saved_blocks);
    CHECK(input->module_instructions==input->scalars+input->blocks+4 && input->module_blocks==input->blocks+2);
}
static void pm_golden(const XrXirEffects *effects,const PmCase *input) {
    CHECK(effects && effects->count==3 && effect_final_outcomes(effects)->atom_count==2 && effect_final_outcomes(effects)->words==1);
    CHECK(!effect_final_outcomes(effects)->errors[0] && effect_final_outcomes(effects)->errors[1]==(input->empty?0u:pc_expected(PC_FORWARD,false)));
    CHECK(effect_final_outcomes(effects)->errors[2]==8);
    CHECK(effect_final_outcomes(effects)->functions[1].throws==(input->empty?XR_XIR_EFFECT_NONE:XR_XIR_EFFECT_MAY));
    CHECK(!xr_xir_effects_error_unknown(effects,1) && !xr_xir_effects_error_unidentified(effects,1));
    CHECK(xr_xir_effects_error(effects,1,(XrXirType)EE_ENUM,0)==!input->empty);
    CHECK(!xr_xir_effects_error(effects,1,(XrXirType)EE_ENUM,1));
    for(uint32_t f=0;f<3;++f) {
        CHECK(effect_final_outcomes(effects)->task_errors[f]==XR_XIR_OK && effect_final_outcomes(effects)->functions[f].suspend==XR_XIR_EFFECT_NONE);
        CHECK(effects->root[f].requires_root==(f==0) && !effects->root[f].unresolved);
    }
}
static void pm_summary(const PmCase *input,const PmPoint *before,const PmPoint *after,uint64_t infer_work) {
    bool original=input->empty && input->scalars==EE_SCALARS && input->blocks==EE_BLOCKS;
    printf("{\"schema\":\"task324.pending.scale.v2\",\"event\":\"summary\","); pm_identity(input);
    printf(",\"status\":0,\"inclusive_work\":%llu,\"inclusive_allocated\":%llu,\"inclusive_attempts\":%zu,"
        "\"inclusive_allocation_count\":%llu,\"final_live\":%llu,\"peak\":%llu,\"infer_work\":%llu,"
        "\"original_20k_applies\":%s,\"original_20k_pass\":%s,\"qualification\":\"MEASUREMENT_ONLY\"}\n",
        (unsigned long long)(after->stats.work-before->stats.work),
        (unsigned long long)(after->stats.allocated_bytes-before->stats.allocated_bytes),after->attempts-before->attempts,
        (unsigned long long)(after->stats.allocation_count-before->stats.allocation_count),
        (unsigned long long)after->stats.live_bytes,(unsigned long long)after->stats.peak_bytes,
        (unsigned long long)infer_work,original?"true":"false",original?(infer_work<UINT64_C(20000)?"true":"false"):"null");
}
static void pm_run(const PmCase *input) {
    CHECK(!pm_live && !pm_bytes); XrXirCompileContext context={0};
    PmCase current_case=*input; input=&current_case;
    PmPoint initial=pm_point(&context); PmFixture fixture; pm_fixture(&fixture,&current_case);
    PmPoint point=pm_point(&context); pm_phase(input,"fixture",&initial,&point);
    CHECK(xr_compile_resources_new(&pm_caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    PmPoint owner=pm_point(&context); pm_phase(input,"owner",&point,&owner);
    XrXirArtifact *checked=NULL; XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.base.module,&checked,NULL)==XR_XIR_OK);
    PmPoint admitted=pm_point(&context); pm_phase(input,"check",&owner,&admitted);
    const XrXirModule *current=xr_xir_compile_artifact_module(checked);
    CHECK(current && current->stage==XR_XIR_CHECKED);
    CHECK(xr_xir_compile_verify_v2(&context,current,xr_xir_compile_artifact_construction(checked),NULL)==XR_XIR_OK);
    PmPoint verified=pm_point(&context); pm_phase(input,"verify",&admitted,&verified);
    CHECK(xr_xir_compile_effects_infer_verified(&context,current,&effects)==XR_XIR_OK);
    pm_golden(effects,input);
    PmPoint inferred=pm_point(&context); pm_phase(input,"infer",&verified,&inferred);
    xr_xir_compile_effects_free(effects); xr_xir_compile_artifact_free(checked);
    PmPoint final=pm_point(&context); pm_phase(input,"cleanup",&inferred,&final);
    CHECK(final.stats.live_bytes==owner.stats.live_bytes);
    pm_summary(input,&initial,&final,inferred.stats.work-verified.stats.work);
    xr_compile_resources_release(context.resources); context.resources=NULL;
    CHECK(!pm_live && !pm_bytes);
    printf("{\"schema\":\"task324.pending.scale.v2\",\"event\":\"owner_drop\","); pm_identity(input);
    printf(",\"physical_blocks\":%zu,\"physical_bytes\":%zu,\"ledger_released\":true}\n",pm_live,pm_bytes);
}
static bool pm_parse(int argc,char **argv,PmCase *input) {
    if(argc!=6 || strcmp(argv[1],"--case"))return false;
    input->scalars=!strcmp(argv[2],"128")?128u:!strcmp(argv[2],"256")?256u:
        !strcmp(argv[2],"512")?512u:!strcmp(argv[2],"1024")?1024u:0u;
    input->blocks=!strcmp(argv[3],"8")?8u:!strcmp(argv[3],"16")?16u:!strcmp(argv[3],"32")?32u:0u;
    if(!strcmp(argv[4],"forward"))input->reverse=0;
    else if(!strcmp(argv[4],"reverse"))input->reverse=1;
    else return false;
    if(!strcmp(argv[5],"empty"))input->empty=true;
    else if(!strcmp(argv[5],"nonempty"))input->empty=false;
    else return false;
    return input->scalars && input->blocks && input->scalars<=PM_INSTRUCTION_MAX-input->blocks-1 &&
        (input->blocks==32 || input->scalars==512);
}
int main(int argc,char **argv) {
    if(argc!=1) {
        PmCase input={0}; if(!pm_parse(argc,argv,&input))return 2;
        pm_run(&input); return 0;
    }
    static const uint32_t scales[6][2]={{128,32},{256,32},{512,32},{512,8},{512,16},{1024,32}};
    for(unsigned empty=0;empty<2;++empty)for(unsigned reverse=0;reverse<2;++reverse)
        for(unsigned i=0;i<6;++i) {
            PmCase input={.scalars=scales[i][0],.blocks=scales[i][1],.reverse=reverse,.empty=empty!=0}; pm_run(&input);
        }
    return 0;
}
