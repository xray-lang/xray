/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_task_result_source.c - Owned four-state Source results after producer retirement
 */
#include "xray_source_instance.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"TaskResult Source %d: %s\n",__LINE__,#c);exit(1); } } while (0)
typedef struct Output { char bytes[4096];size_t length; } Output;
static XrSourceWriteStatus collect(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    Output *output=(Output *)context;
    CHECK(stream==XR_SOURCE_STDOUT || stream==XR_SOURCE_STDERR);
    CHECK(length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;
    return XR_SOURCE_WRITE_OK;
}
static int contains(const Output *output,const char *text) {
    size_t length=strlen(text);
    for (size_t i=0;i<=output->length && length<=output->length-i;++i)
        if (!memcmp(output->bytes+i,text,length)) return 1;
    return 0;
}
static char *read_input(const char *path,size_t *length) {
    FILE *file=NULL;
#if defined(_MSC_VER)
    CHECK(!fopen_s(&file,path,"rb"));
#else
    file=fopen(path,"rb");
#endif
    CHECK(file && !fseek(file,0,SEEK_END));long size=ftell(file);
    CHECK(size>0 && size<65536 && !fseek(file,0,SEEK_SET));
    char *text=(char *)malloc((size_t)size+1);CHECK(text);
    CHECK(fread(text,1,(size_t)size,file)==(size_t)size && !fclose(file));
    *length=(size_t)size;text[*length]=0;return text;
}
static XrSourceInstance *prepare(const char *stdlib,const char *path,Output *output) {
    size_t length=0;char *text=read_input(path,&length);
    XrSourceInput input={0};input.kind=XR_SOURCE_INPUT_TEXT;
    input.value.text.namespace_id="task-result";input.value.text.bytes=text;input.value.text.length=length;
    XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=stdlib;
    XrSourceArtifact *artifact=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_build(&input,&options,&artifact,&failure,&diagnostic);
    memset(text,0xa5,length);free(text);
    if (status!=XR_SOURCE_OK && diagnostic) {
        XrSourceDiagnosticReportInfo info={0};CHECK(xray_source_diagnostic_report(diagnostic,&info)==XR_SOURCE_OK);
        for (uint32_t i=0;i<info.roots;++i) {
            XrSourceDiagnosticRoot root={0};CHECK(xray_source_diagnostic_root(diagnostic,i,&root)==XR_SOURCE_OK);
            fprintf(stderr,"Source%u stage%u %s\n",status,failure.stage,root.message);
        }
    }
    CHECK(status==XR_SOURCE_OK && artifact && !diagnostic);
    XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
    CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    runtime.write=collect;runtime.context=output;XrSourceInstance *instance=NULL;
    CHECK(xray_source_instance_new(execution,artifact,&runtime,&instance)==XR_SOURCE_OK && instance);
    xray_source_artifact_free(artifact);xray_source_execution_free(execution);return instance;
}
static XrSourceInvocation invocation(const char *name) {
    XrSourceInvocation request;CHECK(xray_source_invocation_init(&request,sizeof(request))==XR_SOURCE_OK);
    request.name=name;request.name_length=strlen(name);return request;
}
static XrSourceEvent terminal(XrSourceCall *call) {
    XrSourceEvent event={0};unsigned steps=0;
    do {
        CHECK(++steps<10000 && xray_source_call_poll(call,64,&event)==XR_SOURCE_OK);
        if (event.state==XR_SOURCE_RUN_SUSPENDED) {
            XrSourceWait wait={0};CHECK(xray_source_call_wait_request(call,event.epoch,event.wake,&wait,sizeof(wait))==XR_SOURCE_OK);
            CHECK(wait.kind==XR_SOURCE_WAIT_YIELD);
            CHECK(xray_source_call_resume(call,event.epoch,event.wake)==XR_SOURCE_OK);
        }
    } while (event.state==XR_SOURCE_RUN_READY || event.state==XR_SOURCE_RUN_SUSPENDED);
    return event;
}
static XrSourceResult *execute(XrSourceInstance *instance,const char *name) {
    XrSourceInvocation request=invocation(name);XrSourceCall *call=NULL;
    CHECK(xray_source_instance_begin(instance,&request,&call)==XR_SOURCE_OK && call);
    (void)terminal(call);XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);return result;
}
static void task_result_source(const char *stdlib,const char *path) {
    Output output={0};XrSourceInstance *instance=prepare(stdlib,path,&output);
    const char *names[]={"successResult","errorResult","panicResult","cancelledResult","unitResult","nestedResult","resultArray"};
    XrSourceValue *survivors[7]={0};
    for (size_t i=0;i<7;++i) {
        XrSourceResult *result=execute(instance,names[i]);XrSourceEvent event={0};
        CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
        XrSourceValue *original=NULL;
        CHECK(xray_source_result_value(result,&original)==XR_SOURCE_OK && original);
        CHECK(xray_source_value_copy(original,&survivors[i])==XR_SOURCE_OK && survivors[i]);
        xray_source_value_free(original);xray_source_result_free(result);
    }
    XrSourceResult *probe=execute(instance,"runtimeProbe");int64_t number=0;
    CHECK(xray_source_result_i64(probe,&number)==XR_SOURCE_OK && number==42);xray_source_result_free(probe);
    CHECK(xray_source_instance_drop(&instance,NULL)==XR_SOURCE_OK && !instance);
    for (size_t i=0;i<7;++i) {
        XrSourceValueKind kind=XR_SOURCE_VALUE_UNIT;
        CHECK(xray_source_value_kind(survivors[i],&kind)==XR_SOURCE_OK &&
            kind==(i==6?XR_SOURCE_VALUE_ARRAY:XR_SOURCE_VALUE_ENUM));
    }
    output=(Output){0};
    CHECK(xray_source_value_format(survivors[0],collect,&output)==XR_SOURCE_OK && contains(&output,"TaskResult.Success(42)"));
    output=(Output){0};
    CHECK(xray_source_value_format(survivors[1],collect,&output)==XR_SOURCE_OK && contains(&output,"TaskResult.Error(A.Bad("));
    static const char error_text[]="error \303\251\0\347\225\214";
    bool found=false;
    for (size_t i=0;i<=output.length && sizeof(error_text)-1<=output.length-i;++i)
        found|=!memcmp(output.bytes+i,error_text,sizeof(error_text)-1);
    CHECK(found);
    output=(Output){0};
    CHECK(xray_source_value_format(survivors[3],collect,&output)==XR_SOURCE_OK && contains(&output,"TaskResult.Cancelled"));
    for (size_t i=0;i<7;++i)xray_source_value_free(survivors[i]);
    puts("TaskResult Source: four terminal tags, Unit/Never, nested Task/Array, repeated observations and retired input/request/instance PASS");
}
static void unsafe_constructor(const char *stdlib,const char *path) {
    Output output={0};XrSourceInstance *instance=prepare(stdlib,path,&output);
    XrSourceResult *ordinary=execute(instance,"unsafePanicInfo");
    XrSourceValue *optional=NULL,*info=NULL;
    CHECK(xray_source_result_value(ordinary,&optional)==XR_SOURCE_OK && optional);
    int some=0;CHECK(xray_source_value_nullable(optional,&some,&info)==XR_SOURCE_OK && some==1 && info);
    xray_source_value_free(optional);xray_source_result_free(ordinary);
    XrSourceResult *refused=execute(instance,"unsafeConstruct");XrSourceEvent event={0};
    CHECK(xray_source_result_event(refused,&event)==XR_SOURCE_OK &&
        event.state==XR_SOURCE_RUN_API_ERROR && event.engine_status==9);
    XrSourceValue *unchanged=NULL;
    CHECK(xray_source_result_value(refused,&unchanged)==XR_SOURCE_BAD_STATE && !unchanged);
    xray_source_result_free(refused);
    CHECK(xray_source_instance_drop(&instance,NULL)==XR_SOURCE_OK && !instance);
    int64_t code=0;CHECK(xray_source_value_panic_code(info,&code)==XR_SOURCE_OK && code==446);
    XrSourceValue *copy=NULL;CHECK(xray_source_value_copy(info,&copy)==XR_SOURCE_OK && copy);
    xray_source_value_free(info);
    CHECK(xray_source_value_panic_code(copy,&code)==XR_SOURCE_OK && code==446);
    xray_source_value_free(copy);
    puts("TaskResult Source: unsafe complete PanicInfo graph rejected as API error; ordinary local graph and owner preserved PASS");
}
static void independent_terminal(const char *stdlib,const char *path,bool own_cancel) {
    Output output={0};XrSourceInstance *instance=prepare(stdlib,path,&output);
    XrSourceResult *initialized=execute(instance,"runtimeProbe");int64_t probe=0;
    CHECK(xray_source_result_i64(initialized,&probe)==XR_SOURCE_OK && probe==42);
    xray_source_result_free(initialized);
    static const char root[]="child-cleanup\n0\n1\n2\n3\n42\n0\n0\n1\n";
    CHECK(output.length==sizeof(root)-1 && !memcmp(output.bytes,root,output.length));
    output=(Output){0};
    XrSourceInvocation request=invocation(own_cancel?"ownAwaitResult":"fatalResult");
    XrSourceCall *call=NULL;
    CHECK(xray_source_instance_begin(instance,&request,&call)==XR_SOURCE_OK && call);
    if (own_cancel) {
        for (unsigned step=0;step<100;++step) {
            XrSourceEvent running={0};
            CHECK(xray_source_call_poll(call,1,&running)==XR_SOURCE_OK);
            CHECK(running.state==XR_SOURCE_RUN_READY || running.state==XR_SOURCE_RUN_SUSPENDED);
            if (running.state==XR_SOURCE_RUN_SUSPENDED) {
                XrSourceWait wait={0};
                CHECK(xray_source_call_wait_request(call,running.epoch,running.wake,&wait,sizeof(wait))==XR_SOURCE_OK &&
                    wait.kind==XR_SOURCE_WAIT_YIELD);
                CHECK(xray_source_call_resume(call,running.epoch,running.wake)==XR_SOURCE_OK);
            }
        }
        CHECK(!output.length && xray_source_call_cancel(call)==XR_SOURCE_OK);
    }
    XrSourceEvent terminal_event=terminal(call);
    CHECK(terminal_event.state==(own_cancel?XR_SOURCE_RUN_CANCELLED:XR_SOURCE_RUN_CLEANUP_FAILED));
    XrSourceResult *result=NULL;CHECK(xray_source_call_take(call,&result)==XR_SOURCE_OK && result);
    XrSourceValue *value=NULL;
    CHECK(xray_source_result_value(result,&value)==XR_SOURCE_BAD_STATE && !value);
    CHECK(xray_source_call_drop(&call,NULL)==XR_SOURCE_OK && !call);
    CHECK(xray_source_instance_drop(&instance,NULL)==XR_SOURCE_OK && !instance);
    XrSourceEvent retained={0};CHECK(xray_source_result_event(result,&retained)==XR_SOURCE_OK &&
        retained.state==terminal_event.state && retained.engine_status==terminal_event.engine_status);
    const char *expected=own_cancel?"parent-cleanup\nchild-cleanup\n":"fatal-started\nfatal-cleanup\n";
    CHECK(output.length==strlen(expected) && !memcmp(output.bytes,expected,output.length) && !contains(&output,"caught-own"));
    xray_source_result_free(result);
    puts(own_cancel?"TaskResult Source: own cancellation bypasses catch and publishes after parent/child cleanup PASS":
        "TaskResult Source: original fatal child cleanup remains independent and owns no Result value PASS");
}
static void sendable_cycle_controls(const char *stdlib) {
    static const char *sources[]={
        "enum Loop { Again { value: TaskResult<i64,Loop> } }\nexport fn answer()->i64{return 42}\n",
        "enum Loop { Again { value: TaskResult<i64,Loop> } }\nfn work()->Loop { return Loop.Again { value: TaskResult<i64,Loop>.Success { value: 42 } } }\nexport fn answer()->i64 { var task = go work(); return 42 }\n",
        "enum Other { Bad }\nenum Loop { Again { value: TaskResult<i64,Loop|Other> } }\nfn work()->Loop { return Loop.Again { value: TaskResult<i64,Loop|Other>.Success { value: 42 } } }\nexport fn answer()->i64 { var task=go work();return 42 }\n",
        "class Box implements Sendable {\n items: Array<Box>\n init(items:Array<Box>){this.items=items}\n}\nenum E{Bad{value:Box}}\nfn work()->TaskResult<i64,E>{return TaskResult<i64,E>.Success{value:42}}\nexport fn answer()->i64{var task=go work();const result=task.awaitResult();return 42}\nprint(answer())\n",
        "enum A{Bad}\nenum B{Bad}\nfn work()->TaskResult<i64,A|B>{return TaskResult<i64,A|B>.Error{error:B.Bad}}\nexport fn answer()->i64{var task=go work();const result=task.awaitResult();return 42}\nprint(answer())\n"
    };
    for (size_t i=0;i<sizeof(sources)/sizeof(sources[0]);++i) {
        XrSourceInput input={0};input.kind=XR_SOURCE_INPUT_TEXT;
        input.value.text.namespace_id="task-result-sendable-cycle";
        input.value.text.bytes=sources[i];input.value.text.length=strlen(sources[i]);
        XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=stdlib;
        XrSourceArtifact *artifact=NULL;XrSourceDiagnostic *diagnostic=NULL;XrSourceFailure failure={0};
        XrSourceStatus status=xray_source_build(&input,&options,&artifact,&failure,&diagnostic);
        if (i==1 || i==2) {
            CHECK(status==XR_SOURCE_REJECTED && !artifact && diagnostic);
            XrSourceDiagnosticReportInfo report={0};
            CHECK(xray_source_diagnostic_report(diagnostic,&report)==XR_SOURCE_OK && report.roots);
        } else {
            CHECK(status==XR_SOURCE_OK && artifact && !diagnostic);
            if (i>=3) {
                XrSourceExecutionOptions policy=xray_source_execution_options_default();XrSourceExecution *execution=NULL;
                CHECK(xray_source_execution_new(&policy,&execution)==XR_SOURCE_OK);
                XrSourceCallOptions call;CHECK(xray_source_call_options_init(&call,sizeof(call))==XR_SOURCE_OK);
                Output output={0};call.write=collect;call.context=&output;XrSourceResult *result=NULL;
                CHECK(xray_source_artifact_execute(execution,artifact,&call,&result,NULL)==XR_SOURCE_OK && result);
                XrSourceEvent event={0};CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK &&
                    event.state==XR_SOURCE_RUN_RETURNED && output.length==3 && !memcmp(output.bytes,"42\n",3));
                xray_source_result_free(result);xray_source_execution_free(execution);
            }
        }
        xray_source_diagnostic_free(diagnostic);xray_source_artifact_free(artifact);
    }
    puts("TaskResult Sendable: recursive E/union reject without recursion, declaration and readonly Class identity cycle remain valid PASS");
}
int main(int argc,char **argv) {
    CHECK(argc==3);sendable_cycle_controls(argv[1]);task_result_source(argv[1],argv[2]);unsafe_constructor(argv[1],argv[2]);
    independent_terminal(argv[1],argv[2],true);independent_terminal(argv[1],argv[2],false);return 0;
}
