/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * try_assert_source_cases.h - Owned panic graphs at actual public call boundaries
 */
static void try_assert_source_graph(void) {
    static const char pure[]=
        "enum Failure { Bad { text:string } }\n"
        "fn fail() throws(Failure)->i64 {throw Failure.Bad{text:\"owned é\\0界\"}}\n";
    static const char authority[]=
        "final class Local {value:i64 init(value:i64){this.value=value}}\n"
        "enum Failure { Bad { hidden:Local } }\n"
        "fn fail() throws(Failure)->i64 {throw Failure.Bad{hidden:Local(23)}}\n";
    static const char capture[]=
        "fn capture()->PanicInfo {try {const ignored=try! fail();return capture()} catch panic(p){return p}}\n";
    static const char direct[]=
        "export fn make()->PanicInfo {return capture()}\n"
        "export fn read(value:PanicInfo)->i64 {return value.code}\n";
    static const char array[]=
        "export fn make()->Array<PanicInfo> {return [capture()]}\n"
        "export fn read(value:Array<PanicInfo>)->i64 {return len(value)}\n";
    for(unsigned aggregate=0;aggregate<2;++aggregate) for(unsigned foreign=0;foreign<2;++foreign) {
        char source[2048];int count=snprintf(source,sizeof(source),"%s%s%s",foreign?authority:pure,capture,aggregate?array:direct);
        CHECK(count>0 && (size_t)count<sizeof(source));
        XrSourceArtifact *artifact=build_text(XR_SOURCE_TARGET_VM,source,(size_t)count);memset(source,0xa5,sizeof(source));
        XrSourceExecution *execution=NULL;XrSourceExecutionOptions options=xray_source_execution_options_default();
        CHECK(xray_source_execution_new(&options,&execution)==XR_SOURCE_OK);
        XrSourceInvocation invocation;CHECK(xray_source_invocation_init(&invocation,sizeof(invocation))==XR_SOURCE_OK);
        invocation.name="make";invocation.name_length=4;
        XrSourceCallOptions call_options;CHECK(xray_source_call_options_init(&call_options,sizeof(call_options))==XR_SOURCE_OK);
        XrSourceResult *result=NULL;XrSourceEvent event={0};
        CHECK(xray_source_invoke(execution,artifact,&invocation,&call_options,&result,&event)==XR_SOURCE_OK && result);
        CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
        XrSourceValue *value=NULL,*copy=NULL;CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK);
        xray_source_result_free(result);result=NULL;
        CHECK(xray_source_value_copy(value,&copy)==XR_SOURCE_OK);xray_source_value_free(value);
        XrSourceValueKind kind;CHECK(xray_source_value_kind(copy,&kind)==XR_SOURCE_OK &&
            kind==(aggregate?XR_SOURCE_VALUE_ARRAY:XR_SOURCE_VALUE_PANIC_INFO));
        const XrSourceValue *arguments[]={copy};invocation.name="read";invocation.arguments=arguments;invocation.argument_count=1;
        XrSourceCall *call=NULL;XrSourceStatus admitted=xray_source_invoke_begin(execution,artifact,&invocation,&call_options,&call);
        if(foreign) CHECK(admitted==XR_SOURCE_BAD_ARGUMENT && !call);
        else CHECK(admitted==XR_SOURCE_OK && call);
        xray_source_value_free(copy);xray_source_artifact_free(artifact);xray_source_execution_free(execution);
        if(!foreign) {
            for(unsigned i=0;i<4096;++i) {CHECK(xray_source_call_poll(call,8,&event)==XR_SOURCE_OK);if(event.state!=XR_SOURCE_RUN_READY)break;}
            CHECK(event.state==XR_SOURCE_RUN_RETURNED && xray_source_call_take(call,&result)==XR_SOURCE_OK);
            XrSourceEvent cleanup={0};CHECK(xray_source_call_drop(&call,&cleanup)==XR_SOURCE_OK && cleanup.engine_status==XR_XIR_CALL_READY);
            int64_t answer=0;CHECK(xray_source_result_i64(result,&answer)==XR_SOURCE_OK && answer==(aggregate?1:446));
            xray_source_result_free(result);
        }
        CHECK(compile_cancel.polls==compile_cancel.finished);
        printf("Public %s PanicInfo graph: %s, request/input/producer retired\n",aggregate?"Array":"direct",
            foreign?"foreign Class Domain rejected before receiver":"owned scalar/String Error admitted");
    }
}

static void try_assert_source_rejections(void) {
    static const char prefix[]="enum Failure { Bad }\nfn fail() throws(Failure)->i64 {throw Failure.Bad}\n";
    static const char *const bodies[]={
        "fn run()->i64 {const value=try! fail();return fail()}\n",
        "fn run()->i64 {const callback=try! fn()->i64 {return fail()};return callback()}\n"
    };
    for(size_t i=0;i<sizeof(bodies)/sizeof(*bodies);++i) {
        char text[1024];int count=snprintf(text,sizeof(text),"%s%s",prefix,bodies[i]);CHECK(count>0 && (size_t)count<sizeof(text));
        XrSourceCompileOptions options=xray_source_compile_options_default();options.stdlib_path=XR_PUBLIC_STDLIB;
        XrSourceInput input={.kind=XR_SOURCE_INPUT_TEXT,.value.text={NULL,"try-lexical-errors",text,(size_t)count}};
        XrSourceArtifact *output=NULL;XrSourceFailure failure={0};XrSourceDiagnostic *diagnostic=NULL;
        CHECK(xray_source_build(&input,&options,&output,&failure,&diagnostic)==XR_SOURCE_REJECTED && !output && diagnostic);
        XrSourceDiagnosticRoot info={0};CHECK(xray_source_diagnostic_root(diagnostic,0,&info)==XR_SOURCE_OK && info.message && strstr(info.message,"E0330:"));
        xray_source_diagnostic_free(diagnostic);
    }
    puts("Actual Source lexical continuation ends before later calls and nested function bodies; exact E0330");
}
