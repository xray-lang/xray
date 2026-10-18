/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_binding_history_callable.c - Non-Sendable history with real birth storage
 */
#define main existing_submission_regressions
#include "test_source_submission.c"
#undef main
#include "source_binding_history_support.h"
#include "program/xr_xir_source_product.h"

static void callable_birth_case(unsigned capture_kind) {
    bool captured=capture_kind!=0;
    SubmissionOwners owned={0};CHECK(submission_open(&owned,&generous)==XR_SOURCE_OK);
    CHECK(reads_publish(&owned,capture_kind==2 ?
        "fn make()->fn()->i64{var held=40;return fn()->i64{held+=1;return held}}\n"
        "const callback=make()\nprint(17)\n" : captured ?
        "var state=40\nconst callback=fn()->i64{state+=1;return state}\nprint(17)\n" :
        "const callback=fn()->i64{return 42}\nprint(17)\n")==XR_SOURCE_OK);
    CHECK(reads_publish(&owned,"fn invoke(action:fn()->i64)->i64{return action()}\n"
        "const widened:fn()->i64=callback\nconst answer=invoke(widened)\n"
        "export fn current()->i64{return answer}\n")==XR_SOURCE_OK);
    reads_scalar(&owned,"answer",XR_XIR_I64,captured ? 41 : 42);CHECK(owned.outputs==1);
    const XrXirInstanceBinding *current=NULL;
    CHECK(xr_source_submission_lookup_runtime(owned.owner,"current",&current)==XR_SOURCE_OK);
    XrXirInstanceBindingView view={0};
    CHECK(xr_xir_instance_binding_view(owned.owner->runtime,current,&view)==XR_XIR_CALL_READY);
    XrXirProgram *program=(XrXirProgram *)view.program;CHECK(program && program->requires_birth_storage);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *fresh=NULL;CHECK(xr_xir_instance_new(program,&config,&fresh)==XR_XIR_CALL_BAD_STATE && !fresh);
    CHECK(xr_xir_instance_generation_new(&config,&fresh)==XR_XIR_CALL_READY);
    uint64_t serial=0;
    CHECK(xr_xir_instance_generation_prepare(fresh,program,&serial)==XR_XIR_CALL_BAD_STATE && !serial);
    CHECK(xr_xir_instance_free(fresh)==XR_XIR_CALL_READY);

    XrXirSourceGenerationBinding selected={0};
    CHECK(xr_source_submission_lookup(owned.owner,"current",&selected)==XR_SOURCE_OK);
    XrXirSourceGenerationView source={0};
    CHECK(xr_xir_compile_source_generation_view(owned.owner->source,selected.generation,&source)==XR_XIR_OK);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(source.result->checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&owned.owner->target,&lowered,NULL)==XR_XIR_OK);
    XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_lowered_public_admit(lowered,&diagnostic)==XR_XIR_BAD_STRUCTURE &&
        strstr(diagnostic.message,"published birth storage"));
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);

    XrXirValue saved={0};CHECK(xr_source_submission_read(owned.owner,"callback",&saved)==XR_SOURCE_OK);
    const char *worker="fn worker()->i64{return callback()}\nconst task=go worker()\n";
    XrXirSourceText rejected={NULL,worker,strlen(worker)};
    CHECK(xr_source_submission_prepare(owned.owner,&rejected,&diagnostic)==XR_SOURCE_REJECTED &&
        strstr(diagnostic.message,"GO target requires"));
    CHECK(owned.outputs==1);
    CHECK(reads_publish(&owned,"var state=\"shadow\"\nconst result=callback()\n")==XR_SOURCE_OK);
    reads_scalar(&owned,"result",XR_XIR_I64,42);
    CHECK(xr_xir_instance_start_function(owned.owner->runtime,&saved,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus terminal;
    do {terminal=xr_xir_instance_poll_bounded(owned.owner->runtime,8).outcome.status;}while(terminal==XR_XIR_CALL_READY);
    XrXirValue answer={0};CHECK(terminal==XR_XIR_CALL_RETURNED &&
        xr_xir_instance_take_result(owned.owner->runtime,&answer)==XR_XIR_CALL_RETURNED);
    CHECK(answer.type==XR_XIR_I64 && answer.payload==(captured ? 43u : 42u));xr_xir_value_drop(&answer);
    XrXirValue shadow={0};CHECK(xr_source_submission_read(owned.owner,"state",&shadow)==XR_SOURCE_OK);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&shadow,&bytes,&length) &&
        length==6 && !memcmp(bytes,"shadow",6));xr_xir_value_drop(&shadow);
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    CHECK(xr_source_submission_drop(&owned.owner,&cleanup)==XR_SOURCE_OK && !owned.owner);
    CHECK(xr_xir_value_valid(&saved));xr_xir_value_drop(&saved);submission_close(&owned);
    puts(capture_kind==2 ? "Actual owned Cell capture history:41/42/43, higher-order call, original gate and capture, no replay, worker/fresh/unbound refusal, physical0" :
        captured ? "Actual captured history:41/42/43 through String shadow, old function owner, no replay, worker/fresh/unbound refusal, physical0" :
        "Actual captureless history:42 across generations, no false Sendable grant, worker/fresh/unbound refusal, physical0");
}
static void generic_callable_birth(void) {
    SubmissionOwners owned={0};CHECK(submission_open(&owned,&generous)==XR_SOURCE_OK);
    CHECK(reads_publish(&owned,"fn identity<T>(value:T)->T{return value}\n"
        "const callback=identity<i64>\nprint(17)\n")==XR_SOURCE_OK);
    XrXirValue saved={0};CHECK(xr_source_submission_read(owned.owner,"callback",&saved)==XR_SOURCE_OK);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&saved);CHECK(binding);
    void *birth=binding->owner;
    CHECK(reads_publish(&owned,"const answer=callback(42)\n")==XR_SOURCE_OK);
    reads_scalar(&owned,"answer",XR_XIR_I64,42);CHECK(owned.outputs==1);
    XrXirValue read={0};CHECK(xr_source_submission_read(owned.owner,"callback",&read)==XR_SOURCE_OK);
    CHECK(xr_xir_function_binding(&read)->owner==birth);xr_xir_value_drop(&read);
    const char *worker="fn worker()->i64{return callback(1)}\nconst task=go worker()\n";
    XrXirSourceText text={NULL,worker,strlen(worker)};XrXirSourceDiagnostic diagnostic={0};
    CHECK(xr_source_submission_prepare(owned.owner,&text,&diagnostic)==XR_SOURCE_REJECTED &&
        strstr(diagnostic.message,"GO target requires"));
    CHECK(reads_publish(&owned,"const callback=false\nfn identity(value:string)->string{return value}\n")==XR_SOURCE_OK);
    reads_scalar(&owned,"callback",XR_XIR_BOOL,0);
    XrXirValue argument={XR_XIR_I64,0,42},answer={0};
    CHECK(xr_xir_instance_start_function(owned.owner->runtime,&saved,&argument,1)==XR_XIR_CALL_READY);
    XrXirCallStatus terminal;
    do {terminal=xr_xir_instance_poll_bounded(owned.owner->runtime,8).outcome.status;}while(terminal==XR_XIR_CALL_READY);
    CHECK(terminal==XR_XIR_CALL_RETURNED &&
        xr_xir_instance_take_result(owned.owner->runtime,&answer)==XR_XIR_CALL_RETURNED &&
        answer.type==XR_XIR_I64 && answer.payload==42);xr_xir_value_drop(&answer);
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    CHECK(xr_source_submission_drop(&owned.owner,&cleanup)==XR_SOURCE_OK && xr_xir_value_valid(&saved));
    xr_xir_value_drop(&saved);submission_close(&owned);
    puts("Actual generic I64 function value:42 through Bool and String-function shadows, original gate, no replay or worker grant, physical0");
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--generic")) {generic_callable_birth();return 0;}
    CHECK(argc==1);
    callable_birth_case(0);callable_birth_case(1);callable_birth_case(2);return 0;
}
