/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_state_atomic_i64_runtime.h - Independent Instance state and retained values
 */
#ifndef XIR_LIBRARY_STATE_ATOMIC_I64_RUNTIME_H
#define XIR_LIBRARY_STATE_ATOMIC_I64_RUNTIME_H
#include "xir/xxir.h"
XR_FUNC void xr_test_library_state_atomic_i64_run(const XrXirCompileContext *context,XrXirArtifact *owned);
#ifdef XR_LIBRARY_STATE_ATOMIC_I64_RUNTIME_IMPLEMENTATION
#include "xir/xxir_output.h"
#include "xir/xxir_atomic.h"
enum { LIBRARY_STATE_ATOMIC_I64_EXPORTS=10 };
typedef struct LibraryStateAtomicI64Output { char bytes[64];size_t length; } LibraryStateAtomicI64Output;
static XrXirOutputStatus library_state_atomic_i64_write(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    LibraryStateAtomicI64Output *output=context;
    CHECK(stream==XR_XIR_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;return XR_XIR_OUTPUT_OK;
}
static void library_state_atomic_i64_held(const XrXirValue *value) {
    const char *bytes=NULL;size_t length=0;
    CHECK(value->type==XR_XIR_STRING && xr_xir_string_view(value,&bytes,&length));
    CHECK(length==5 && !memcmp(bytes,"seed!",5));
}
static XrXirDomain *library_state_atomic_i64_read_domain;
/* Retained handles keep their actual arena alive after producer and Instance
 * destruction. This trusted host read is distinct from the GO permission gate. */
static void library_state_atomic_i64_handle(const XrXirValue *value,int64_t expected) {
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena && library_state_atomic_i64_read_domain);
    CHECK(xr_xir_atomic_element(xr_xir_compile_type_arena_types(arena),(XrXirType)value->type)==XR_XIR_I64);
    XrXirValueAdmission admission={.arena=arena,.domain=library_state_atomic_i64_read_domain,
        .work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
    XrXirAtomicRequest request={value,NULL,NULL,0,XR_XIR_ATOMIC_OPERATION_LOAD,XR_XIR_I64};
    XrXirAtomicProgress progress={0};XrXirValue output={0};
    XrXirAtomicOutcome outcome=xr_xir_atomic_start(&request,&admission,NULL,&progress,&output);
    CHECK(outcome.status==XR_XIR_RUN_OK && outcome.permission==XR_XIR_CALL_READY && !outcome.continuing);
    CHECK(output.type==XR_XIR_I64 && output.payload==expected);
    xr_xir_atomic_progress_clear(&progress);xr_xir_value_drop(&output);
}
#if CONSUMER_KIND==0 || CONSUMER_KIND==3
static void library_state_atomic_i64_ids(const XrXirModule *module,uint32_t ids[LIBRARY_STATE_ATOMIC_I64_EXPORTS]) {
    static const char *const library_state_atomic_i64_names[LIBRARY_STATE_ATOMIC_I64_EXPORTS]={"run","ownValue","held","pureWorker","snapshot","atomicHeld","rebind","constWorker","rootHandleWorker","badOrder"};
    for (uint32_t e=0;e<LIBRARY_STATE_ATOMIC_I64_EXPORTS;++e) {
        ids[e]=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *fn=&module->functions[f];
            const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
            if (identity->module!=module->declarations->root_module || fn->name_length!=strlen(library_state_atomic_i64_names[e]) ||
                memcmp(fn->name,library_state_atomic_i64_names[e],fn->name_length)) continue;
            CHECK(identity->exported && ids[e]==UINT32_MAX);ids[e]=f;
        }
        CHECK(ids[e]!=UINT32_MAX);
    }
}
#endif
static bool library_state_atomic_i64_call(XrXirInstance *instance,uint32_t entry,int64_t expected,XrXirValue *held) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if (status==XR_XIR_CALL_OOM) return false;
    CHECK(status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};status=xr_xir_instance_take_result(instance,&value);
    if (status==XR_XIR_CALL_OOM) return false;
    CHECK(status==XR_XIR_CALL_RETURNED);
    if (held) {
        if (value.type==XR_XIR_STRING) library_state_atomic_i64_held(&value);
        else library_state_atomic_i64_handle(&value,expected);
        *held=value;
    }
    else {CHECK(value.type==XR_XIR_I64 && value.payload==expected);xr_xir_value_drop(&value);}
    return true;
}
static bool library_state_atomic_i64_bad_order(XrXirInstance *instance,uint32_t entry) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,NULL,0);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if (status==XR_XIR_CALL_OOM) return false;
    CHECK(status==XR_XIR_CALL_BAD_ARGUMENT && xr_xir_instance_state(instance)==XR_XIR_INSTANCE_READY);
    return true;
}
static bool library_state_atomic_i64_pair(XrXirProgram *program,const uint32_t ids[LIBRARY_STATE_ATOMIC_I64_EXPORTS],XrXirValue held[4]) {
    XrXirInstance *instances[2]={0};LibraryStateAtomicI64Output outputs[2]={0};XrXirOutputSink sinks[2]={0};bool ok=false;
    for (uint32_t i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        sinks[i]=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,library_state_atomic_i64_write,&outputs[i],64};
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sinks[i]};
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instances[i]);
        if (status==XR_XIR_CALL_OOM) goto done;
        CHECK(status==XR_XIR_CALL_READY);
    }
    for (uint32_t i=0;i<2;++i) {
        CHECK(!outputs[i].length);
        if (!library_state_atomic_i64_call(instances[i],ids[1],100,NULL) ||
            !library_state_atomic_i64_call(instances[i],ids[3],41,NULL) ||
            !library_state_atomic_i64_call(instances[i],ids[4],1,NULL) ||
            !library_state_atomic_i64_call(instances[i],ids[7],41,NULL) ||
            !library_state_atomic_i64_call(instances[i],ids[8],0,NULL)) goto done;
    }
    for (uint32_t step=0;step<2;++step) for (uint32_t i=0;i<2;++i) {
        size_t peer=outputs[1-i].length;
        if (!library_state_atomic_i64_call(instances[i],ids[0],step+1,NULL)) goto done;
        CHECK(outputs[1-i].length==peer);
        const char *expected=step ? "1\nseed!\n2\nseed!!\n" : "1\nseed!\n";
        size_t length=step ? 17 : 8;
        CHECK(outputs[i].length==length && !memcmp(outputs[i].bytes,expected,length));
        if (!step && !library_state_atomic_i64_call(instances[i],ids[2],0,&held[i])) goto done;
        if (!step && !library_state_atomic_i64_call(instances[i],ids[5],1,&held[i+2])) goto done;
        library_state_atomic_i64_held(&held[i]);library_state_atomic_i64_handle(&held[i+2],step+1);
        if (!library_state_atomic_i64_call(instances[i],ids[8],step+1,NULL)) goto done;
        if (held[2].type && held[3].type) CHECK(held[2].payload!=held[3].payload);
    }
    for (uint32_t i=0;i<2;++i) {
        if (!library_state_atomic_i64_call(instances[i],ids[1],102,NULL) ||
            !library_state_atomic_i64_call(instances[i],ids[4],3,NULL) ||
            !library_state_atomic_i64_call(instances[i],ids[7],42,NULL) ||
            !library_state_atomic_i64_call(instances[i],ids[6],30,NULL) ||
            !library_state_atomic_i64_call(instances[i],ids[8],30,NULL) ||
            !library_state_atomic_i64_call(instances[i],ids[4],31,NULL)) goto done;
        library_state_atomic_i64_handle(&held[i+2],2);
        XrXirValue current={0};
        if (!library_state_atomic_i64_call(instances[i],ids[5],30,&current)) goto done;
        bool valid=library_state_atomic_i64_bad_order(instances[i],ids[9]);
        library_state_atomic_i64_handle(&current,30);xr_xir_value_drop(&current);
        if (!valid) goto done;
    }
    ok=true;
done:
    for (uint32_t i=0;i<2;++i) if (instances[i]) CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    return ok;
}
static XrXirStatus library_state_atomic_i64_seal_operation(const XrXirCompileContext *context,void *opaque) {
    XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_program_seal(context,opaque,&program);
    CHECK(status==XR_XIR_OK ? program!=NULL : program==NULL);xr_xir_compile_program_drop(program);return status;
}
static void library_state_atomic_i64_finish(const XrXirCompileContext *context,XrXirProgramSpec *spec,XrXirArtifact *lowered,
    const uint32_t ids[LIBRARY_STATE_ATOMIC_I64_EXPORTS]) {
    library_compile_operation_cases("Library private state seal",library_state_atomic_i64_seal_operation,spec);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,spec,&program)==XR_XIR_OK);
    CHECK(xr_xir_domain_new(65536,&library_state_atomic_i64_read_domain)==XR_XIR_VALUE_OK);
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t p=0;p<=sites;++p) {
        XrXirValue held[4]={0};runtime_attempts=0;runtime_fail_at=p ? p-1 : SIZE_MAX;
        bool ok=library_state_atomic_i64_pair(program,ids,held);
        if (!p) {CHECK(ok);sites=runtime_attempts;CHECK(sites);} else CHECK(!ok);
        for (uint32_t i=0;i<4;++i) xr_xir_value_drop(&held[i]);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;XrXirValue held[4]={0};CHECK(library_state_atomic_i64_pair(program,ids,held));
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);
    runtime_attempts=0;runtime_fail_at=0;
    for (uint32_t i=0;i<4;++i) {
        if (i<2) library_state_atomic_i64_held(&held[i]);else library_state_atomic_i64_handle(&held[i],2);
        xr_xir_value_drop(&held[i]);
    }
    xr_xir_domain_drop(library_state_atomic_i64_read_domain);library_state_atomic_i64_read_domain=NULL;
    CHECK(!runtime_attempts && !runtime_live && !runtime_bytes);runtime_fail_at=SIZE_MAX;
    printf("Library Atomic I64 twoInstances const41/42 var1/2 rebind30 oldHandle2 rootPreparedChild dynamicOrderRejected retainedString runtimeOOM=%zu physicalzero\n",sites);
}
#endif
#endif
