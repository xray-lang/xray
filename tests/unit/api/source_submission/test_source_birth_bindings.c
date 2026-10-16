/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_birth_bindings.c - Actual Source declarations retain birth storage
 */
#define main source_submission_existing_main
#include "test_source_submission.c"
#undef main

static const char birth_source[]="fn unused<T>(value:T)->T{return value}\nvar value:i64=11\nvar empty=()\n"
    "export fn read()->i64{return value}\nexport fn saved()->i64{return 42}\nfn hidden()->i64{return 17}\n";
static XrSourceStatus birth_invoke(SubmissionOwners *owned,const XrXirInstanceBinding *binding,
    XrXirType type,int64_t expected) {
    XrXirCallStatus started=xr_xir_instance_binding_start(owned->owner->runtime,binding,NULL,0);
    if(started!=XR_XIR_CALL_READY) {
        fprintf(stderr,"birth invocation start=%u\n",started);return submission_call(started);
    }
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(unsigned step=0;step<4096 && status==XR_XIR_CALL_READY;++step)
        status=xr_xir_instance_poll_bounded(owned->owner->runtime,8).outcome.status;
    if(status!=XR_XIR_CALL_RETURNED) {
        fprintf(stderr,"birth invocation terminal=%u\n",status);return submission_call(status);
    }
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(owned->owner->runtime,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==(uint32_t)type && result.payload==expected);xr_xir_value_drop(&result);return XR_SOURCE_OK;
}
static XrSourceStatus birth_publish(SubmissionOwners *owned,const char *source) {
    XrXirSourceText text={NULL,source,strlen(source)};XrXirSourceDiagnostic diagnostic={0};
    XrSourceStatus status=xr_source_submission_prepare(owned->owner,&text,&diagnostic);
    if(status!=XR_SOURCE_OK) {
        fprintf(stderr,"birth prepare status=%u source=%u line=%d message=%s\n",status,diagnostic.status,diagnostic.line,diagnostic.message);
        return status;
    }
    XrXirCallStatus terminal=submission_finish(owned);
    status=xr_source_submission_take(owned->owner,&owned->result);if(status!=XR_SOURCE_OK)return status;
    if(terminal!=XR_XIR_CALL_RETURNED)return submission_call(terminal);
    size_t attempts=runtime_attempts;
    status=xr_source_submission_publish(owned->owner);CHECK(runtime_attempts==attempts);
    if(status!=XR_SOURCE_OK)fprintf(stderr,"birth publication status=%u\n",status);
    xr_xir_call_result_drop(&owned->result);return status;
}
static const XrXirInstanceBinding *birth_lookup(SubmissionOwners *owned,const char *name,
    XrXirInstanceBindingView *view) {
    const XrXirInstanceBinding *binding=NULL;
    CHECK(xr_source_submission_lookup_runtime(owned->owner,name,&binding)==XR_SOURCE_OK && binding);
    CHECK(xr_xir_instance_binding_view(owned->owner->runtime,binding,view)==XR_XIR_CALL_READY);return binding;
}
static void birth_normal(void) {
    SubmissionOwners owned={0};CHECK(submission_open(&owned,&generous)==XR_SOURCE_OK);
    CHECK(birth_publish(&owned,birth_source)==XR_SOURCE_OK);
    XrXirInstanceBindingView old_function={0},old_slot={0},old_read_view={0};
    const XrXirInstanceBinding *old_saved=birth_lookup(&owned,"saved",&old_function);
    const XrXirInstanceBinding *old_value=birth_lookup(&owned,"value",&old_slot);
    const XrXirInstanceBinding *old_read=birth_lookup(&owned,"read",&old_read_view);
    XrXirSourceGenerationDefinition definition={0};
    CHECK(xr_xir_compile_source_generation_definition(owned.owner->source,1,"saved",&definition)==XR_XIR_OK);
    CHECK(old_function.ordinal!=definition.ordinal && xr_xir_type_is_cell(old_slot.types,old_slot.slot_type) &&
        xr_xir_cell_element(old_slot.types,old_slot.slot_type)==XR_XIR_I64);
    XrXirInstanceBindingView unit_view={0},private_view={0};
    const XrXirInstanceBinding *unit=birth_lookup(&owned,"empty",&unit_view);
    CHECK(unit && unit_view.mutable && xr_xir_type_is_cell(unit_view.types,unit_view.slot_type) &&
        xr_xir_cell_element(unit_view.types,unit_view.slot_type)==XR_XIR_UNIT);
    const XrXirInstanceBinding *hidden=birth_lookup(&owned,"hidden",&private_view);
    CHECK(xr_xir_instance_binding_start(owned.owner->runtime,hidden,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(birth_invoke(&owned,old_saved,XR_XIR_I64,42)==XR_SOURCE_OK);
    CHECK(birth_invoke(&owned,old_read,XR_XIR_I64,11)==XR_SOURCE_OK);
    CHECK(submission_text(&owned,"const rejected=missing\n")==XR_SOURCE_REJECTED);
    CHECK(birth_publish(&owned,"var value:bool=true\nexport fn read()->bool{return value}\nexport fn saved()->i64{return 99}\n")==XR_SOURCE_OK);
    XrXirInstanceBindingView new_function={0},new_slot={0},new_read_view={0};
    const XrXirInstanceBinding *new_saved=birth_lookup(&owned,"saved",&new_function);
    const XrXirInstanceBinding *new_value=birth_lookup(&owned,"value",&new_slot);
    const XrXirInstanceBinding *new_read=birth_lookup(&owned,"read",&new_read_view);
    XrXirSourceGenerationBinding lexical={0};
    CHECK(xr_source_submission_lookup(owned.owner,"value",&lexical)==XR_SOURCE_OK);
    CHECK(lexical.generation==3 && new_slot.generation==2 && old_value!=new_value &&
        old_function.program!=new_function.program && xr_xir_type_is_cell(new_slot.types,new_slot.slot_type) &&
        xr_xir_cell_element(new_slot.types,new_slot.slot_type)==XR_XIR_BOOL);
    CHECK(birth_invoke(&owned,old_saved,XR_XIR_I64,42)==XR_SOURCE_OK);
    CHECK(birth_invoke(&owned,new_saved,XR_XIR_I64,99)==XR_SOURCE_OK);
    CHECK(birth_invoke(&owned,old_read,XR_XIR_I64,11)==XR_SOURCE_OK);
    CHECK(birth_invoke(&owned,new_read,XR_XIR_BOOL,1)==XR_SOURCE_OK);
    CHECK(submission_text(&owned,"var value:i64=99\nassert(false,\"unpublished\")\n")==XR_SOURCE_OK);
    CHECK(xr_xir_call_panic_status(submission_finish(&owned)));
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    CHECK(xr_source_submission_abort(owned.owner,&cleanup)!=XR_SOURCE_BUSY);
    XrXirInstanceBindingView unchanged={0};CHECK(birth_lookup(&owned,"value",&unchanged)==new_value);
    CHECK(birth_publish(&owned,"const value=()\nfn saved<T>(value:T)->T{return value}\n")==XR_SOURCE_OK);
    const XrXirInstanceBinding *absent=NULL;
    CHECK(xr_source_submission_lookup_runtime(owned.owner,"value",&absent)==XR_SOURCE_BAD_STATE && !absent);
    CHECK(xr_source_submission_lookup_runtime(owned.owner,"saved",&absent)==XR_SOURCE_BAD_STATE && !absent);
    CHECK(birth_invoke(&owned,old_saved,XR_XIR_I64,42)==XR_SOURCE_OK);
    CHECK(birth_invoke(&owned,old_read,XR_XIR_I64,11)==XR_SOURCE_OK);
    XrXirSourceGenerationView first={0},last={0};
    CHECK(xr_xir_compile_source_generation_view(owned.owner->source,1,&first)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_generation_view(owned.owner->source,5,&last)==XR_XIR_OK);
    SubmissionBindingDraft wrong={.owner=owned.owner,.checked=last.result->checked};
    XrXirSpecializationReceiver rejected={&wrong,submission_receive_bindings};XrXirArtifact *empty=NULL;
    size_t live=runtime_live,bytes=runtime_bytes;
    CHECK(xr_xir_compile_specialize_receive(first.result->checked,&rejected,&empty,NULL)==XR_XIR_BAD_STRUCTURE && !empty);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    submission_close(&owned);
    puts("Actual Source ordinal remap; old42/new99; old Int11/new Bool1; separate serials; abort/Unit/template shadow and receiver failure preserved");
}
static XrSourceStatus birth_drive(SubmissionOwners *owned,const XrCompileResourceLimits *limits) {
    XrSourceStatus status=submission_open(owned,limits);if(status!=XR_SOURCE_OK)return status;
    status=birth_publish(owned,birth_source);if(status!=XR_SOURCE_OK)return status;
    const XrXirInstanceBinding *binding=NULL;
    status=xr_source_submission_lookup_runtime(owned->owner,"saved",&binding);if(status!=XR_SOURCE_OK)return status;
    return birth_invoke(owned,binding,XR_XIR_I64,42);
}
static void birth_resources(void) {
    size_t sites=0;XrCompileResourceStats required={0};
    for(size_t point=0;point<=sites;++point) {
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;SubmissionOwners owned={0};
        XrSourceStatus status=birth_drive(&owned,&generous);
        if(!point) {
            CHECK(status==XR_SOURCE_OK);sites=runtime_attempts;CHECK(sites);
            CHECK(xr_compile_resources_stats(owned.resources,&required)==XR_COMPILE_RESOURCE_OK);
        } else CHECK(status==XR_SOURCE_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at);
        submission_close(&owned);
    }
    runtime_fail_at=SIZE_MAX;
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0 ? &limits.allocated_bytes : axis==1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound && *bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        SubmissionOwners owned={0};CHECK(birth_drive(&owned,&limits)==(delta<0 ? XR_SOURCE_BUDGET : XR_SOURCE_OK));
        submission_close(&owned);
    }
    printf("Source birth binding complete physical faults=%zu; compiler three axes exact/minus1/plus1; physical=0/0\n",sites);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--resources")));
    if(argc==2)birth_resources();else birth_normal();return 0;
}
