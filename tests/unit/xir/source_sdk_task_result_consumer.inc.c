/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_sdk_task_result_consumer.inc.c - Detached terminal Task data in public hosts
 */
typedef struct SdkTaskOutput { char bytes[64]; size_t length; } SdkTaskOutput;
static XrSourceWriteStatus sdk_task_output(void *context,XrSourceOutputStream stream,
    const char *bytes,size_t length) {
    SdkTaskOutput *output=(SdkTaskOutput *)context;
    CHECK(stream==XR_SOURCE_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;
    return XR_SOURCE_WRITE_OK;
}
static XrSourceValue *sdk_task_value(XrSourceResult *result,XrSourceValueKind expected) {
    XrSourceValue *value=NULL;XrSourceValueKind kind;
    CHECK(xray_source_result_value(result,&value)==XR_SOURCE_OK && value);
    CHECK(xray_source_value_kind(value,&kind)==XR_SOURCE_OK && kind==expected);
    return value;
}
static void sdk_terminal_task_cases(const char *stdlib_path) {
    static const char source[]=
        "enum Failure{Bad}\n"
        "fn number()->i64{return 42}\nfn text()->string{return \"A\\0中\"}\n"
        "fn array()->Array<string>{return [\"A\\0中\",\"next\"]}\n"
        "fn unit()->(){return ()}\n"
        "fn nested()->Task<i64>{const t=go number();await t;return t}\n"
        "fn broken()->i64{throw Failure.Bad}\n"
        "fn panicValue()->i64{assert(false,\"panic\\0中\");return 0}\n"
        "export fn numberTask()->Task<i64>{const t=go number();await t;return t}\n"
        "export fn textTask()->Task<string>{const t=go text();await t;return t}\n"
        "export fn arrayTask()->Task<Array<string>>{const t=go array();await t;return t}\n"
        "export fn unitTask()->Task<()>{const t=go unit();await t;return t}\n"
        "export fn nestedTask()->Task<Task<i64>>{const t=go nested();await t;return t}\n"
        "export fn errorTask()->Task<i64>{const t=go broken();try{await t}catch(e){};return t}\n"
        "export fn panicTask()->Task<i64>{const t=go panicValue();try{await t}catch panic(p){};return t}\n";
    static const char *names[]={"numberTask","textTask","arrayTask","unitTask","nestedTask","errorTask","panicTask"};
    XrSourceCompilerOptions options;
    CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=stdlib_path;
    XrSourceCompiler *compiler=NULL;XrSourceProgram *program=NULL;
    CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK && compiler);
    const XrSourceText text={NULL,"installed-terminal-tasks",source,sizeof(source)-1};
    CHECK(xray_source_compile_text(compiler,&text,&program,NULL)==XR_SOURCE_OK && program);
    XrSourceValue *tasks[7]={NULL};XrSourceResult *survivors[7]={NULL};
    for(size_t i=0;i<7;++i) {
        XrSourceInvocation invocation=request(names[i],NULL,0);XrSourceResult *result=NULL;
        CHECK(xray_source_invoke(program,&invocation,NULL,&result,NULL)==XR_SOURCE_OK && result);
        XrSourceEvent event;
        CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
        tasks[i]=sdk_task_value(result,XR_SOURCE_VALUE_TASK);xray_source_result_free(result);
    }
    xray_source_program_free(program);xray_source_compiler_free(compiler);
    for(size_t i=0;i<7;++i) {
        XrSourceResult *first=NULL;
        CHECK(xray_source_value_task_result(tasks[i],&first)==XR_SOURCE_OK && first);
        CHECK(xray_source_value_task_result(tasks[i],&survivors[i])==XR_SOURCE_OK && survivors[i]);
        XrSourceResult *occupied=first;
        CHECK(xray_source_value_task_result(tasks[i],&occupied)==XR_SOURCE_BAD_ARGUMENT && occupied==first);
        CHECK(xray_source_value_task_result(tasks[i],NULL)==XR_SOURCE_BAD_ARGUMENT);
        xray_source_result_free(first);xray_source_value_free(tasks[i]);
        XrSourceEvent event;
        CHECK(xray_source_result_event(survivors[i],&event)==XR_SOURCE_OK);
        CHECK(event.state==(i==5?XR_SOURCE_RUN_THROWN:i==6?XR_SOURCE_RUN_PANICKED:XR_SOURCE_RUN_RETURNED));
    }
    int64_t number=0;
    CHECK(xray_source_result_i64(survivors[0],&number)==XR_SOURCE_OK && number==42);
    const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_result_bytes(survivors[1],&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof("A\0中")-1 && !memcmp(bytes,"A\0中",length));
    SdkTaskOutput formatted;memset(&formatted,0,sizeof(formatted));
    CHECK(xray_source_result_format(survivors[2],sdk_task_output,&formatted)==XR_SOURCE_OK);
    static const char array_text[]="[\"A\0中\", \"next\"]";
    CHECK(formatted.length==sizeof(array_text)-1 && !memcmp(formatted.bytes,array_text,formatted.length));
    XrSourceValue *unit=sdk_task_value(survivors[3],XR_SOURCE_VALUE_UNIT);XrSourceResult *refused=NULL;
    CHECK(xray_source_value_task_result(unit,&refused)==XR_SOURCE_INVALID && !refused);xray_source_value_free(unit);
    XrSourceValue *nested=sdk_task_value(survivors[4],XR_SOURCE_VALUE_TASK);
    CHECK(xray_source_value_task_result(nested,&refused)==XR_SOURCE_OK && refused);xray_source_value_free(nested);
    CHECK(xray_source_result_i64(refused,&number)==XR_SOURCE_OK && number==42);xray_source_result_free(refused);refused=NULL;
    XrSourceValue *error=sdk_task_value(survivors[5],XR_SOURCE_VALUE_ENUM);xray_source_value_free(error);
    CHECK(xray_source_result_bytes(survivors[6],&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof("panic\0中")-1 && !memcmp(bytes,"panic\0中",length));
    for(size_t i=0;i<7;++i)xray_source_result_free(survivors[i]);
    CHECK(xray_source_value_task_result(NULL,&refused)==XR_SOURCE_BAD_ARGUMENT && !refused);
    puts("Installed terminal Task outcomes: repeated owned data, nested Task42, thrown and NUL/UTF8 panic after all producers PASS");
}
