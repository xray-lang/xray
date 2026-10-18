/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_budget.c - Original-owner boundaries for foreign constant snapshots
 */
#define XR_IMPORT_MAIN import_original_main
#include "test_import.c"

typedef enum ImportBudgetAxis {
    IMPORT_WORK,IMPORT_ALLOCATED,IMPORT_LIVE,IMPORT_STEPS,IMPORT_FRAME,IMPORT_OUTPUT,IMPORT_DEPTH
} ImportBudgetAxis;
typedef struct ImportBudgetCost { uint64_t work,allocated; } ImportBudgetCost;
typedef struct ImportBudgetProbe {
    const char *text;
    const XrXirValue *input;
    bool leaf;
    ImportBudgetCost cost;
} ImportBudgetProbe;

/* Each trial has one finite receiver. Failed execution and its retry always
 * retain that receiver; independent boundary probes start independent jobs. */
static void import_budget_close(XrXirCompileContext *receiver,XrXirArtifact *target) {
    xr_xir_compile_artifact_free(target);xr_compile_resources_release(receiver->resources);
}
static uint64_t import_allocation_overhead(XrXirCompileContext *receiver) {
    XrCompileResourceStats before=stats(receiver);void *block=NULL;
    CHECK(xr_compile_resources_alloc(receiver->resources,1,&block)==XR_COMPILE_RESOURCE_OK);
    uint64_t charged=stats(receiver).allocated_bytes-before.allocated_bytes;
    xr_compile_resources_free(block);CHECK(charged>1);return charged-1;
}
static void import_consume_allocated(XrXirCompileContext *receiver,uint64_t bytes,uint64_t overhead) {
    while (bytes) {
        uint64_t charged=bytes>1048576?1048576:bytes;
        if (bytes>charged && bytes-charged<=overhead) charged-=overhead+1;
        CHECK(charged>overhead);void *block=NULL;
        CHECK(xr_compile_resources_alloc(receiver->resources,(size_t)(charged-overhead),&block)==XR_COMPILE_RESOURCE_OK);
        xr_compile_resources_free(block);bytes-=charged;
    }
}
static ImportBudgetCost import_budget_cost(const char *text,const XrXirValue *input,bool leaf) {
    XrXirCompileContext receiver;context_new(&receiver);XrXirArtifact *target=import_lower(&receiver,text);
    XrXirCtfeCallRequest request=import_request(target,"pass",input);XrXirValue output={0};
    XrCompileResourceStats before=stats(&receiver);
    CHECK(import_evaluate(&request,leaf,&output)==XR_XIR_CTFE_OK);
    XrCompileResourceStats after=stats(&receiver);xr_xir_value_drop(&output);
    CHECK(stats(&receiver).live_bytes==before.live_bytes);
    import_budget_close(&receiver,target);
    return (ImportBudgetCost){after.work-before.work,after.allocated_bytes-before.allocated_bytes};
}
static XrXirCtfeStatus import_budget_expected(ImportBudgetAxis axis) {
    return axis<=IMPORT_LIVE?XR_XIR_CTFE_BUDGET:axis==IMPORT_STEPS?XR_XIR_CTFE_STEP_LIMIT:
        axis==IMPORT_FRAME?XR_XIR_CTFE_FRAME_LIMIT:axis==IMPORT_OUTPUT?XR_XIR_CTFE_OUTPUT_LIMIT:XR_XIR_CTFE_DEPTH_LIMIT;
}
static bool import_budget_trial(const ImportBudgetProbe *probe,ImportBudgetAxis axis,uint64_t available,bool retry) {
    const char *text=probe->text;const XrXirValue *input=probe->input;bool leaf=probe->leaf;ImportBudgetCost cost=probe->cost;
    XrXirCompileContext receiver;context_new(&receiver);XrXirArtifact *target=import_lower(&receiver,text);
    XrXirCtfeCallRequest request=import_request(target,"pass",input);XrXirValue output={0};void *hold=NULL;
    XrCompileResourceStats initial=stats(&receiver);uint64_t overhead=0;
    if (axis==IMPORT_ALLOCATED || axis==IMPORT_LIVE) overhead=import_allocation_overhead(&receiver);
    XrCompileResourceStats prepared=stats(&receiver);
    if (axis==IMPORT_WORK) {
        CHECK(available<=caps.work-prepared.work);
        CHECK(xr_compile_resources_work(receiver.resources,caps.work-prepared.work-available)==XR_COMPILE_RESOURCE_OK);
    } else if (axis==IMPORT_ALLOCATED) {
        CHECK(available<=caps.allocated_bytes-prepared.allocated_bytes);
        import_consume_allocated(&receiver,caps.allocated_bytes-prepared.allocated_bytes-available,overhead);
    } else if (axis==IMPORT_LIVE) {
        CHECK(available<=caps.live_bytes-prepared.live_bytes);
        uint64_t charged=caps.live_bytes-prepared.live_bytes-available;
        CHECK(!charged || charged>overhead);
        if (charged) CHECK(xr_compile_resources_alloc(receiver.resources,(size_t)(charged-overhead),&hold)==XR_COMPILE_RESOURCE_OK);
    } else if (axis==IMPORT_STEPS) request.evaluation.limits.steps=available;
    else if (axis==IMPORT_FRAME) request.evaluation.limits.frame_bytes=available;
    else if (axis==IMPORT_OUTPUT) request.evaluation.limits.output_bytes=available;
    else request.call_depth=(uint32_t)available;
    XrCompileResourceStats before=stats(&receiver);XrXirCtfeStatus status=import_evaluate(&request,leaf,&output);
    bool success=status==XR_XIR_CTFE_OK;
    if (success) {
        CHECK(xr_xir_value_valid(&output));xr_xir_value_drop(&output);
        if (axis==IMPORT_WORK) CHECK(stats(&receiver).work-before.work==cost.work);
        if (axis==IMPORT_ALLOCATED) CHECK(stats(&receiver).allocated_bytes-before.allocated_bytes==cost.allocated);
    } else {
        /* Common Value receiving reports scratch exhaustion as LIMIT, whose
         * existing CTFE mapping is BUDGET. Prove this is not ledger exhaustion. */
        bool value_scratch=axis==IMPORT_FRAME && status==XR_XIR_CTFE_BUDGET;
        if (value_scratch) {
            XrCompileResourceStats failed=stats(&receiver);
            CHECK(cost.work<=caps.work-failed.work && cost.allocated<=caps.allocated_bytes-failed.allocated_bytes);
        }
        if (status!=import_budget_expected(axis) && !value_scratch) fprintf(stderr,"Foreign unexpected bound leaf=%u axis=%u available=%llu status=%u expected=%u\n",
            leaf,(unsigned)axis,(unsigned long long)available,status,import_budget_expected(axis));
        CHECK((status==import_budget_expected(axis) || value_scratch) && !output.type && !output.reserved && !output.payload);
    }
    CHECK(stats(&receiver).live_bytes==before.live_bytes);
    if (retry && !success) {
        CHECK(import_evaluate(&request,leaf,&output)==status);
        CHECK(!output.type && !output.reserved && !output.payload && stats(&receiver).live_bytes==before.live_bytes);
    }
    xr_compile_resources_free(hold);
    if (retry && axis>=IMPORT_LIVE) {
        request=import_request(target,"pass",input);
        CHECK(import_evaluate(&request,leaf,&output)==XR_XIR_CTFE_OK);xr_xir_value_drop(&output);
    } else if (retry) {
        CHECK(import_evaluate(&request,leaf,&output)==XR_XIR_CTFE_BUDGET);
        CHECK(!output.type && !output.reserved && !output.payload);
    }
    CHECK(stats(&receiver).live_bytes==initial.live_bytes);
    import_budget_close(&receiver,target);return success;
}
static void import_budget(const char *kind) {
    const char *text=import_text(kind);XrXirCompileContext producer;context_new(&producer);
    XrXirArtifact *origin=import_lower(&producer,text);XrXirCtfeCallRequest make=import_request(origin,"produce",NULL);
    XrXirValue input={0};CHECK(xr_xir_compile_ctfe_calls(&make,&input)==XR_XIR_CTFE_OK);
    XrXirValue unchanged=input;XrCompileResourceStats source=stats(&producer);
    for (unsigned mode=0;mode<2;++mode) {
        bool leaf=mode!=0;ImportBudgetCost cost=import_budget_cost(text,&input,leaf);
        ImportBudgetProbe probe={text,&input,leaf,cost};
        CHECK(cost.work && cost.allocated);
        for (ImportBudgetAxis axis=IMPORT_WORK;axis<=IMPORT_DEPTH;axis=(ImportBudgetAxis)(axis+1)) {
            if (leaf && axis==IMPORT_DEPTH) continue;
            uint64_t exact=axis==IMPORT_WORK?cost.work:axis==IMPORT_ALLOCATED?cost.allocated:0;
            if (axis>=IMPORT_LIVE) {
                uint64_t low=0,high=axis==IMPORT_LIVE?1048576:axis==IMPORT_STEPS?65536:
                    axis==IMPORT_FRAME?65536:axis==IMPORT_OUTPUT?8388608:128;
                CHECK(import_budget_trial(&probe,axis,high,false));
                while (low<high) {
                    uint64_t middle=low+(high-low)/2;
                    if (import_budget_trial(&probe,axis,middle,false)) high=middle;
                    else low=middle+1;
                }
                exact=low;
            }
            CHECK(exact && import_budget_trial(&probe,axis,exact,true));
            CHECK(!import_budget_trial(&probe,axis,exact-1,true));
            fprintf(stderr,"Foreign budget %s leaf=%u axis=%u exact=%llu/minus1 same-owner retry/live\n",
                kind,mode,(unsigned)axis,(unsigned long long)exact);
            CHECK(!memcmp(&input,&unchanged,sizeof(input)) && stats(&producer).work==source.work &&
                stats(&producer).allocated_bytes==source.allocated_bytes);
        }
    }
    xr_xir_value_drop(&input);xr_xir_compile_artifact_free(origin);xr_compile_resources_release(producer.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==2);CHECK(!strcmp(argv[1],"array") || !strcmp(argv[1],"tuple") || !strcmp(argv[1],"nominal") ||
        !strcmp(argv[1],"nullable") || !strcmp(argv[1],"enum") || !strcmp(argv[1],"empty"));
    import_budget(argv[1]);return 0;
}
