/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_snapshot_delta_mask.c - Complete physical delta mask read controls
 *
 * KEY CONCEPT:
 *   Storage-only rows grant no IR, type, effect or execution admission.
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_compile_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir/xxir_verify.c"
#include "xir/xxir_effects.c"
#include "xir_cell_class_view_data_cases.h"

typedef struct MaskRead {const XrXirCompileContext *context;uint64_t calls,reject;} MaskRead;
static bool mask_read_work(const void *owner,uint64_t units) {
    MaskRead *read=(MaskRead *)owner;
    return read->calls++!=read->reject && xir_compile_work(read->context,units);
}
static bool mask_same(const XrXirInstruction *a,const XrXirInstruction *b) {
    return a->op==b->op && a->type==b->type && a->args[0]==b->args[0] && a->args[1]==b->args[1] &&
        a->targets[0]==b->targets[0] && a->targets[1]==b->targets[1] && a->immediate==b->immediate &&
        a->type_arguments[0]==b->type_arguments[0] && a->type_arguments[1]==b->type_arguments[1];
}
static XrXirInstruction mask_expected(uint32_t mask) {
    return (XrXirInstruction){.op=mask&1?XR_XIR_CONST_INT+2:XR_XIR_CONST_INT,
        .type=mask&2?XR_XIR_I64+6:XR_XIR_I64,.args={mask&4?17:7,mask&8?UINT32_MAX-1:UINT32_MAX},
        .targets={mask&16?27:9,mask&32?42:10},
        .immediate=(int64_t)(((uint64_t)(mask&128?UINT32_C(0x80000004):UINT32_C(0x80000000))<<32)|
            (mask&64?UINT32_C(0xffffffff):UINT32_C(0xfffffffb))),
        .type_arguments={mask&256?51:11,mask&512?74:12}};
}
static XrXirInstruction mask_third(uint32_t mask) {
    return (XrXirInstruction){.op=mask&1?XR_XIR_CONST_INT+4:XR_XIR_CONST_INT,
        .type=mask&2?XR_XIR_I64+12:XR_XIR_I64,.args={mask&4?27:7,mask&8?UINT32_MAX-2:UINT32_MAX},
        .targets={mask&16?45:9,mask&32?74:10},
        .immediate=(int64_t)(((uint64_t)(mask&128?UINT32_C(0x80000008):UINT32_C(0x80000000))<<32)|
            (mask&64?UINT32_C(3):UINT32_C(0xfffffffb))),
        .type_arguments={mask&256?91:11,mask&512?136:12}};
}
typedef struct MaskVisit {
    const EffectBodyRun *runs;
    uint32_t count,calls,next,refuse;
    uint32_t observed[3];
} MaskVisit;
static XrXirStatus mask_visit_run(void *owner,const EffectBodyRun *run) {
    MaskVisit *visit=owner;
    CHECK(visit->calls<visit->count && run==&visit->runs[visit->calls] && run->first==visit->next);
    if (visit->calls<3)visit->observed[visit->calls]=run->base.args[0];
    visit->next+=run->count;++visit->calls;
    return visit->calls==visit->refuse?XR_XIR_IO:XR_XIR_OK;
}
static void mask_visit_walks(const XrXirCompileContext *context) {
    EffectBodyRun runs[3];memset(runs,0xa5,sizeof(runs));
    const uint32_t deltas[10]={2,6,10,UINT32_MAX,18,32,4,4,40,62};
    for(uint32_t r=0;r<3;++r) {
        runs[r].first=3*r;runs[r].count=3;runs[r].mask=1023;runs[r].base=mask_expected(0);
        for(uint32_t w=0;w<10;++w)runs[r].delta[w]=deltas[w];
    }
    EffectBodyFunction body={.instruction_count=9,.runs=runs,.run_count=3,.run_capacity=3};
    MaskVisit visit={.runs=runs,.count=3};EffectBodyRunVisitor visitor={&visit,mask_visit_run};
    MaskRead observation={context,0,UINT64_MAX};EffectBodyRead read={&observation,mask_read_work};
    CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_OK && visit.calls==3 && visit.next==9 &&
        visit.observed[0]==7 && visit.observed[1]==7 && visit.observed[2]==7);
    uint64_t points=observation.calls;
    for(uint64_t n=0;n<points;++n) {
        visit.calls=visit.next=0;observation.calls=0;observation.reject=n;
        CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_BUDGET && observation.calls==n+1);
        visit.calls=visit.next=0;observation.calls=0;observation.reject=UINT64_MAX;
        CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_OK && visit.calls==3 && visit.next==9);
    }
    for(unsigned r=0;r<3;++r)for(unsigned field=0;field<4;++field) {
        EffectBodyRun saved=runs[r];
        if(!field)++runs[r].first;
        else if(field==1)runs[r].count=0;
        else if(field==2)runs[r].mask|=1024;
        else runs[r].delta[9]=0;
        visit.calls=visit.next=0;
        CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_BAD_STRUCTURE && !visit.calls && !visit.next);
        runs[r]=saved;
    }
    --body.instruction_count;visit.calls=visit.next=0;
    CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_BAD_STRUCTURE && !visit.calls);
    ++body.instruction_count;--body.run_count;
    CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_BAD_STRUCTURE && !visit.calls);
    ++body.run_count;runs[2].base.args[0]=444;
    CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_OK && visit.calls==3 && visit.observed[2]==444);
    for(uint32_t n=1;n<=3;++n) {
        visit.calls=visit.next=0;visit.refuse=n;
        CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_IO && visit.calls==n);
    }
    visit.refuse=0;visit.calls=visit.next=0;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    uint64_t exact=after.work-before.work+1;
    for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits caps={8388608,4194304,exact-minus};XrXirCompileContext finite={0};
        CHECK(xr_compile_resources_new(&caps,&finite.resources)==XR_COMPILE_RESOURCE_OK);
        finite.limits=xr_xir_compile_default_limits();EffectBodyRead finite_read=effect_body_compile_read(&finite);
        visit.calls=visit.next=0;
        CHECK(effect_body_runs_visit(&finite_read,&body,&visitor)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        xr_compile_resources_release(finite.resources);
    }
    fprintf(stderr,"Complete physical visit: all canonical damage before callbacks, current changed row, every read-failure prefix/same-ledger retry, actual callback IO and exact%llu/minusone work PASS\n",(unsigned long long)exact);
}
static void mask_prefixes(const XrXirCompileContext *context,const EffectBodyFunction *body,
    uint32_t instruction,const XrXirInstruction *expected) {
    MaskRead observation={context,0,UINT64_MAX};EffectBodyRead read={&observation,mask_read_work};
    XrXirInstruction output={0};
    CHECK(effect_body_instruction_get(&read,body,instruction,&output)==XR_XIR_OK && mask_same(&output,expected));
    uint64_t points=observation.calls;CHECK(points);
    for(uint64_t n=0;n<points;++n) {
        observation.calls=0;observation.reject=n;
        const XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};
        output=sentinel;
        CHECK(effect_body_instruction_get(&read,body,instruction,&output)==XR_XIR_BUDGET &&
            mask_same(&output,&sentinel) && observation.calls==n+1);
    }
    observation.calls=0;observation.reject=UINT64_MAX;
    CHECK(effect_body_instruction_get(&read,body,instruction,&output)==XR_XIR_OK && mask_same(&output,expected));
}
static void mask_view_prefixes(const XrXirCompileContext *context,const EffectBodyRun *run,
    uint32_t instruction,const XrXirInstruction *expected) {
    MaskRead observation={context,0,UINT64_MAX};EffectBodyRead read={&observation,mask_read_work};
    const XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};
    XrXirInstruction storage=sentinel;const XrXirInstruction *output=&sentinel;
    CHECK(effect_body_instruction_ref_read(&read,run,instruction,&storage,&output)==XR_XIR_OK && mask_same(output,expected));
    uint64_t points=observation.calls;CHECK(points);
    for(uint64_t n=0;n<points;++n) {
        observation.calls=0;observation.reject=n;storage=sentinel;output=&sentinel;
        CHECK(effect_body_instruction_ref_read(&read,run,instruction,&storage,&output)==XR_XIR_BUDGET &&
            output==&sentinel && mask_same(&storage,&sentinel) && observation.calls==n+1);
    }
    observation.calls=0;observation.reject=UINT64_MAX;
    CHECK(effect_body_instruction_ref_read(&read,run,instruction,&storage,&output)==XR_XIR_OK && mask_same(output,expected));
}
static void mask_matrix(const XrXirCompileContext *context) {
    EffectBodyRead read=effect_body_compile_read(context);unsigned zeros=0;
    for(uint32_t mask=0;mask<1024;++mask) {
        EffectBodyRun run;memset(&run,0xa5,sizeof(run));
        run.first=0;run.count=3;run.mask=mask;run.base=mask_expected(0);
        const uint32_t deltas[10]={2,6,10,UINT32_MAX,18,32,4,4,40,62};
        for(uint32_t w=0;w<10;++w)if(mask&(1u<<w))run.delta[w]=deltas[w];
        EffectBodyFunction body={.instruction_count=3,.runs=&run,.run_count=1,.run_capacity=1};
        MaskVisit visit={.runs=&run,.count=1};EffectBodyRunVisitor visitor={&visit,mask_visit_run};
        CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_OK && visit.calls==1 && visit.next==3);
        const EffectBodyRun *ref=NULL;
        CHECK(effect_body_record_ref(&read,&body,1,&ref)==XR_XIR_OK && ref==&run);
        XrXirInstruction expected=mask_expected(mask),output={0};
        CHECK(effect_body_instruction_get(&read,&body,1,&output)==XR_XIR_OK && mask_same(&output,&expected));
        XrXirInstruction third=mask_third(mask);
        CHECK(effect_body_instruction_get(&read,&body,2,&output)==XR_XIR_OK && mask_same(&output,&third));
        const XrXirInstruction held_view={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};
        XrXirInstruction view_storage=held_view;const XrXirInstruction *view=&held_view;
        CHECK(effect_body_instruction_ref_read(&read,&run,0,&view_storage,&view)==XR_XIR_OK &&
            view==&run.base && mask_same(view,&run.base) && mask_same(&view_storage,&held_view));
        CHECK(effect_body_instruction_ref_read(&read,&run,1,&view_storage,&view)==XR_XIR_OK &&
            view==(mask?&view_storage:&run.base) && mask_same(view,&expected));
        if(mask==0 || mask==1 || mask==512 || mask==513 || mask==1023) {
            mask_prefixes(context,&body,1,&expected);
            mask_view_prefixes(context,&run,0,&run.base);mask_view_prefixes(context,&run,1,&expected);
        }
        for(uint32_t w=0;w<10;++w)if(mask&(1u<<w)) {
            uint32_t delta=run.delta[w];run.delta[w]=0;
            const EffectBodyRun *sentinel=(const EffectBodyRun *)(const void *)&body;ref=sentinel;
            CHECK(effect_body_record_ref(&read,&body,1,&ref)==XR_XIR_BAD_STRUCTURE && ref==sentinel);
            XrXirInstruction held={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};output=held;
            CHECK(effect_body_instruction_get(&read,&body,1,&output)==XR_XIR_BAD_STRUCTURE && mask_same(&output,&held));
            CHECK(effect_body_instruction_ref_prepare(&read,&run,1)==XR_XIR_BAD_STRUCTURE);
            visit.calls=visit.next=0;
            CHECK(effect_body_runs_visit(&read,&body,&visitor)==XR_XIR_BAD_STRUCTURE && !visit.calls);
            view_storage=held_view;view=&held_view;
            CHECK(effect_body_instruction_ref_read(&read,&run,0,&view_storage,&view)==XR_XIR_BAD_STRUCTURE &&
                view==&held_view && mask_same(&view_storage,&held_view));
            run.delta[w]=delta;++zeros;
        }
        run.mask|=1024;ref=NULL;
        CHECK(effect_body_record_ref(&read,&body,1,&ref)==XR_XIR_BAD_STRUCTURE && !ref);
        run.mask=mask;run.count=1;
        CHECK(effect_body_record_ref(&read,&body,1,&ref)==XR_XIR_BAD_STRUCTURE && !ref);
        run.count=2;run.first=1;
        CHECK(effect_body_record_ref(&read,&body,1,&ref)==XR_XIR_BAD_STRUCTURE && !ref);
        run.first=0;run.count=4;
        CHECK(effect_body_record_ref(&read,&body,1,&ref)==XR_XIR_BAD_STRUCTURE && !ref);
    }
    CHECK(zeros==5120);
    fprintf(stderr,"Snapshot physical masks1024 and selected zero deltas%u; actual first/constant views, full-word oracle and all read-failure prefixes/same-ledger retries\n",zeros);
}
static void mask_boundaries(const XrXirCompileContext *context) {
    const uint32_t masks[5]={0,1,512,513,1023};EffectBodyRead read=effect_body_compile_read(context);
    for(unsigned m=0;m<5;++m) {
        EffectBodyRun runs[3];memset(runs,0xa5,sizeof(runs));
        const uint32_t deltas[10]={2,6,10,UINT32_MAX,18,32,4,4,40,62};
        for(unsigned r=0;r<3;++r) {
            runs[r].first=3*r;runs[r].count=3;runs[r].mask=masks[m];runs[r].base=mask_expected(0);
            for(unsigned w=0;w<10;++w)if(masks[m]&(1u<<w))runs[r].delta[w]=deltas[w];
        }
        EffectBodyFunction body={.instruction_count=9,.runs=runs,.run_count=3,.run_capacity=3};
        for(uint32_t i=0;i<9;++i) {
            XrXirInstruction expected=i%3==2?mask_third(masks[m]):mask_expected(i%3?masks[m]:0),output={0};
            CHECK(effect_body_instruction_get(&read,&body,i,&output)==XR_XIR_OK && mask_same(&output,&expected));
            if(i%3)mask_prefixes(context,&body,i,&expected);
        }
        for(unsigned r=0;r<3;++r) {
            ++runs[r].first;
            for(unsigned i=0;i<9;++i){const EffectBodyRun *ref=NULL;CHECK(effect_body_record_ref(&read,&body,i,&ref)==XR_XIR_BAD_STRUCTURE && !ref);}
            --runs[r].first;
            for(unsigned w=0;w<10;++w)if(masks[m]&(1u<<w)) {
                runs[r].delta[w]=0;
                for(unsigned i=0;i<9;++i){const EffectBodyRun *ref=NULL;CHECK(effect_body_record_ref(&read,&body,i,&ref)==XR_XIR_BAD_STRUCTURE && !ref);}
                runs[r].delta[w]=deltas[w];
            }
        }
    }
    fprintf(stderr,"Snapshot first/middle/last selected and adjacent boundaries, selected delta damage and all read prefixes PASS\n");
}
static void mask_work_bounds(const XrXirCompileContext *context) {
    EffectBodyRun run={.first=0,.count=3,.mask=1023,.base=mask_expected(0),
        .delta={2,6,10,UINT32_MAX,18,32,4,4,40,62}};
    EffectBodyFunction body={.instruction_count=3,.runs=&run,.run_count=1,.run_capacity=1};
    EffectBodyRead read=effect_body_compile_read(context);XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirInstruction expected=mask_expected(1023),output={0};
    CHECK(effect_body_instruction_get(&read,&body,1,&output)==XR_XIR_OK && mask_same(&output,&expected));
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    /* A newly created finite ledger already owns one work unit. */
    uint64_t exact=after.work-before.work+1;CHECK(exact>1);
    for(unsigned n=0;n<2;++n) {
        XrCompileResourceLimits caps={8388608,4194304,exact-(n==0)};XrXirCompileContext finite={0};
        CHECK(xr_compile_resources_new(&caps,&finite.resources)==XR_COMPILE_RESOURCE_OK);
        finite.limits=xr_xir_compile_default_limits();read=effect_body_compile_read(&finite);
        XrXirInstruction held={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};output=held;
        XrXirStatus status=effect_body_instruction_get(&read,&body,1,&output);
        CHECK(status==(n?XR_XIR_OK:XR_XIR_BUDGET) && mask_same(&output,n?&expected:&held));
        xr_compile_resources_release(finite.resources);
    }
    fprintf(stderr,"Snapshot current physical read work exact%llu/minusone preserves output atomicity\n",(unsigned long long)exact);
}
static void mask_cell_values(const XrXirCompileContext *context) {
    unsigned zeroes=0;
    for(uint32_t mask=0;mask<1024;++mask) {
        XrXirCellProofRun run;memset(&run,0xa5,sizeof(run));
        const XrXirInstruction base=mask_expected(0);
        run.first=7;run.count=3;run.varying=mask;run.operation=&base;run.second=NULL;
        const uint32_t delta[10]={2,6,10,UINT32_MAX,18,32,4,4,40,62};
        for(unsigned w=0;w<10;++w)if(mask&(1u<<w))run.delta[w]=delta[w];
        CellProofBuild build={.context=context,.status=XR_XIR_OK};
        const XrXirInstruction first_held={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};
        XrXirInstruction storage=first_held;
        const XrXirInstruction *first=cell_proof_run_operation(&build,0,&run,7,&storage);
        CHECK(first==run.operation && mask_same(first,run.operation) && mask_same(&storage,&first_held));
        const XrXirInstruction *second=cell_proof_run_operation(&build,0,&run,8,&storage);
        XrXirInstruction expected=mask_expected(mask);
        CHECK(second==&storage && mask_same(second,&expected));
        run.operation=NULL;storage=first_held;
        CHECK(!cell_proof_run_operation(&build,0,&run,8,&storage) && build.status==XR_XIR_BAD_STRUCTURE &&
            build.location.function==0 && build.location.instruction==8 && mask_same(&storage,&first_held));
        run.operation=&base;build.status=XR_XIR_OK;
        for(unsigned w=0;w<10;++w)if(mask&(1u<<w)) {
            run.delta[w]=0;build.status=XR_XIR_OK;
            const XrXirInstruction held={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};storage=held;
            CHECK(!cell_proof_run_operation(&build,0,&run,8,&storage) && build.status==XR_XIR_BAD_STRUCTURE &&
                build.location.function==0 && build.location.instruction==8 && mask_same(&storage,&held));
            run.delta[w]=delta[w];++zeroes;
        }
    }
    CHECK(zeroes==5120);
    fprintf(stderr,"Cell local physical range: all1024 masks/5120 selected zero deltas, poisoned inactive words and exact full instruction oracle PASS\n");
}
#include "xir_snapshot_literal_pair_cases.h"
int main(void) {
    const XrCompileResourceLimits caps={8388608,4194304,4000000};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    mask_matrix(&context);mask_boundaries(&context);mask_work_bounds(&context);mask_cell_values(&context);mask_visit_walks(&context);
    class_view_data_controls(&context);
    literal_pair_controls();
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK &&
        after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes);
    fprintf(stderr,"Snapshot no-allocation read matrix work%llu cap4000000 live%llu/%llu\n",
        (unsigned long long)after.work,(unsigned long long)before.live_bytes,(unsigned long long)after.live_bytes);
    xr_compile_resources_release(context.resources);return 0;
}
