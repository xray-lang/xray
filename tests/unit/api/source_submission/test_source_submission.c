/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_submission.c - Real paired Source and Instance publication
 */
#include "api/xsource_submission.h"
#include "xir/xxir_panic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "base/xcompile_state.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_task.c"
#include "api/xsource_submission.c"

typedef struct SubmissionOwners {
    XrCompileResources *resources;
    XrCompileState *state;
    XrSourceSubmission *owner;
    XrXirCallResult result;
    unsigned outputs;
    bool reentry;
} SubmissionOwners;
static XrXirOutputStatus submission_output(void *context,const XrXirOutputGroup *group) {
    SubmissionOwners *owned=context;
    CHECK(group && group->count==1 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==17);
    ++owned->outputs;
    if(owned->reentry) {
        XrXirInstanceResult untouched={0};untouched.epoch=77;
        XrXirCallStatus cleanup=XR_XIR_CALL_RETURNED;
        XrSourceSubmission *original=owned->owner;
        CHECK(xr_source_submission_poll(original,1,&untouched)==XR_SOURCE_BUSY && untouched.epoch==77);
        CHECK(xr_source_submission_publish(original)==XR_SOURCE_BUSY);
        CHECK(xr_source_submission_abort(original,&cleanup)==XR_SOURCE_BUSY && cleanup==XR_XIR_CALL_RETURNED);
        CHECK(xr_source_submission_drop(&owned->owner,&cleanup)==XR_SOURCE_BUSY && owned->owner==original && cleanup==XR_XIR_CALL_RETURNED);
    }
    return XR_XIR_OUTPUT_OK;
}
static const XrCompileResourceLimits generous={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
static XrSourceStatus submission_open(SubmissionOwners *owned,const XrCompileResourceLimits *limits) {
    XrCompileResourceStatus status=xr_compile_resources_new(limits,&owned->resources);
    if(status!=XR_COMPILE_RESOURCE_OK)return submission_resource(status);
    status=xr_compile_state_new(owned->resources,&owned->state);
    if(status!=XR_COMPILE_RESOURCE_OK)return submission_resource(status);
    XrSourceSubmissionConfig config={0};config.state=owned->state;
    config.source=(XrXirSourceGenerationConfig){{owned->resources,xr_xir_compile_default_limits()},"paired-source-records",XR_SUBMISSION_STDLIB,NULL,NULL};
    config.target=(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_instance_config_init(&config.runtime,sizeof(config.runtime))==XR_XIR_CALL_READY);
    config.runtime.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,submission_output,owned};
    return xr_source_submission_new(&config,&owned->owner);
}
static XrSourceStatus submission_text(SubmissionOwners *owned,const char *source) {
    XrXirSourceText text={NULL,source,strlen(source)};XrXirSourceDiagnostic diagnostic={0};
    return xr_source_submission_prepare(owned->owner,&text,&diagnostic);
}
static XrXirCallStatus submission_finish(SubmissionOwners *owned) {
    for(size_t step=0;step<4096;++step) {
        XrXirInstanceResult observed={0};CHECK(xr_source_submission_poll(owned->owner,8,&observed)==XR_SOURCE_OK);
        if(observed.outcome.status!=XR_XIR_CALL_READY)return observed.outcome.status;
    }
    CHECK(0);return XR_XIR_CALL_LIMIT;
}
static XrSourceStatus submission_drive(SubmissionOwners *owned,const XrCompileResourceLimits *limits) {
    XrSourceStatus status=submission_open(owned,limits);if(status!=XR_SOURCE_OK)return status;
    char source[]="const value:i64 = 5\nprint(17)\n";
    XrXirSourceText text={NULL,source,sizeof(source)-1};
    status=xr_source_submission_prepare(owned->owner,&text,NULL);if(status!=XR_SOURCE_OK)return status;
    memset(source,'!',sizeof(source));
    xr_compile_state_release(owned->state);owned->state=NULL;
    xr_compile_resources_release(owned->resources);owned->resources=NULL;
    XrXirCallStatus terminal=submission_finish(owned);
    status=xr_source_submission_take(owned->owner,&owned->result);if(status!=XR_SOURCE_OK)return status;
    CHECK(owned->result.status==terminal);
    if(terminal!=XR_XIR_CALL_RETURNED) {CHECK(terminal==XR_XIR_CALL_OOM);return XR_SOURCE_OK;}
    size_t before=runtime_attempts;
    status=xr_source_submission_publish(owned->owner);
    CHECK(status==XR_SOURCE_OK && runtime_attempts==before);
    return status;
}
static void submission_close(SubmissionOwners *owned) {
    runtime_fail_at=SIZE_MAX;
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    XrSourceStatus dropped=xr_source_submission_drop(&owned->owner,&cleanup);
    CHECK(dropped!=XR_SOURCE_BUSY && !owned->owner);
    xr_xir_call_result_drop(&owned->result);xr_compile_state_release(owned->state);
    xr_compile_resources_release(owned->resources);CHECK(!runtime_live && !runtime_bytes);
}
static void submission_normal(void) {
    runtime_fail_at=SIZE_MAX;SubmissionOwners owned={0};owned.reentry=true;
    CHECK(submission_drive(&owned,&generous)==XR_SOURCE_OK && owned.outputs==1);
    XrXirSourceGenerationBinding old={0};CHECK(xr_source_submission_lookup(owned.owner,"value",&old)==XR_SOURCE_OK);
    CHECK(old.generation==1 && old.declaration && old.declaration->type.type==XR_XIR_I64);
    xr_xir_call_result_drop(&owned.result);
    XrXirSourceText bad={NULL,"const value=missing",19};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_source_submission_prepare(owned.owner,&bad,&diagnostic)==XR_SOURCE_REJECTED && diagnostic.line==1);
    XrXirSourceGenerationBinding after={0};CHECK(xr_source_submission_lookup(owned.owner,"value",&after)==XR_SOURCE_OK && after.generation==1);
    CHECK(submission_text(&owned,"const value=\"new\"\nassert(false,\"candidate-owned-panic\")\n")==XR_SOURCE_OK);
    CHECK(xr_xir_call_panic_status(submission_finish(&owned)));
    CHECK(xr_source_submission_take(owned.owner,&owned.result)==XR_SOURCE_OK);
    CHECK(xr_source_submission_publish(owned.owner)==XR_SOURCE_BAD_STATE);
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    XrSourceStatus aborted=xr_source_submission_abort(owned.owner,&cleanup);
    CHECK(aborted!=XR_SOURCE_BUSY && !owned.owner->source_serial && !owned.owner->runtime_serial);
    CHECK(xr_source_submission_lookup(owned.owner,"value",&after)==XR_SOURCE_OK && after.generation==1 && after.declaration->type.type==XR_XIR_I64);
    CHECK(submission_text(&owned,"const value:i64=6\n")==XR_SOURCE_OK);
    CHECK(submission_finish(&owned)==XR_XIR_CALL_RETURNED);
    XrCompileResourceStats before={0},later={0};XrCompileResources *ledger=xr_compile_state_resources(owned.owner->state);
    CHECK(xr_compile_resources_stats(ledger,&before)==XR_COMPILE_RESOURCE_OK);
    size_t allocations=runtime_attempts;CHECK(xr_source_submission_publish(owned.owner)==XR_SOURCE_OK && runtime_attempts==allocations);
    CHECK(xr_compile_resources_stats(ledger,&later)==XR_COMPILE_RESOURCE_OK && before.allocated_bytes==later.allocated_bytes && before.work==later.work);
    CHECK(xr_source_submission_lookup(owned.owner,"value",&after)==XR_SOURCE_OK && after.generation==4 && after.declaration->type.type==XR_XIR_I64);
    CHECK(xr_source_submission_drop(&owned.owner,&cleanup)!=XR_SOURCE_BUSY && !owned.owner);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&owned.result.panic.message,&bytes,&length));
    CHECK(length==strlen("candidate-owned-panic") && !memcmp(bytes,"candidate-owned-panic",length));
    submission_close(&owned);
    puts("paired actual Source/Instance commit, compiler death, abort, owned Panic and callback reentry PASS; historical semantic bindings OPEN");
}
static void submission_resource_axes(void) {
    runtime_fail_at=SIZE_MAX;SubmissionOwners owned={0};
    CHECK(submission_drive(&owned,&generous)==XR_SOURCE_OK);
    XrCompileResourceStats required={0};
    CHECK(xr_compile_resources_stats(xr_compile_state_resources(owned.owner->state),&required)==XR_COMPILE_RESOURCE_OK);
    submission_close(&owned);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound>1 && *bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        owned=(SubmissionOwners){0};XrSourceStatus status=submission_drive(&owned,&limits);
        CHECK(status==(delta<0?XR_SOURCE_BUDGET:XR_SOURCE_OK));
        if(delta<0 && owned.owner) {
            XrCompileResources *ledger=xr_compile_state_resources(owned.owner->state);
            XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(ledger,&before)==XR_COMPILE_RESOURCE_OK);
            size_t attempts=runtime_attempts;
            CHECK(submission_text(&owned,"const value:i64=5\n")==XR_SOURCE_BUDGET && runtime_attempts==attempts);
            CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK && before.work==after.work && before.allocated_bytes==after.allocated_bytes);
        }
        submission_close(&owned);
    }
    puts("paired compiler axes exact/minus-one/plus-one, sticky retry without work/allocation and physical0 PASS");
}
static void submission_faults(void) {
    size_t count=0;
    for(size_t point=0;!point || point<=count;++point) {
        runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;SubmissionOwners owned={0};
        XrSourceStatus status=submission_drive(&owned,&generous);
        if(!point){CHECK(status==XR_SOURCE_OK && owned.result.status==XR_XIR_CALL_RETURNED);count=runtime_attempts;CHECK(count);}
        else {
            CHECK(runtime_attempts>runtime_fail_at);
            CHECK(status==XR_SOURCE_OUT_OF_MEMORY || (status==XR_SOURCE_OK && owned.result.status==XR_XIR_CALL_OOM));
        }
        submission_close(&owned);
    }
    printf("paired Source/Instance full actual allocator cuts=%zu; physical=0/0\n",count);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--normal")));
    if(argc==2){submission_normal();submission_resource_axes();}else submission_faults();return 0;
}
