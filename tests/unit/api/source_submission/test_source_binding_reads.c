/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_binding_reads.c - Checked birth readers return actual owned values
 */
#define main source_submission_existing_main
#include "test_source_submission.c"
#undef main
#include "xir/xxir_array.h"
#include "module/xmodule_identity.h"

static XrSourceStatus reads_publish(SubmissionOwners *owned,const char *source) {
    XrXirSourceText text={NULL,source,strlen(source)};XrXirSourceDiagnostic diagnostic={0};
    XrSourceStatus status=xr_source_submission_prepare(owned->owner,&text,&diagnostic);
    if(status!=XR_SOURCE_OK) {
        if(status!=XR_SOURCE_BUDGET && status!=XR_SOURCE_OUT_OF_MEMORY)
            fprintf(stderr,"reader prepare=%u source=%u line=%d message=%s\n",status,diagnostic.status,diagnostic.line,diagnostic.message);
        return status;
    }
    XrXirCallStatus terminal=submission_finish(owned);
    status=xr_source_submission_take(owned->owner,&owned->result);if(status!=XR_SOURCE_OK)return status;
    if(terminal!=XR_XIR_CALL_RETURNED)return submission_call(terminal);
    size_t attempts=runtime_attempts;status=xr_source_submission_publish(owned->owner);
    CHECK(runtime_attempts==attempts);xr_xir_call_result_drop(&owned->result);return status;
}
static void reads_scalar(SubmissionOwners *owned,const char *name,uint32_t type,int64_t payload) {
    XrXirValue value={0};CHECK(xr_source_submission_read(owned->owner,name,&value)==XR_SOURCE_OK);
    CHECK(value.type==type && !value.reserved && value.payload==payload);xr_xir_value_drop(&value);
}
static void reads_normal(void) {
    SubmissionOwners owned={0};CHECK(submission_open(&owned,&generous)==XR_SOURCE_OK);
    CHECK(reads_publish(&owned,"fn unused<T>(value:T)->T{return value}\nvar value:i64=11\nvar empty=()\n"
        "const text=\"birth-owned\"\nconst values=[11,42]\nconst callback=fn()->i64{return value}\n"
        "export fn mutate(){value=42}\nprint(17)\n")==XR_SOURCE_OK);
    reads_scalar(&owned,"value",XR_XIR_I64,11);reads_scalar(&owned,"empty",XR_XIR_UNIT,0);
    XrXirValue occupied={XR_XIR_I64,0,99},missing={0};
    CHECK(xr_source_submission_read(owned.owner,"value",&occupied)==XR_SOURCE_BAD_ARGUMENT && occupied.payload==99);
    CHECK(xr_source_submission_read(owned.owner,"mutate",&missing)==XR_SOURCE_BAD_STATE && !missing.type && !missing.payload);
    CHECK(xr_source_submission_read(owned.owner,"absent",&missing)!=XR_SOURCE_OK && !missing.type && !missing.payload);
    const XrXirInstanceBinding *mutate=NULL;
    CHECK(xr_source_submission_lookup_runtime(owned.owner,"mutate",&mutate)==XR_SOURCE_OK);
    CHECK(xr_xir_instance_binding_start(owned.owner->runtime,mutate,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus terminal;
    do {terminal=xr_xir_instance_poll_bounded(owned.owner->runtime,32).outcome.status;}while(terminal==XR_XIR_CALL_READY);
    CHECK(terminal==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(owned.owner->runtime,&missing)==XR_XIR_CALL_RETURNED);
    reads_scalar(&owned,"value",XR_XIR_I64,42);
    CHECK(owned.outputs==1);
    CHECK(submission_text(&owned,"const rejected=missing\n")==XR_SOURCE_REJECTED);
    CHECK(reads_publish(&owned,"var value:bool=true\n")==XR_SOURCE_OK);
    reads_scalar(&owned,"value",XR_XIR_BOOL,1);reads_scalar(&owned,"empty",XR_XIR_UNIT,0);
    XrXirSourceGenerationBinding lexical={0};CHECK(xr_source_submission_lookup(owned.owner,"value",&lexical)==XR_SOURCE_OK && lexical.generation==3);
    CHECK(submission_text(&owned,"var value:i64=99\nassert(false,\"unpublished\")\n")==XR_SOURCE_OK);
    CHECK(xr_source_submission_read(owned.owner,"value",&missing)==XR_SOURCE_BUSY && !missing.type && !missing.payload);
    CHECK(xr_xir_call_panic_status(submission_finish(&owned)));
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;CHECK(xr_source_submission_abort(owned.owner,&cleanup)!=XR_SOURCE_BUSY);
    reads_scalar(&owned,"value",XR_XIR_BOOL,1);
    CHECK(reads_publish(&owned,"const value=()\n")==XR_SOURCE_OK);
    reads_scalar(&owned,"value",XR_XIR_UNIT,0);
    XrXirValue text={0},array={0},callback={0};
    CHECK(xr_source_submission_read(owned.owner,"text",&text)==XR_SOURCE_OK);
    CHECK(xr_source_submission_read(owned.owner,"values",&array)==XR_SOURCE_OK);
    CHECK(xr_source_submission_read(owned.owner,"callback",&callback)==XR_SOURCE_OK);
    CHECK(xr_xir_value_arena(&array) && xr_xir_value_arena(&callback) && owned.outputs==1);
    CHECK(xr_source_submission_drop(&owned.owner,&cleanup)!=XR_SOURCE_BUSY && !owned.owner);
    xr_compile_state_release(owned.state);owned.state=NULL;xr_compile_resources_release(owned.resources);owned.resources=NULL;
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&text,&bytes,&length) && length==11 && !memcmp(bytes,"birth-owned",11));
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(4096,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(&array),.domain=domain,.work=10000,.scratch_bytes=4096};
    XrXirValue element={0};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_get(&array,1,&admission,&element,&fault)==XR_XIR_VALUE_OK &&
        element.type==XR_XIR_I64 && element.payload==42);
    CHECK(xr_xir_value_valid(&callback));
    xr_xir_value_drop(&element);xr_xir_value_drop(&callback);xr_xir_value_drop(&array);xr_xir_value_drop(&text);
    xr_xir_domain_drop(domain);
    submission_close(&owned);
    puts("Actual checked birth reads: live Cell42, Bool/Unit shadow, failed serial, pending guard, one initializer output, owned String/Array/callable after producer death; physical0");
}
static const char *const reads_sources[]={"var value:i64=42\n","const value=\"owned\"\n",
    "var root:i64=42\nconst value=fn()->i64{return root}\n",
    "fn answer()->i64{return 42}\nconst value=comptime answer()\n",
    "const value:i64=42\nassert(value==42)\n"};
static XrSourceStatus reads_drive(SubmissionOwners *owned,const XrCompileResourceLimits *limits,unsigned family,XrXirValue *value) {
    XrSourceStatus status=submission_open(owned,limits);if(status!=XR_SOURCE_OK)return status;
    status=reads_publish(owned,reads_sources[family]);if(status!=XR_SOURCE_OK)return status;
    return xr_source_submission_read(owned->owner,"value",value);
}
static void reads_resources(bool faults) {
    for(unsigned family=0;family<sizeof(reads_sources)/sizeof(*reads_sources);++family) {
        size_t sites=0;XrCompileResourceStats required={0};
        for(size_t point=0;point<=(faults ? sites : 0);++point) {
            runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
            SubmissionOwners owned={0};XrXirValue value={0};XrSourceStatus status=reads_drive(&owned,&generous,family,&value);
            if(!point) {
                if(status!=XR_SOURCE_OK)fprintf(stderr,"reader family%u status=%u\n",family,status);
                CHECK(status==XR_SOURCE_OK && xr_xir_value_valid(&value));sites=runtime_attempts;CHECK(sites);
                CHECK(xr_compile_resources_stats(owned.resources,&required)==XR_COMPILE_RESOURCE_OK);
            } else {
                CHECK(status==XR_SOURCE_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at);
                CHECK(!value.type && !value.reserved && !value.payload);
            }
            xr_xir_value_drop(&value);submission_close(&owned);
        }
        runtime_fail_at=SIZE_MAX;
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
            uint64_t *bound=axis==0 ? &limits.allocated_bytes : axis==1 ? &limits.live_bytes : &limits.work;
            CHECK(*bound && *bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
            SubmissionOwners owned={0};XrXirValue value={0};
            CHECK(reads_drive(&owned,&limits,family,&value)==(delta<0 ? XR_SOURCE_BUDGET : XR_SOURCE_OK));
            if(delta<0 && owned.owner) {
                size_t attempts=runtime_attempts;XrCompileResourceStats before={0},after={0};
                CHECK(xr_compile_resources_stats(owned.resources,&before)==XR_COMPILE_RESOURCE_OK);
                CHECK(xr_source_submission_read(owned.owner,"value",&value)==XR_SOURCE_BUDGET && runtime_attempts==attempts);
                CHECK(xr_compile_resources_stats(owned.resources,&after)==XR_COMPILE_RESOURCE_OK &&
                    before.work==after.work && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
            }
            xr_xir_value_drop(&value);submission_close(&owned);
        }
        printf("Actual birth reader family%u observed=%zu swept=%zu; compiler three axes exact/minus1/plus1; sticky retry; physical0\n",family,sites,faults ? sites : 0);
    }
}
static void reads_history_inputs(void) {
    const char *cases[]={"var number:i64=11\n","const text=\"owned\"\n","const values=[11,42]\n",
        "var root:i64=11\nconst callback=fn()->i64{return root}\n","print(17)\n","const value:i64=5\nprint(17)\n",
        "var value:i64=11\nconst text=\"owned\"\nconst values=[11,42]\nconst callback=fn()->i64{return value}\nprint(17)\n",
        "var empty=()\n","const empty=()\n","const callback=fn()->i64{return 42}\n"};
    unsigned failures=0;
    for(unsigned family=0;family<sizeof(cases)/sizeof(*cases);++family) {
        SubmissionOwners owned={0};CHECK(submission_open(&owned,&generous)==XR_SOURCE_OK);
        CHECK(reads_publish(&owned,cases[family])==XR_SOURCE_OK);
        XrXirSourceText text={NULL,"var future:bool=true\n",21};XrXirSourceDiagnostic diagnostic={0};
        text.length=strlen(text.text);
        XrSourceStatus status=xr_source_submission_prepare(owned.owner,&text,&diagnostic);
        fprintf(stderr,"history input family%u status=%u source=%u line=%d message=%s\n",family,status,diagnostic.status,diagnostic.line,diagnostic.message);
        if(status!=XR_SOURCE_OK)++failures;
        else {XrXirCallStatus cleanup=XR_XIR_CALL_READY;CHECK(xr_source_submission_abort(owned.owner,&cleanup)==XR_SOURCE_OK);}
        submission_close(&owned);
    }
    CHECK(!failures);
}
static void reads_history_admission(void) {
    SubmissionOwners owned={0};CHECK(submission_open(&owned,&generous)==XR_SOURCE_OK);
    CHECK(reads_publish(&owned,"var value:i64=11\nexport fn read()->i64{return value}\nprint(17)\n")==XR_SOURCE_OK);
    CHECK(reads_publish(&owned,"var future:bool=true\n")==XR_SOURCE_OK);
    CHECK(owned.outputs==1);
    reads_scalar(&owned,"value",XR_XIR_I64,11);reads_scalar(&owned,"future",XR_XIR_BOOL,1);
    const char *source="const answer=read()\n";
    XrXirSourceText text={NULL,source,strlen(source)};XrXirSourceDiagnostic diagnostic={0};
    XrSourceStatus status=xr_source_submission_prepare(owned.owner,&text,&diagnostic);
    if(status!=XR_SOURCE_OK)
        fprintf(stderr,"active history status=%u line=%d message=%s\n",status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_SOURCE_OK && submission_finish(&owned)==XR_XIR_CALL_RETURNED);
    CHECK(xr_source_submission_publish(owned.owner)==XR_SOURCE_OK);
    CHECK(owned.outputs==1);
    reads_scalar(&owned,"value",XR_XIR_I64,11);reads_scalar(&owned,"future",XR_XIR_BOOL,1);
    reads_scalar(&owned,"answer",XR_XIR_I64,11);
    submission_close(&owned);
    puts("Active history uses actual published birth storage without initializer replay; original value11 and output17 once; physical0");
}
static void reads_history_receiving(void) {
    SubmissionOwners owned={0};CHECK(submission_open(&owned,&generous)==XR_SOURCE_OK);
    CHECK(reads_publish(&owned,"const value=41\nconst text=\"birth\"\n"
        "export fn old()->i64{return value+1}\nexport fn message()->string{return text}\n"
        "export fn fail(){assert(false,\"unpublished\")}\nprint(17)\n")==XR_SOURCE_OK);
    const XrXirInstanceBinding *old=NULL;
    CHECK(xr_source_submission_lookup_runtime(owned.owner,"old",&old)==XR_SOURCE_OK);
    CHECK(reads_publish(&owned,"const value=\"shadow\"\nexport fn answer()->i64{return old()}\n"
        "assert(message()==\"birth\")\n")==XR_SOURCE_OK);
    XrXirValue observed={0};
    CHECK(xr_source_submission_read(owned.owner,"text",&observed)==XR_SOURCE_OK);xr_xir_value_drop(&observed);
    CHECK(reads_publish(&owned,"const value:bool=true\nconst total=old()\n"
        "assert(total==42)\nassert(message()==\"birth\")\n")==XR_SOURCE_OK);
    CHECK(owned.outputs==1);reads_scalar(&owned,"value",XR_XIR_BOOL,1);reads_scalar(&owned,"total",XR_XIR_I64,42);
    CHECK(submission_text(&owned,"const bad=missing\n")==XR_SOURCE_REJECTED);
    CHECK(submission_text(&owned,"const value:i64=99\nfail()\n")==XR_SOURCE_OK);
    CHECK(submission_finish(&owned)==XR_XIR_CALL_ASSERTION);
    XrXirCallResult panic={0};
    CHECK(xr_source_submission_take(owned.owner,&panic)==XR_SOURCE_OK && panic.status==XR_XIR_CALL_ASSERTION);
    CHECK(xr_source_submission_publish(owned.owner)==XR_SOURCE_BAD_STATE);
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    CHECK(xr_source_submission_abort(owned.owner,&cleanup)==XR_SOURCE_OK);
    reads_scalar(&owned,"value",XR_XIR_BOOL,1);
    CHECK(reads_publish(&owned,"const value:i64=7\nconst after=old()\n"
        "assert(after==42)\nassert(message()==\"birth\")\n")==XR_SOURCE_OK);
    reads_scalar(&owned,"after",XR_XIR_I64,42);reads_scalar(&owned,"value",XR_XIR_I64,7);
    XrXirSourceGenerationBinding lexical={0};const XrXirInstanceBinding *physical=NULL;
    XrXirInstanceBindingView view={0};
    CHECK(xr_source_submission_lookup(owned.owner,"value",&lexical)==XR_SOURCE_OK && lexical.generation==6);
    XrXirSourceGenerationView source_view={0};
    CHECK(xr_xir_compile_source_generation_view(owned.owner->source,lexical.generation,&source_view)==XR_XIR_OK);
    const XrXirDeclarations *declarations=xr_xir_compile_artifact_module(source_view.result->checked)->declarations;
    for(uint32_t module=0;module<declarations->module_count;++module) {
        const XrXirSourceModule *entry=&declarations->modules[module];char *identity=NULL;
        CHECK(xr_compile_state_calloc(owned.state,(size_t)entry->name_length+1,1,(void **)&identity)==XR_COMPILE_RESOURCE_OK);
        memcpy(identity,entry->name,entry->name_length);
        CHECK(xr_module_identity_valid(identity,NULL));xr_compile_state_free(identity);
    }
    CHECK(xr_source_submission_lookup_runtime(owned.owner,"value",&physical)==XR_SOURCE_OK &&
        xr_xir_instance_binding_view(owned.owner->runtime,physical,&view)==XR_XIR_CALL_READY && view.generation==5);
    CHECK(xr_xir_instance_binding_start(owned.owner->runtime,old,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus terminal;
    do {terminal=xr_xir_instance_poll_bounded(owned.owner->runtime,32).outcome.status;}while(terminal==XR_XIR_CALL_READY);
    XrXirValue answer={0},text={0};CHECK(terminal==XR_XIR_CALL_RETURNED &&
        xr_xir_instance_take_result(owned.owner->runtime,&answer)==XR_XIR_CALL_RETURNED);
    CHECK(answer.type==XR_XIR_I64 && answer.payload==42);xr_xir_value_drop(&answer);
    CHECK(xr_source_submission_read(owned.owner,"text",&text)==XR_SOURCE_OK && owned.outputs==1);
    CHECK(xr_source_submission_drop(&owned.owner,&cleanup)==XR_SOURCE_OK && !owned.owner);
    xr_compile_state_release(owned.state);owned.state=NULL;xr_compile_resources_release(owned.resources);owned.resources=NULL;
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&text,&bytes,&length) && length==5 && !memcmp(bytes,"birth",length));
    CHECK(xr_xir_string_view(&panic.panic.message,&bytes,&length) && length==11 && !memcmp(bytes,"unpublished",length));
    xr_xir_value_drop(&text);xr_xir_call_result_drop(&panic);submission_close(&owned);
    puts("Actual paired history: four publications, old42 through String/Bool shadows, output17 once, rejected Source and historical Panic abort, Source6/runtime5, original birth entry, escaped String after owner death; physical0");
}
#include "source_binding_history_array.h"
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && (!strcmp(argv[1],"--resources") || !strcmp(argv[1],"--budget") ||
        !strcmp(argv[1],"--history-inputs") || !strcmp(argv[1],"--history-admission") || !strcmp(argv[1],"--history-receiving") ||
        !strcmp(argv[1],"--history-array-receiving"))));
    if(argc==1)reads_normal();else if(!strcmp(argv[1],"--history-inputs"))reads_history_inputs();
    else if(!strcmp(argv[1],"--history-admission"))reads_history_admission();
    else if(!strcmp(argv[1],"--history-receiving"))reads_history_receiving();
    else if(!strcmp(argv[1],"--history-array-receiving"))reads_history_array_receiving();
    else reads_resources(!strcmp(argv[1],"--resources"));return 0;
}
