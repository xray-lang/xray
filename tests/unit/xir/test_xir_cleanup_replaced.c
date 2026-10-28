/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cleanup_replaced.c - Owned cleanup outcomes after producer retirement
 */
#include "xir/xxir_instance_activation.h"
#if defined(XR_CLEANUP_VALUE_OBSERVER)
#include "base/xmalloc.h"
static size_t resource_index=SIZE_MAX,resource_calls,resource_live;
static bool resource_active;
static bool resource_refuse(void) {return resource_active && resource_calls++==resource_index;}
static void *resource_malloc(size_t size) {if(resource_refuse())return NULL;void *p=xr_malloc(size);if(p)++resource_live;return p;}
static void *resource_calloc(size_t count,size_t size) {if(resource_refuse())return NULL;void *p=xr_calloc(count,size);if(p)++resource_live;return p;}
static void *resource_realloc(void *p,size_t size) {if(resource_refuse())return NULL;bool empty=!p;void *q=xr_realloc(p,size);if(q && empty)++resource_live;return q;}
static void resource_free(void *p) {if(p){if(!resource_live)abort();--resource_live;}xr_free(p);}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(size) resource_malloc(size)
#define xr_calloc(count,size) resource_calloc(count,size)
#define xr_realloc(p,size) resource_realloc(p,size)
#define xr_free(p) resource_free(p)
#include "xir/xxir_value.c"
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(size) xr_malloc_raw(size)
#define xr_calloc(count,size) xr_calloc_raw(count,size)
#define xr_realloc(p,size) xr_realloc_raw(p,size)
#define xr_free(p) xr_free_raw(p)
#endif
#if defined(XR_CLEANUP_CALL_OBSERVER)
/* Fault-only reception observations reuse the actual Call implementation.
 * Canonical product targets do not compile this replacement translation unit. */
#define xr_xir_call_complete_task_wait xir_cleanup_actual_complete_task_wait
#define xr_xir_call_poll_bounded xir_cleanup_actual_call_poll_bounded
#include "xir/xxir_call.c"
#undef xr_xir_call_complete_task_wait
#undef xr_xir_call_poll_bounded
#endif
#if defined(XR_CLEANUP_HOST_OBSERVER)
#define xr_xir_host_wait_begin xir_cleanup_injected_wait_begin
#include "execution/xr_xir_host_execution.c"
#undef xr_xir_host_wait_begin
#endif
#include "xray_source_entry.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_call_internal.h"
#include "xir/xxir_task_internal.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_output.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_error.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_format.h"
#include "xir/xxir_compile_domain.h"
#include "execution/xr_xir_host_cli.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) {fprintf(stderr,"cleanup %d: %s\n",__LINE__,#c);exit(1);} } while (0)
typedef struct Output { unsigned char bytes[8192];size_t size; } Output;
static const char owned_stdout[]="true\ntrue\n420\ntrue\n420\n42\ntrue\n446\n";
static XrSourceWriteStatus output(void *context,XrSourceOutputStream stream,const char *text,size_t length) {
    Output *out=context;(void)stream;CHECK(length<=sizeof(out->bytes)-out->size);
    memcpy(out->bytes+out->size,text,length);out->size+=length;return XR_SOURCE_WRITE_OK;
}
static int sink(void *context,const void *text,size_t length) {
    return output(context,XR_SOURCE_STDERR,text,length)==XR_SOURCE_WRITE_OK;
}
static char *read_fixture(const char *name,size_t *length) {
    char path[2048];CHECK(snprintf(path,sizeof(path),"%s/cleanup_replaced_%s.xr",XR_CLEANUP_FIXTURES,name)>0);
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));long bytes=ftell(file);CHECK(bytes>0 && !fseek(file,0,SEEK_SET));
    char *text=xr_malloc((size_t)bytes+1);CHECK(text && fread(text,1,(size_t)bytes,file)==(size_t)bytes && !fclose(file));
    text[bytes]=0;*length=(size_t)bytes;return text;
}
static XrSourceArtifact *public_build(const char *name) {
    size_t length;char *text=read_fixture(name,&length);
    XrSourceCompileOptions options=xray_source_compile_options_default();options.target=XR_SOURCE_TARGET_VM;options.stdlib_path=XR_CLEANUP_STDLIB;
    XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"cleanup-replaced",text,length}};
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_build(&input,&options,&artifact,&failure,&diagnostic);
    if (diagnostic) {XrSourceDiagnosticRoot root={0};CHECK(xray_source_diagnostic_root(diagnostic,0,&root)==XR_SOURCE_OK);
        fprintf(stderr,"cleanup Source %u stage%u %d:%d %s\n",status,failure.stage,root.primary.line,root.primary.column,root.message);}
    xray_source_diagnostic_free(diagnostic);memset(text,0xa5,length);xr_free(text);CHECK(status==XR_SOURCE_OK && artifact);return artifact;
}
static XrSourceExecution *execution(void) {
    XrSourceExecutionOptions options=xray_source_execution_options_default();XrSourceExecution *owner=NULL;
    CHECK(xray_source_execution_new(&options,&owner)==XR_SOURCE_OK && owner);return owner;
}
static XrSourceCallOptions options(Output *out) {
    XrSourceCallOptions result;CHECK(xray_source_call_options_init(&result,sizeof(result))==XR_SOURCE_OK);
    result.write=output;result.context=out;return result;
}
static XrSourceResult *invoke(XrSourceExecution *owner,XrSourceArtifact *program,const char *name,Output *out) {
    XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
    invocation.name=name;invocation.name_length=strlen(name);XrSourceCallOptions config=options(out);
    XrSourceResult *result=NULL;XrSourceEvent failure={0};
    CHECK(xray_source_invoke(owner,program,&invocation,&config,&result,&failure)==XR_SOURCE_OK && result);return result;
}
static void public_sources(void) {
    const char *names[]={"plain","optional","owned"};
    for (size_t n=0;n<3;++n) {
        XrSourceArtifact *program=public_build(names[n]);XrSourceExecution *owner=execution();Output out={0};
        XrSourceCallOptions config=options(&out);XrSourceResult *result=NULL;XrSourceEvent failure={0};
        CHECK(xray_source_artifact_execute(owner,program,&config,&result,&failure)==XR_SOURCE_OK && result);
        xray_source_artifact_free(program);xray_source_execution_free(owner);
        XrSourceEvent event;CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
        const char *expected=n==2?owned_stdout:"420\n";
        CHECK(out.size==strlen(expected) && !memcmp(out.bytes,expected,out.size));xray_source_result_free(result);
    }
    puts("cleanup Source: original plain/optional420 and owned Some/None/RETURN/local catch/446 passed");
}
static bool contains(const Output *out,const char *text,size_t length) {
    for (size_t i=0;length<=out->size-i;++i) if (!memcmp(out->bytes+i,text,length)) return true;
    return false;
}
static void public_fatal(void) {
    static const char expected[]="[Fatal Cleanup] E0421: modulo by zero\n[Prior Panic] E0420: division by zero\n";
    XrSourceArtifact *program=public_build("owned");XrSourceExecution *owner=execution();
    const char *names[]={"fatalPanic","fatal446","fatalTask"};XrSourceResult *retained[3]={0};
    for (size_t n=0;n<3;++n) {
        Output out={0};XrSourceResult *result=invoke(owner,program,names[n],&out);XrSourceEvent event;
        CHECK(out.size==sizeof(owned_stdout)-1 && !memcmp(out.bytes,owned_stdout,out.size));
        CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_CLEANUP_FAILED &&
            event.engine_status==XR_XIR_CALL_CLEANUP_FAILED);
        CHECK(xray_source_result_copy(result,&retained[n])==XR_SOURCE_OK);xray_source_result_free(result);
        /* The same embedding execution owner can create another fresh request. */
        out=(Output){0};result=invoke(owner,program,"afterFailure",&out);int64_t value=0;
        CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==42);xray_source_result_free(result);
    }
    xray_source_artifact_free(program);xray_source_execution_free(owner);
    for (size_t n=0;n<3;++n) {
        Output first={0},copy={0};CHECK(xray_source_result_format(retained[n],output,&first)==XR_SOURCE_OK);
        CHECK(xray_source_result_format(retained[n],output,&copy)==XR_SOURCE_OK && copy.size==first.size && !memcmp(copy.bytes,first.bytes,first.size));
        if (n!=1) CHECK(first.size==sizeof(expected)-1 && !memcmp(first.bytes,expected,first.size));
        else {static const char raw[]="new é\0界";CHECK(contains(&first,raw,sizeof(raw)-1) && contains(&first,"[Prior Panic] E0446",18));}
        xray_source_result_free(retained[n]);
    }
    puts("cleanup fatal: direct/446/Task own primary+prior, retired opaque results and next host requests passed");
}
static void public_cancel(void) {
    XrSourceArtifact *program=public_build("owned");XrSourceExecution *owner=execution();Output out={0};
    XrSourceCallOptions config=options(&out);XrSourceInvocation request;CHECK(xray_source_invocation_init(&request,sizeof(request))==XR_SOURCE_OK);
    request.name="cancellable";request.name_length=11;XrSourceCall *call=NULL;
    CHECK(xray_source_invoke_begin(owner,program,&request,&config,&call)==XR_SOURCE_OK);
    XrSourceEvent event={0};unsigned polls=0;
    do {CHECK(++polls<100000 && xray_source_call_poll(call,1,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);}
    while (!contains(&out,"armed\n",6));
    CHECK(xray_source_call_cancel(call)==XR_SOURCE_OK);
    do {CHECK(++polls<100000 && xray_source_call_poll(call,1,&event)==XR_SOURCE_OK);} while (event.state==XR_SOURCE_RUN_READY);
    CHECK(event.state==XR_SOURCE_RUN_CLEANUP_FAILED);XrSourceResult *result=NULL;
    CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    XrSourceEvent cleanup={0};CHECK(xray_source_call_drop(&call,&cleanup)==XR_SOURCE_OK && !call && cleanup.state==XR_SOURCE_RUN_CLEANUP_FAILED);
    xray_source_artifact_free(program);xray_source_execution_free(owner);Output formatted={0};
    static const char expected[]="[Fatal Cleanup] E0420: division by zero\n[Prior Cancellation]\n";
    CHECK(xray_source_result_format(result,output,&formatted)==XR_SOURCE_OK && formatted.size==sizeof(expected)-1 && !memcmp(formatted.bytes,expected,formatted.size));
    xray_source_result_free(result);puts("cleanup cancellation: real registered Source defer, owned fatal and late result passed");
}
#if defined(XR_CLEANUP_HOST_OBSERVER)
static bool host_wait_inject;
static unsigned host_wait_failures;
XR_FUNC XrXirHostWaitStatus xr_xir_host_wait_begin(XrXirHostWait *wait,const XrXirWaitRequest *request);
XR_FUNC XrXirHostWaitStatus xir_cleanup_injected_wait_begin(XrXirHostWait *wait,const XrXirWaitRequest *request) {
    if (host_wait_inject) {
        CHECK(request->kind==XR_XIR_WAIT_YIELD && !request->reserved && !request->after_ms &&
            !request->subject && !request->generation && !request->ticket);
        host_wait_inject=false;++host_wait_failures;return XR_XIR_HOST_WAIT_FAILED;
    }
    return xr_xir_host_wait_begin(wait,request);
}
static void host_wait_failure(void) {
    XrSourceArtifact *program=public_build("owned");XrSourceExecution *owner=execution();Output out={0};
    host_wait_inject=true;host_wait_failures=0;
    XrSourceResult *result=invoke(owner,program,"hostWaitFailure",&out);XrSourceEvent event={0};
    CHECK(!host_wait_inject && host_wait_failures==1);
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.engine_status==XR_XIR_CALL_HOST_ERROR);
    CHECK(out.size==sizeof(owned_stdout)-1 && !memcmp(out.bytes,owned_stdout,out.size));
    xray_source_result_free(result);out=(Output){0};
    result=invoke(owner,program,"afterFailure",&out);int64_t value=0;
    CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==42);
    xray_source_result_free(result);xray_source_artifact_free(program);xray_source_execution_free(owner);
    puts("hard host wait failure preserves HOST_ERROR, closes without guest cleanup and permits a fresh request");
}
#endif
typedef struct Prepared { XrXirProgram *program;XrCompileResources *resources;uint64_t baseline; } Prepared;
static Prepared prepare_text(const char *text,size_t length) {
    Prepared ready={0};XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY,.profile_allocations=true};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&ready.resources)==XR_COMPILE_RESOURCE_OK);context.resources=ready.resources;
    XrCompileResourceStats initial;CHECK(xr_compile_resources_stats(ready.resources,&initial)==XR_COMPILE_RESOURCE_OK);ready.baseline=initial.held_bytes;
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(ready.resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"cleanup-owned",NULL};
    XrXirSourceProductRequest request={{session,NULL,&authority,&context,XR_CLEANUP_STDLIB,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceText input={NULL,text,length};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    CHECK(xr_xir_compile_source_product_build_text(&request,&input,&product,&diagnostic)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_vm_take(product,&ready.program)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session);return ready;
}
static Prepared prepare(void) {
    size_t length;char *text=read_fixture("owned",&length);Prepared ready=prepare_text(text,length);
    memset(text,0xa5,length);xr_free(text);return ready;
}
static uint32_t named(const XrXirProgram *program,const char *name) {
    const XrXirModule *module=xr_xir_compile_artifact_module(program->permissions->input->artifact);
    for (uint32_t i=0;i<module->function_count;++i) if (module->functions[i].name_length==strlen(name) &&
        !memcmp(module->functions[i].name,name,strlen(name))) return i;
    CHECK(false);return UINT32_MAX;
}
static XrXirOutputStatus raw_bytes(void *context,XrXirOutputStream stream,const char *text,size_t length) {
    return output(context,(XrSourceOutputStream)stream,text,length)==XR_SOURCE_WRITE_OK?XR_XIR_OUTPUT_OK:XR_XIR_OUTPUT_ERROR;
}
typedef struct RawOutput {Output output;XrXirOutputSink sink;} RawOutput;
static XrXirOutputStatus raw_group(void *context,const XrXirOutputGroup *group) {
    return xr_xir_output_render(&((RawOutput *)context)->sink,group);
}
static XrXirCallResult raw_call(XrXirProgram *program,const char *name) {
    RawOutput out={0};out.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,raw_bytes,&out.output,sizeof(out.output.bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,raw_group,&out};XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,named(program,name),NULL,0)==XR_XIR_CALL_READY);
    XrXirCallResult observed=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome,owned={0};
    CHECK(xr_xir_call_result_copy(&observed,&owned)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_free(instance)==(owned.status==XR_XIR_CALL_CLEANUP_FAILED?XR_XIR_CALL_CLEANUP_FAILED:XR_XIR_CALL_READY));
    return owned;
}
static void text_field(const XrXirValue *error,const char *expected,size_t length,int64_t number) {
    XrXirValue concrete={0};CHECK(xr_xir_error_borrow(error,&concrete));uint32_t variant=UINT32_MAX;
    CHECK(xr_xir_enum_variant(&concrete,&variant)==XR_XIR_VALUE_OK && variant==0);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(error),domain,NULL,NULL,100000,100000};XrXirValue text={0},value={0};
    CHECK(xr_xir_enum_get(&concrete,0,0,&admission,&text)==XR_XIR_VALUE_OK);
    const char *bytes=NULL;size_t size=0;CHECK(xr_xir_string_view(&text,&bytes,&size) && size==length && !memcmp(bytes,expected,length));
    CHECK(xr_xir_enum_get(&concrete,0,1,&admission,&value)==XR_XIR_VALUE_OK && value.type==XR_XIR_I64 && value.payload==number);
    xr_xir_value_drop(&text);xr_xir_value_drop(&value);xr_xir_domain_drop(domain);
}
static void owned_graph(void) {
    Prepared ready=prepare();XrXirCallResult single=raw_call(ready.program,"single446"),fatal=raw_call(ready.program,"fatal446");
    CHECK(single.status==XR_XIR_CALL_RUNTIME_PANIC && single.panic.detail.code==446 && single.prior_status==XR_XIR_CALL_READY);
    CHECK(fatal.status==XR_XIR_CALL_CLEANUP_FAILED && fatal.panic.detail.code==446 && fatal.prior_panic.detail.code==446);
    Output formatted={0};CHECK(xr_xir_host_report_result(&single,(XrValueFormatSink){&formatted,sink},false)==XR_XIR_HOST_REPORT_OK);
    CHECK(contains(&formatted,"[Replaced Error]",16));
    xr_xir_compile_program_drop(ready.program);
    static const char old[]="old é\0界",next[]="new é\0界",prior[]="prior é\0界";
    text_field(&single.panic.error,next,sizeof(next)-1,31);text_field(&single.panic.replaced_error,old,sizeof(old)-1,23);
    text_field(&fatal.panic.error,next,sizeof(next)-1,31);text_field(&fatal.prior_panic.error,prior,sizeof(prior)-1,47);
    XrXirCallResult copy={0};CHECK(xr_xir_call_result_copy(&single,&copy)==XR_XIR_VALUE_OK);xr_xir_call_result_drop(&single);
    text_field(&copy.panic.replaced_error,old,sizeof(old)-1,23);xr_xir_call_result_drop(&copy);xr_xir_call_result_drop(&fatal);
    XrCompileResourceStats stats;CHECK(xr_compile_resources_stats(ready.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"owned final held=%llu allocations=%llu\n",(unsigned long long)stats.held_bytes,(unsigned long long)stats.allocation_count);
    CHECK(stats.held_bytes==ready.baseline);
    xr_compile_resources_release(ready.resources);puts("cleanup raw owned graphs: dual Error, prior panic, copied result and producer/arena retirement passed");
}
static void sticky_host_guard(void) {
    static const char text[]="enum InitFailure { Failed { text: string } }\n"
        "fn fail() throws(InitFailure) -> i64 { throw InitFailure.Failed { text: \"sticky é\\0界\" } }\n"
        "const neverPublished = try fail()\nexport fn answer() -> i64 { return 42 }\n";
    Prepared ready=prepare_text(text,sizeof(text)-1);XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(ready.program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,named(ready.program,"answer"),NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult original=xr_xir_instance_poll_bounded(instance,UINT64_MAX),untouched={.epoch=91};
    CHECK(original.outcome.status==XR_XIR_CALL_THROWN && original.outcome.value.payload);
    CHECK(xir_instance_host_failure(instance,&untouched)==XR_XIR_CALL_BAD_STATE && untouched.epoch==91 && xr_xir_call_result_empty(&untouched.outcome));
    XrXirInstanceResult again=xr_xir_instance_poll_bounded(instance,1);
    CHECK(again.outcome.status==XR_XIR_CALL_THROWN && again.outcome.value.type==original.outcome.value.type &&
        again.outcome.value.payload==original.outcome.value.payload && again.epoch==original.epoch);
    XrXirCallResult owned={0};CHECK(xr_xir_instance_copy_failure(instance,&owned)==XR_XIR_CALL_THROWN);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(ready.program);
    XrXirValueAdmission admission={xr_xir_value_arena(&owned.value),NULL,NULL,NULL,100000,100000};
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);admission.domain=domain;
    XrXirValue field={0};CHECK(xr_xir_enum_get(&owned.value,0,0,&admission,&field)==XR_XIR_VALUE_OK);
    static const char expected[]="sticky é\0界";const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&field,&bytes,&length) && length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    xr_xir_value_drop(&field);xr_xir_domain_drop(domain);xr_xir_call_result_drop(&owned);
    XrCompileResourceStats stats;CHECK(xr_compile_resources_stats(ready.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==ready.baseline);
    xr_compile_resources_release(ready.resources);puts("hard host ingress rejects sticky initialization Error without touching its owner or observation");
}
typedef struct ReentryContext {XrXirTaskExecutor *executor;unsigned releases,guest;} ReentryContext;
static XrXirAction reentry_resume(XrXirCallView *view) {
    ++((ReentryContext *)view->environment)->guest;return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
}
static void reentry_release(XrXirCallView *view,XrXirCallStatus reason) {
    ReentryContext *context=(ReentryContext *)view->environment;(void)reason;++context->releases;
    CHECK(xr_xir_task_executor_poll(context->executor,1)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_task_executor_stop(context->executor)==XR_XIR_CALL_BUSY);
    CHECK(xr_xir_task_executor_drain(context->executor,XR_XIR_CALL_HOST_ERROR)==XR_XIR_CALL_BUSY);
}
static void hard_close_reentry(void) {
    Prepared ready=prepare();ReentryContext context={0};XrXirDomain *domain=NULL;
    const XrXirDomainBudgetControls controls={1048576,1048576,1000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&controls,&domain)==XR_XIR_VALUE_OK);
    XrXirCallBudget budget={.byte_limit=1048576,.requested_limit=1048576,.resume_limit=100000,.work_domain=domain};
    XrXirCallEntry entry={.abi_version=XR_XIR_CALL_ABI_VERSION,.result=XR_XIR_I64,.resume=reentry_resume,
        .release=reentry_release,.environment=&context};
    XrXirValueAdmission admission={ready.program->arena,domain,NULL,NULL,1000000,1048576};
    XrXirTaskExecutorConfig executor={.program=ready.program,.domain=domain,.task_arena=ready.program->arena,
        .call_limit=1048576,.poll_limit=100000,.requested_value_limit=1048576,.requested_call_limit=1048576,
        .work_limit=1000000,.depth_limit=64,.budget=&budget,.entries=&entry,.entry_count=1,.admission=admission};
    CHECK(xr_xir_task_executor_new(&executor,&context.executor)==XR_XIR_CALL_READY);
    XrXirCallAccounting accounting={0};XrXirCallConfig config;CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=&entry;config.entry_count=1;config.admission=admission;config.byte_limit=1048576;config.poll_limit=100000;
    config.accounting=&accounting;
    XrXirCallRequest request={0};XrXirCall *call=NULL;
    CHECK(xr_xir_call_new_budgeted(&config,&request,&budget,&call)==XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_attach_root(context.executor,call,1)==XR_XIR_CALL_READY);
    CHECK(xr_xir_task_executor_drain(context.executor,XR_XIR_CALL_HOST_ERROR)==XR_XIR_CALL_HOST_ERROR);
    CHECK(context.releases==1 && !context.guest);
    CHECK(xr_xir_task_executor_poll_root(context.executor,1).status==XR_XIR_CALL_HOST_ERROR);
    CHECK(xr_xir_task_executor_free(context.executor,NULL)==XR_XIR_CALL_HOST_ERROR);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && !budget.live_bytes);xr_xir_domain_drop(domain);
    xr_xir_compile_program_drop(ready.program);XrCompileResourceStats stats;
    CHECK(xr_compile_resources_stats(ready.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==ready.baseline);
    xr_compile_resources_release(ready.resources);puts("hard close release hooks observe BUSY and cannot resume another guest activation");
}
static void task_sendable(void) {
    XrSourceArtifact *program=public_build("owned");XrSourceExecution *owner=execution();Output out={0};
    XrSourceResult *result=invoke(owner,program,"unsafeOrdinary",&out);int64_t value=0;
    CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==420);xray_source_result_free(result);
    out=(Output){0};result=invoke(owner,program,"unsafeTask",&out);XrSourceEvent event={0};
    CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.engine_status==XR_XIR_CALL_BAD_STATE);
    CHECK(out.size==sizeof(owned_stdout)-1 && !memcmp(out.bytes,owned_stdout,out.size));xray_source_result_free(result);
    out=(Output){0};result=invoke(owner,program,"ordinaryTask",&out);
    CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==420);xray_source_result_free(result);
    xray_source_artifact_free(program);xray_source_execution_free(owner);
    puts("complete unsafe enum Empty: ordinary owning replacement accepted, actual Task publication rejected BAD_STATE; Sendable pair accepted");
}
typedef struct LateContext { XrXirValue error;bool admitted,received,swallow;unsigned releases; } LateContext;
static XrXirAction late_root(XrXirCallView *view) {
    LateContext *context=(LateContext *)view->environment;uint32_t *state=view->state;
    if (view->phase==XR_XIR_CALL_EXIT) {
        if (*state==1) { *state=2;return (XrXirAction){.kind=XR_XIR_ACTION_CALL,.callee=1,.flags=XR_XIR_ACTION_CLEANUP}; }
        CHECK(xr_xir_call_cleanup_frontier(view,0)==XR_XIR_CALL_READY);
        return (XrXirAction){.kind=XR_XIR_ACTION_EXIT_DONE};
    }
    if (!*state) {
        *state=1;CHECK(xr_xir_call_cleanup_frontier(view,1)==XR_XIR_CALL_READY);
        return (XrXirAction){.kind=XR_XIR_ACTION_LEAVE,.value=context->error,.flags=XR_XIR_ACTION_LEAVE_ERROR};
    }
    CHECK(!context->received);context->received=true;CHECK(xr_xir_call_panic_status(view->inbox.status));
    if (context->swallow) return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
    return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,view->inbox.status},.panic=view->inbox.panic};
}
static XrXirAction late_cleanup(XrXirCallView *view) {
    (void)view;return xr_xir_call_numeric_fault(XR_XIR_RUN_DIVIDE_BY_ZERO,false);
}
static void late_release(XrXirCallView *view,XrXirCallStatus reason) {
    LateContext *context=(LateContext *)view->environment;(void)reason;++context->releases;
}
static void late_cleanup_release(XrXirCallView *view,XrXirCallStatus reason) {
    LateContext *context=(LateContext *)view->environment;
    CHECK(reason==XR_XIR_CALL_DIVIDE_BY_ZERO);++context->releases;context->admitted=true;
}
static void late_cancel_once(bool swallow) {
    /* The Error is produced by the complete Source fixture. The trusted native
     * callbacks isolate the same scope-exit handoff without lending frame state. */
    Prepared ready=prepare();XrXirCallResult source=raw_call(ready.program,"single446");
    LateContext context={.swallow=swallow};CHECK(xr_xir_error_borrow(&source.panic.replaced_error,&context.error));
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    XrXirCallEntry entries[2]={
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_I64,sizeof(uint32_t),late_root,late_release,&context,XR_XIR_ENTRY_EXIT,0},
        {XR_XIR_CALL_ABI_VERSION,NULL,0,XR_XIR_UNIT,0,late_cleanup,late_cleanup_release,&context,0,1}};
    XrXirCallAccounting accounting={0};XrXirCallConfig config;
    CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.entries=entries;config.entry_count=2;config.accounting=&accounting;
    config.admission=(XrXirValueAdmission){ready.program->arena,domain,NULL,NULL,100000,100000};
    XrXirCall *call=NULL;CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);
    XrXirCallResult observed={0};unsigned polls=0;
    do {CHECK(++polls<100);observed=xr_xir_call_poll_bounded(call,1);fprintf(stderr,"pre-admission step%u status%u released%u\n",polls,observed.status,context.releases);CHECK(observed.status==XR_XIR_CALL_READY);}
    while (!context.admitted);
    CHECK(context.releases==1 && !context.received);
    CHECK(xr_xir_call_request_cancel(call)==XR_XIR_CALL_CANCEL_REQUESTED);
    do {CHECK(++polls<100);observed=xr_xir_call_poll_bounded(call,1);} while(observed.status==XR_XIR_CALL_READY);
    XrXirCallStatus terminal=observed.status;XrXirCallResult owned={0};
    CHECK(xr_xir_call_take_outcome(call,&owned)==terminal);
    CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY && accounting.live_bytes==0 && context.releases==2);
    xr_xir_domain_drop(domain);xr_xir_compile_program_drop(ready.program);xr_xir_call_result_drop(&source);
    fprintf(stderr,"late after authenticated cleanup release: terminal=%u received=%u polls=%u\n",terminal,context.received,polls);
    CHECK(context.received && terminal==(swallow?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_DIVIDE_BY_ZERO));
    if (!swallow) {static const char old[]="old é\0界";CHECK(owned.panic.detail.code==420);
        text_field(&owned.panic.replaced_error,old,sizeof(old)-1,23);}
    xr_xir_call_result_drop(&owned);
    XrCompileResourceStats stats;CHECK(xr_compile_resources_stats(ready.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==ready.baseline);
    xr_compile_resources_release(ready.resources);puts("late cancellation after admitted panic preserves the owned inbox across quantum boundaries");
}
static void late_cancel(void) {late_cancel_once(false);late_cancel_once(true);}
#if defined(XR_CLEANUP_CALL_OBSERVER)
static unsigned receiver_checks;
static bool receiver_active;
static bool shutdown_observing,shutdown_admitted;
static bool shutdown_pair,shutdown_first,shutdown_fatal;
XR_FUNC XrXirCallResult xr_xir_call_poll_bounded(XrXirCall *call,uint64_t quantum) {
    XrXirCallResult result=xir_cleanup_actual_call_poll_bounded(call,quantum);
    if (shutdown_observing && call->top && !call->driving &&
        call->top->inbox.status==XR_XIR_CALL_DIVIDE_BY_ZERO && call->top->inbox.panic.replaced_error.type==XR_XIR_ERROR) {
        CHECK(xr_xir_call_result_valid(&call->top->inbox) && !call->top->inbox_dispatched);
        shutdown_observing=false;shutdown_admitted=true;
    }
    if (shutdown_pair && result.status==XR_XIR_CALL_DIVIDE_BY_ZERO && result.panic.replaced_error.type==XR_XIR_ERROR) {
        CHECK(xr_xir_call_result_valid(&result));shutdown_first=true;
        fprintf(stderr,"Source Task A completes its admitted owned ordinary panic before peer fatal\n");
    }
    if (shutdown_pair && result.status==XR_XIR_CALL_CLEANUP_FAILED) {
        CHECK(shutdown_first && xr_xir_call_result_valid(&result) && result.prior_status==XR_XIR_CALL_CANCELLED);
        shutdown_fatal=true;fprintf(stderr,"Source Task B completes genuine cancellation-cleanup fatal after A\n");
    }
    return result;
}
XR_FUNC XrXirCallStatus xr_xir_call_complete_task_wait(XrXirCall *call,const XrXirTaskWaitToken *token,XrXirCallResult *outcome) {
    if (receiver_active && outcome->status==XR_XIR_CALL_DIVIDE_BY_ZERO && outcome->panic.replaced_error.type==XR_XIR_ERROR) {
        receiver_active=false;
        XrXirValue concrete={0},fields[2]={{0}},duplicate={0},error={0};uint32_t variant=UINT32_MAX;
        CHECK(xr_xir_error_borrow(&outcome->panic.replaced_error,&concrete));
        CHECK(xr_xir_enum_variant(&concrete,&variant)==XR_XIR_VALUE_OK && variant==0);
        CHECK(xr_xir_enum_get(&concrete,0,0,&call->config.admission,&fields[0])==XR_XIR_VALUE_OK);
        CHECK(xr_xir_enum_get(&concrete,0,1,&call->config.admission,&fields[1])==XR_XIR_VALUE_OK);
        CHECK(xr_xir_enum_new((XrXirType)concrete.type,0,fields,2,&call->config.admission,&duplicate)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_error_erase(&duplicate,&call->config.admission,&error)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&fields[0]);xr_xir_value_drop(&fields[1]);xr_xir_value_drop(&duplicate);
        XrXirCallResult bad={0};CHECK(xr_xir_call_result_copy(outcome,&bad)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&bad.panic.replaced_error);xr_xir_value_move(&error,&bad.panic.replaced_error);
        CHECK(bad.panic.replaced_error.payload!=outcome->panic.replaced_error.payload && xr_xir_call_result_valid(&bad));
        int64_t owner=bad.panic.replaced_error.payload;
        XrXirCallStatus status=xir_cleanup_actual_complete_task_wait(call,token,&bad);
        fprintf(stderr,"genuine Source Task/token replacement-only mismatch: status%u owner_preserved%u\n",status,bad.panic.replaced_error.payload==owner);
        CHECK(status==XR_XIR_CALL_BAD_STATE && bad.panic.replaced_error.payload==owner && xr_xir_call_state(call)==XR_XIR_CALL_SUSPENDED);
        xr_xir_call_result_drop(&bad);++receiver_checks;
        CHECK(xr_xir_call_result_copy(outcome,&bad)==XR_XIR_VALUE_OK);bad.prior_status=XR_XIR_CALL_CANCELLED;
        CHECK(xir_cleanup_actual_complete_task_wait(call,token,&bad)==XR_XIR_CALL_BAD_STATE);
        bad.prior_status=XR_XIR_CALL_READY;xr_xir_call_result_drop(&bad);++receiver_checks;
    }
    return xir_cleanup_actual_complete_task_wait(call,token,outcome);
}
#endif
static void task_identity(void) {
    XrSourceArtifact *program=public_build("owned");XrSourceExecution *owner=execution();Output out={0};
#if defined(XR_CLEANUP_CALL_OBSERVER)
    receiver_active=true;receiver_checks=0;
#endif
    XrSourceResult *result=invoke(owner,program,"ordinaryTask",&out);int64_t value=0;
    CHECK(xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==420);
#if defined(XR_CLEANUP_CALL_OBSERVER)
    CHECK(receiver_checks==2 && !receiver_active);
#endif
    xray_source_result_free(result);xray_source_artifact_free(program);xray_source_execution_free(owner);
    puts("complete Source Task panic observed through the actual scheduler and caller");
}
#if defined(XR_CLEANUP_CALL_OBSERVER)
static void task_shutdown(bool pair) {
    Prepared ready=prepare();XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    RawOutput out={0};out.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,raw_bytes,&out.output,sizeof(out.output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,raw_group,&out};XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(ready.program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,named(ready.program,"afterFailure"),NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    shutdown_observing=true;shutdown_admitted=false;shutdown_pair=pair;shutdown_first=shutdown_fatal=false;
    CHECK(xr_xir_instance_start(instance,named(ready.program,pair?"stopPair":"stopTask"),NULL,0)==XR_XIR_CALL_READY);
    unsigned polls=0;XrXirInstanceResult result={0};
    do {
        CHECK(++polls<10000);result=xr_xir_instance_poll_bounded(instance,1);
        CHECK(result.outcome.status==XR_XIR_CALL_READY || result.outcome.status==XR_XIR_CALL_SUSPENDED);
        if (!shutdown_admitted && result.outcome.status==XR_XIR_CALL_SUSPENDED)
            CHECK(xr_xir_instance_resume(instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
    } while (!shutdown_admitted);
    if(pair) CHECK(contains(&out.output,"peer-armed\n",11));
    XrXirCallStatus stopped=xr_xir_instance_stop(instance);result=xr_xir_instance_poll_bounded(instance,1);
    fprintf(stderr,"Source Task new panic admitted before external stop: stop%u result%u valid%u replaced%u\n",
        stopped,result.outcome.status,xr_xir_call_result_valid(&result.outcome),result.outcome.panic.replaced_error.type);
    CHECK(stopped==(pair?XR_XIR_CALL_CLEANUP_FAILED:XR_XIR_CALL_DIVIDE_BY_ZERO) && result.outcome.status==stopped && xr_xir_call_result_valid(&result.outcome));
    XrXirCallResult owned={0};CHECK(xr_xir_call_result_copy(&result.outcome,&owned)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_instance_free(instance)==stopped);xr_xir_compile_program_drop(ready.program);
    if(pair) CHECK(shutdown_first && shutdown_fatal && owned.panic.detail.code==421 && owned.prior_status==XR_XIR_CALL_CANCELLED);
    else {static const char old[]="old é\0界";text_field(&owned.panic.replaced_error,old,sizeof(old)-1,23);}
    shutdown_pair=false;xr_xir_call_result_drop(&owned);
    XrCompileResourceStats stats;CHECK(xr_compile_resources_stats(ready.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==ready.baseline);
    xr_compile_resources_release(ready.resources);puts("Task shutdown preserves the complete already-admitted new panic and replaced Error owner");
}
#endif
static XrXirType optional_error_type(const XrXirTypes *types) {
    for (uint32_t i=0;i<types->count;++i) if (types->nodes[i].kind==XR_XIR_TYPE_NULLABLE && types->nodes[i].element==XR_XIR_ERROR)
        return (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
    CHECK(false);return XR_XIR_UNIT;
}
static int cancelled_compile(const void *context) {(void)context;return 1;}
static void resource_controls(void) {
    Prepared ready=prepare();XrXirCallResult source=raw_call(ready.program,"single446");
    XrXirType type=optional_error_type(ready.program->types);XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new(1024*1024,&domain)==XR_XIR_VALUE_OK);
    XrXirValue info={0},answer={0};CHECK(xr_xir_panic_info_new(domain,&source.panic,&info)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={ready.program->arena,domain,NULL,NULL,100000,100000};
    CHECK(xr_xir_panic_info_replaced_error(&info,XR_XIR_I64,&admission,&answer)==XR_XIR_VALUE_BAD_ARGUMENT && !answer.type && !answer.payload);
    admission.work=0;CHECK(xr_xir_panic_info_replaced_error(&info,type,&admission,&answer)==XR_XIR_VALUE_LIMIT && !answer.type && !answer.payload);
    uint64_t peak=0;
    for (unsigned mode=0;mode<3;++mode) {
        XrCompileResourceLimits limits={.memory_mode=mode?XR_COMPILE_MEMORY_FINITE:XR_COMPILE_MEMORY_ORDINARY,
            .memory_limit=mode?(mode==1?peak-1:peak):0,.profile_allocations=true};
        XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats initial,final;CHECK(xr_compile_resources_stats(resources,&initial)==XR_COMPILE_RESOURCE_OK);
        XrXirDomain *receiver=NULL;CHECK(xr_xir_compile_domain_new(resources,1024*1024,&receiver)==XR_XIR_VALUE_OK);
        XrXirValueAdmission receiving={ready.program->arena,receiver,NULL,NULL,0,100000};
        XrXirValueStatus status=xr_xir_panic_info_replaced_error(&info,type,&receiving,&answer);
        fprintf(stderr,"actual receiving held boundary mode%u limit%llu status%u\n",mode,(unsigned long long)limits.memory_limit,status);
        if (mode==1) CHECK(status==XR_XIR_VALUE_MEMORY_LIMIT && !answer.type && !answer.payload);
        else {CHECK(status==XR_XIR_VALUE_OK);bool some=false;const XrXirValue *error=NULL;
            CHECK(xr_xir_nullable_view(&answer,&some,&error) && some && error && error->payload==source.panic.replaced_error.payload);}
        xr_xir_value_drop(&answer);xr_xir_domain_drop(receiver);
        CHECK(xr_compile_resources_stats(resources,&final)==XR_COMPILE_RESOURCE_OK && final.held_bytes==initial.held_bytes);
        if (!mode) peak=final.peak_bytes;
        xr_compile_resources_release(resources);
    }
#if defined(XR_CLEANUP_VALUE_OBSERVER)
    XrXirDomainStats initial=xr_xir_domain_stats(domain);size_t live=resource_live;
    resource_index=0;resource_calls=0;resource_active=true;XrXirValue failed={0};
    XrXirValueStatus failed_status=xr_xir_panic_info_new(domain,&source.panic,&failed);resource_active=false;
    CHECK(failed_status==XR_XIR_VALUE_OOM && !failed.type && !failed.payload && resource_calls==1 && resource_live==live && xr_xir_domain_stats(domain).live_bytes==initial.live_bytes);
    admission.work=100000;resource_index=SIZE_MAX;resource_calls=0;resource_active=true;
    CHECK(xr_xir_panic_info_replaced_error(&info,type,&admission,&answer)==XR_XIR_VALUE_OK);resource_active=false;
    size_t calls=resource_calls;CHECK(calls);xr_xir_value_drop(&answer);
    for (unsigned i=0;i<2;++i) {
        admission.work=100000;live=resource_live;initial=xr_xir_domain_stats(domain);
        resource_index=i?calls-1:0;resource_calls=0;resource_active=true;
        failed_status=xr_xir_panic_info_replaced_error(&info,type,&admission,&answer);resource_active=false;
        CHECK(failed_status==XR_XIR_VALUE_OOM && !answer.type && !answer.payload && resource_live==live && xr_xir_domain_stats(domain).live_bytes==initial.live_bytes);
        admission.work=100000;CHECK(xr_xir_panic_info_replaced_error(&info,type,&admission,&answer)==XR_XIR_VALUE_OK);xr_xir_value_drop(&answer);
    }
#endif
    xr_xir_value_drop(&info);xr_xir_domain_drop(domain);xr_xir_call_result_drop(&source);xr_xir_compile_program_drop(ready.program);
    XrCompileResourceStats final;CHECK(xr_compile_resources_stats(ready.resources,&final)==XR_COMPILE_RESOURCE_OK && final.held_bytes==ready.baseline);
    xr_compile_resources_release(ready.resources);
#if defined(XR_CLEANUP_VALUE_OBSERVER)
    CHECK(!resource_live);
#endif
    size_t length;char *text=read_fixture("owned",&length);XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"cleanup-cancelled",text,length}};
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_CLEANUP_STDLIB;options.cancel_poll=cancelled_compile;
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    CHECK(xray_source_build(&input,&options,&artifact,&failure,&diagnostic)==XR_SOURCE_CANCELLED && !artifact);
    xray_source_diagnostic_free(diagnostic);xr_free(text);
    puts("cleanup resources: dynamic held peak/minus1, runtime local work, cancellation and empty output rollback passed");
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    if (!strcmp(argv[1],"source")) public_sources();
    else if (!strcmp(argv[1],"host")) public_fatal();
    else if (!strcmp(argv[1],"cancel")) public_cancel();
    else if (!strcmp(argv[1],"graph")) owned_graph();
    else if (!strcmp(argv[1],"late-cancel")) late_cancel();
    else if (!strcmp(argv[1],"task-identity")) task_identity();
#if defined(XR_CLEANUP_CALL_OBSERVER)
    else if (!strcmp(argv[1],"task-shutdown")) task_shutdown(false);
    else if (!strcmp(argv[1],"task-pair")) task_shutdown(true);
#endif
    else if (!strcmp(argv[1],"resources")) resource_controls();
    else if (!strcmp(argv[1],"sticky-host")) sticky_host_guard();
    else if (!strcmp(argv[1],"hard-reentry")) hard_close_reentry();
    else if (!strcmp(argv[1],"task-sendable")) task_sendable();
#if defined(XR_CLEANUP_HOST_OBSERVER)
    else if (!strcmp(argv[1],"host-wait")) host_wait_failure();
#endif
    else if (!strcmp(argv[1],"graph-base") || !strcmp(argv[1],"graph-single") || !strcmp(argv[1],"graph-fatal")) {
        Prepared ready=prepare();fprintf(stderr,"prepared program refs %u\n",atomic_load(&ready.program->references));
        XrXirCallResult result={0};
        if (strcmp(argv[1],"graph-base")) result=raw_call(ready.program,!strcmp(argv[1],"graph-single")?"single446":"fatal446");
        fprintf(stderr,"after call program refs %u\n",atomic_load(&ready.program->references));
        xr_xir_compile_program_drop(ready.program);xr_xir_call_result_drop(&result);
        XrCompileResourceStats stats;CHECK(xr_compile_resources_stats(ready.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"diagnostic final held=%llu\n",(unsigned long long)stats.held_bytes);
        CHECK(stats.held_bytes==ready.baseline);xr_compile_resources_release(ready.resources);
    } else CHECK(false);
    return 0;
}
