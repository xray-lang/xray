/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_unit_destructure_cell_runtime.h - Independent Instance state and retained values
 */
#ifndef XIR_UNIT_DESTRUCTURE_CELL_RUNTIME_H
#define XIR_UNIT_DESTRUCTURE_CELL_RUNTIME_H
#include "xir/xxir.h"
XR_FUNC void xr_test_unit_destructure_cell_run(const XrXirCompileContext *context,XrXirArtifact *owned,bool pending);
#ifdef XR_UNIT_DESTRUCTURE_CELL_RUNTIME_IMPLEMENTATION
#include "xir/xxir_output.h"
enum { UNIT_DESTRUCTURE_CELL_EXPORTS=5 };
typedef struct UnitDestructureCellOutput { char bytes[64];size_t length; } UnitDestructureCellOutput;
static XrXirOutputStatus unit_destructure_cell_write(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    UnitDestructureCellOutput *output=context;
    CHECK(stream==XR_XIR_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;return XR_XIR_OUTPUT_OK;
}
static void unit_destructure_cell_held(const XrXirValue *value) {
    CHECK(value->type==XR_XIR_UNIT && !value->reserved && !value->payload);
}
#if CONSUMER_KIND==0 || CONSUMER_KIND==3
static void unit_destructure_cell_ids(const XrXirModule *module,uint32_t ids[UNIT_DESTRUCTURE_CELL_EXPORTS]) {
    static const char *const unit_destructure_cell_names[UNIT_DESTRUCTURE_CELL_EXPORTS]={"run","ownValue","held","pureWorker","snapshot"};
    for (uint32_t e=0;e<UNIT_DESTRUCTURE_CELL_EXPORTS;++e) {
        ids[e]=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *fn=&module->functions[f];
            const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
            if (identity->module!=module->declarations->root_module || fn->name_length!=strlen(unit_destructure_cell_names[e]) ||
                memcmp(fn->name,unit_destructure_cell_names[e],fn->name_length)) continue;
            CHECK(identity->exported && ids[e]==UINT32_MAX);ids[e]=f;
        }
        CHECK(ids[e]!=UINT32_MAX);
    }
}
#endif
static bool unit_destructure_cell_call(XrXirInstance *instance,uint32_t entry,XrXirType expected_type,int64_t expected,
    XrXirValue *held,XrXirCallStatus *failure) {
    CHECK(failure && *failure==XR_XIR_CALL_READY);
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if (status==XR_XIR_CALL_OOM) {*failure=status;return false;}
    if (status!=XR_XIR_CALL_RETURNED)
        fprintf(stderr,"UNIT_CALL entry=%u status=%u state=%u fail_at=%zu attempts=%zu\n",entry,
            (unsigned)status,(unsigned)xr_xir_instance_state(instance),runtime_fail_at,runtime_attempts);
    CHECK(status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};status=xr_xir_instance_take_result(instance,&value);
    if (status==XR_XIR_CALL_OOM) {*failure=status;return false;}
    CHECK(status==XR_XIR_CALL_RETURNED);
    if (held) {unit_destructure_cell_held(&value);*held=value;}
    else {CHECK(value.type==(uint32_t)expected_type && value.payload==expected);xr_xir_value_drop(&value);}
    return true;
}
static size_t unit_destructure_cell_free_failures[2];
static void unit_destructure_cell_free_observed(XrXirInstance *instance,XrXirCallStatus failure) {
    CHECK(xr_xir_task_executor_root_idle(instance->executor));
    CHECK(!instance->executor->active && !instance->executor->ready_head);
    CHECK(!instance->call || !instance->call->top);
    XrXirCallStatus expected=xr_xir_task_executor_completion_status(instance->executor);
    CHECK(expected==XR_XIR_CALL_READY || expected==failure);
    CHECK(failure==XR_XIR_CALL_READY || failure==XR_XIR_CALL_OOM || failure==XR_XIR_CALL_LIMIT);
    /* A consumed owner still reports the exact recorded executor failure. */
    size_t attempts=runtime_attempts;
    XrXirCallStatus status=xr_xir_instance_free(instance);
    CHECK(status==expected && runtime_attempts==attempts);
    if (status==XR_XIR_CALL_OOM) ++unit_destructure_cell_free_failures[0];
    else if (status==XR_XIR_CALL_LIMIT) ++unit_destructure_cell_free_failures[1];
}
static bool unit_destructure_cell_pair(XrXirProgram *program,
    const uint32_t ids[UNIT_DESTRUCTURE_CELL_EXPORTS],XrXirValue held[2]) {
    XrXirInstance *instances[2]={0};UnitDestructureCellOutput outputs[2]={0};XrXirOutputSink sinks[2]={0};bool ok=false;
    XrXirCallStatus failures[2]={XR_XIR_CALL_READY,XR_XIR_CALL_READY};
    for (uint32_t i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        sinks[i]=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,unit_destructure_cell_write,&outputs[i],64};
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sinks[i]};
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instances[i]);
        if (status==XR_XIR_CALL_OOM) goto done;
        CHECK(status==XR_XIR_CALL_READY);
    }
    for (uint32_t i=0;i<2;++i) {
        CHECK(!outputs[i].length);
        if (!unit_destructure_cell_call(instances[i],ids[1],XR_XIR_BOOL,0,NULL,&failures[i]) ||
            !unit_destructure_cell_call(instances[i],ids[3],XR_XIR_I64,41,NULL,&failures[i]) ||
            !unit_destructure_cell_call(instances[i],ids[4],XR_XIR_I64,1,NULL,&failures[i])) goto done;
    }
    for (uint32_t step=0;step<2;++step) for (uint32_t i=0;i<2;++i) {
        size_t peer=outputs[1-i].length;
        int64_t expected_value=step ? 0 : 1;
        if (!unit_destructure_cell_call(instances[i],ids[0],XR_XIR_BOOL,expected_value,NULL,&failures[i])) goto done;
        CHECK(outputs[1-i].length==peer);
        const char *expected=step ? "true\nfalse\n" : "true\n";
        size_t length=step ? 11 : 5;
        CHECK(outputs[i].length==length && !memcmp(outputs[i].bytes,expected,length));
        if (!unit_destructure_cell_call(instances[i],ids[1],XR_XIR_BOOL,expected_value,NULL,&failures[i]) ||
            !unit_destructure_cell_call(instances[i],ids[4],XR_XIR_I64,expected_value+1,NULL,&failures[i])) goto done;
        if (!step && !unit_destructure_cell_call(instances[i],ids[2],XR_XIR_UNIT,0,&held[i],&failures[i])) goto done;
        unit_destructure_cell_held(&held[i]);
    }
    ok=true;
done:
    for (uint32_t i=0;i<2;++i) if (instances[i]) unit_destructure_cell_free_observed(instances[i],failures[i]);
    return ok;
}
static XrXirStatus unit_destructure_cell_seal_operation(const XrXirCompileContext *context,void *opaque) {
    XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_program_seal(context,opaque,&program);
    CHECK(status==XR_XIR_OK ? program!=NULL : program==NULL);xr_xir_compile_program_drop(program);return status;
}
typedef struct UnitDestructurePendingLog {
    const XrXirDeclarations *declarations;
    uint32_t published[2],released[2],ready[2];
} UnitDestructurePendingLog;
typedef struct UnitDestructurePendingCost {
    XrXirCallStatus status;
    size_t sites;
    XrXirDomainBudgetStats budgets[2];
} UnitDestructurePendingCost;
static void unit_destructure_cell_pending_trace(void *opaque,XrXirLifecycleEvent event,uint32_t index) {
    UnitDestructurePendingLog *log=opaque;const XrXirDeclarations *d=log->declarations;
    if (event==XR_XIR_SLOT_PUBLISHED || event==XR_XIR_SLOT_RELEASED) {
        CHECK(index<d->slot_count);
        unsigned owner=d->slots[index].module!=d->root_module;
        if (event==XR_XIR_SLOT_PUBLISHED) ++log->published[owner];
        else ++log->released[owner];
    } else if (event==XR_XIR_MODULE_READY) {
        CHECK(index<d->module_count);++log->ready[index!=d->root_module];
    }
}
static UnitDestructurePendingCost unit_destructure_cell_pending_pair(XrXirProgram *program,
    const uint32_t ids[UNIT_DESTRUCTURE_CELL_EXPORTS],const XrXirInstanceConfig *limits,unsigned selected) {
    UnitDestructurePendingCost cost={.status=XR_XIR_CALL_READY};
    XrXirInstance *instances[2]={0};XrXirDomain *domains[2]={0};
    XrXirCallStatus failures[2]={XR_XIR_CALL_READY,XR_XIR_CALL_READY};
    UnitDestructurePendingLog logs[2]={0};UnitDestructureCellOutput outputs[2]={0};
    XrXirOutputSink sinks[2]={0};unsigned completed=0;
    for (unsigned pass=0;pass<2;++pass) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        if (limits && pass==selected) config=*limits;
        logs[pass].declarations=program->declarations;
        config.trace=unit_destructure_cell_pending_trace;config.trace_context=&logs[pass];
        sinks[pass]=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,unit_destructure_cell_write,&outputs[pass],64};
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sinks[pass]};
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instances[pass]);
        if (status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT) {CHECK(!instances[pass]);cost.status=status;goto done;}
        CHECK(status==XR_XIR_CALL_READY && instances[pass]);
        domains[pass]=instances[pass]->domain;CHECK(xr_xir_domain_retain(domains[pass]));
        status=xr_xir_instance_start(instances[pass],ids[0],NULL,0);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instances[pass],UINT64_MAX).outcome.status;
        CHECK(status==XR_XIR_CALL_BAD_STATE || status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT);
        CHECK(!outputs[pass].length && !logs[pass].published[0] && !logs[pass].ready[0]);
        if (xr_xir_instance_state(instances[pass])==XR_XIR_INSTANCE_FAILED) {
            size_t attempts=runtime_attempts;XrXirCallResult first={0},second={0};
            CHECK(xr_xir_instance_copy_failure(instances[pass],&first)==status);
            CHECK(xr_xir_instance_copy_failure(instances[pass],&second)==status);
            CHECK(!memcmp(&first,&second,sizeof(first)) && xr_xir_call_result_valid(&first));
            CHECK(xr_xir_instance_start(instances[pass],ids[1],NULL,0)==status);
            CHECK(xr_xir_instance_poll_bounded(instances[pass],1).outcome.status==status);
            XrXirValue output={XR_XIR_I64,0,123},sentinel=output;
            CHECK(xr_xir_instance_take_result(instances[pass],&output)==XR_XIR_CALL_BAD_STATE);
            CHECK(!memcmp(&output,&sentinel,sizeof(output)) && runtime_attempts==attempts);
            xr_xir_call_result_drop(&first);xr_xir_call_result_drop(&second);
        } else CHECK(status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT);
        if (status!=XR_XIR_CALL_BAD_STATE) {failures[pass]=cost.status=status;goto done;}
        CHECK(xr_xir_instance_state(instances[pass])==XR_XIR_INSTANCE_FAILED);
        CHECK(logs[pass].published[1]==2 && logs[pass].ready[1]==1);
        ++completed;
    }
    CHECK(domains[0]!=domains[1]);cost.status=XR_XIR_CALL_BAD_STATE;
done:
    for (unsigned pass=0;pass<2;++pass) {
        if (instances[pass]) unit_destructure_cell_free_observed(instances[pass],failures[pass]);
        CHECK(!logs[pass].published[0] && !logs[pass].released[0] && !logs[pass].ready[0]);
        CHECK(logs[pass].released[1]==logs[pass].published[1]);
        if (domains[pass]) {
            cost.budgets[pass]=xr_xir_domain_budget_stats(domains[pass]);
            CHECK(!cost.budgets[pass].call_live && !cost.budgets[pass].metadata_live);
            CHECK(xr_xir_domain_stats(domains[pass]).live_bytes==sizeof(XrXirDomain));
            xr_xir_domain_drop(domains[pass]);
        }
    }
    CHECK(cost.status!=XR_XIR_CALL_BAD_STATE || completed==2);cost.sites=runtime_attempts;return cost;
}
static void unit_destructure_cell_pending_finish(const XrXirCompileContext *context,XrXirProgramSpec *spec,
    XrXirArtifact *lowered,const uint32_t ids[UNIT_DESTRUCTURE_CELL_EXPORTS]) {
    library_compile_operation_cases("Unpublished Unit authentic Program seal",unit_destructure_cell_seal_operation,spec);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,spec,&program)==XR_XIR_OK);
    size_t live=runtime_live,bytes=runtime_bytes;runtime_attempts=0;runtime_fail_at=SIZE_MAX;
    UnitDestructurePendingCost baseline=unit_destructure_cell_pending_pair(program,ids,NULL,0);
    CHECK(baseline.status==XR_XIR_CALL_BAD_STATE && baseline.sites && runtime_live==live && runtime_bytes==bytes);
    for (size_t site=0;site<baseline.sites;++site) {
        runtime_attempts=0;runtime_fail_at=site;
        UnitDestructurePendingCost failure=unit_destructure_cell_pending_pair(program,ids,NULL,0);
        CHECK(failure.status==XR_XIR_CALL_OOM && runtime_attempts>site);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;
    for (unsigned pass=0;pass<2;++pass) for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        uint64_t exact=axis==0 ? baseline.budgets[pass].requested_bytes :
            axis==1 ? baseline.budgets[pass].requested_call_bytes : baseline.budgets[pass].work;
        CHECK(exact>1);XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        if (axis==0) config.requested_value_limit=exact-minus;
        else if (axis==1) config.requested_call_limit=exact-minus;
        else config.work_limit=exact-minus;
        runtime_attempts=0;UnitDestructurePendingCost measured=unit_destructure_cell_pending_pair(program,ids,&config,pass);
        CHECK(measured.status==(minus ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_STATE));
        CHECK(runtime_live==live && runtime_bytes==bytes);
        printf("UNIT_PENDING_AXIS instance=%u axis=%u minus=%u exact=%llu status=%u physicalbaseline\n",
            pass,axis,minus,(unsigned long long)exact,measured.status);
    }
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);
    CHECK(!runtime_live && !runtime_bytes);
    printf("UNIT_PENDING twoInstances BAD_STATE sticky zeroRootGroupPublications runtimeOOM=%zu threeaxes physicalzero\n",baseline.sites);
    printf("UNIT_FREE exactCompletion propagatedOOM=%zu propagatedLIMIT=%zu physicalzero\n",
        unit_destructure_cell_free_failures[0],unit_destructure_cell_free_failures[1]);
}

static void unit_destructure_cell_finish(const XrXirCompileContext *context,XrXirProgramSpec *spec,XrXirArtifact *lowered,
    const uint32_t ids[UNIT_DESTRUCTURE_CELL_EXPORTS]) {
    library_compile_operation_cases("Library private state seal",unit_destructure_cell_seal_operation,spec);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,spec,&program)==XR_XIR_OK);
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t p=0;p<=sites;++p) {
        XrXirValue held[2]={0};runtime_attempts=0;runtime_fail_at=p ? p-1 : SIZE_MAX;
        bool ok=unit_destructure_cell_pair(program,ids,held);
        if (!p) {CHECK(ok);sites=runtime_attempts;CHECK(sites);} else CHECK(!ok);
        for (uint32_t i=0;i<2;++i) xr_xir_value_drop(&held[i]);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;XrXirValue held[2]={0};CHECK(unit_destructure_cell_pair(program,ids,held));
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);
    runtime_attempts=0;runtime_fail_at=0;
    for (uint32_t i=0;i<2;++i) {unit_destructure_cell_held(&held[i]);xr_xir_value_drop(&held[i]);}
    CHECK(!runtime_attempts && !runtime_live && !runtime_bytes);runtime_fail_at=SIZE_MAX;
    printf("Library Unit/bool twoInstances false/true/false rootSnapshot1/2/1 pureWorker41 retainedUnit runtimeOOM=%zu physicalzero\n",sites);
    printf("UNIT_FREE exactCompletion propagatedOOM=%zu propagatedLIMIT=%zu physicalzero\n",
        unit_destructure_cell_free_failures[0],unit_destructure_cell_free_failures[1]);
}
#endif
#endif
