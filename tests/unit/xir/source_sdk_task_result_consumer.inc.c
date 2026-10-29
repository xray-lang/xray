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
static void sdk_task_language_cases(const char *stdlib_path) {
    char source[]=
        "enum A { Bad { text: string } }\n"
        "enum B { Bad { text: string } }\n"
        "fn scalar42() -> i64 { return 42 }\n"
        "fn unit() -> () { return () }\n"
        "fn failA() throws(A) -> i64 { throw A.Bad { text: \"error é\\0界\" } }\n"
        "fn failB() throws(B) -> i64 { throw B.Bad { text: \"panic é\\0界\" } }\n"
        "fn cleanupPanic() throws(A) -> i64 {\n"
        "    defer { const ignored = try! failB() }\n"
        "    throw A.Bad { text: \"replaced é\\0界\" }\n"
        "}\n"
        "fn spinning() -> i64 {\n"
        "    defer { print(\"child-cleanup\") }\n"
        "    while (true) { Coro.yield() }\n"
        "    return 7\n"
        "}\n"
        "export fn successResult() -> TaskResult<i64,Never> {\n"
        "    var task = go scalar42()\n"
        "    const before = task.poll()\n"
        "    return task.awaitResult()\n"
        "}\n"
        "export fn errorResult() -> TaskResult<i64,A> {\n"
        "    var task = go failA()\n"
        "    return task.awaitResult()\n"
        "}\n"
        "export fn panicResult() -> TaskResult<i64,A> {\n"
        "    var task = go cleanupPanic()\n"
        "    return task.awaitResult()\n"
        "}\n"
        "export fn cancelledResult() -> TaskResult<i64,Never> {\n"
        "    var task = go spinning()\n"
        "    task.cancel()\n"
        "    task.cancel()\n"
        "    return task.awaitResult()\n"
        "}\n"
        "export fn unitResult() -> TaskResult<null,Never> {\n"
        "    var task = go unit()\n"
        "    return task.awaitResult()\n"
        "}\n"
        "fn returnedResult() -> TaskResult<null,Never> {\n"
        "    return TaskResult<null,Never>.Success { value: () }\n"
        "}\n"
        "export fn nestedResult() -> TaskResult<TaskResult<null,Never>,Never> {\n"
        "    var task = go returnedResult()\n"
        "    return task.awaitResult()\n"
        "}\n"
        "export fn resultArray() -> Array<TaskResult<null,Never>> {\n"
        "    return [returnedResult()]\n"
        "}\n"
        "export fn runtimeProbe() throws(Cancelled) -> i64 {\n"
        "    var task = go scalar42()\n"
        "    const before = task.poll()\n"
        "    const wrapped = task.awaitResult()\n"
        "    return try await task\n"
        "}\n"
        "const success = successResult()\n"
        "const error = errorResult()\n"
        "const panic = panicResult()\n"
        "const cancelled = cancelledResult()\n"
        "print(success.ordinal)\n"
        "print(error.ordinal)\n"
        "print(panic.ordinal)\n"
        "print(cancelled.ordinal)\n"
        "print(match (success) { TaskResult.Success { value } -> value _ -> -1 })\n"
        "print(unitResult().ordinal)\n"
        "print(nestedResult().ordinal)\n"
        "print(len(resultArray()))\n"
        "\n"
        "final class LocalPayload { value: i64 init(value: i64) { this.value = value } }\n"
        "enum UnsafeError { Bad { hidden: LocalPayload } }\n"
        "fn failLocal() throws(UnsafeError) -> i64 {\n"
        "    throw UnsafeError.Bad { hidden: LocalPayload(7) }\n"
        "}\n"
        "fn panicLocal() throws(UnsafeError) -> i64 {\n"
        "    defer { const ignored = try! failB() }\n"
        "    return try failLocal()\n"
        "}\n"
        "export fn unsafePanicInfo() -> PanicInfo? {\n"
        "    try { const ignored = try panicLocal() }\n"
        "    catch (error: UnsafeError) { return null }\n"
        "    catch panic (info) { return info }\n"
        "    return null\n"
        "}\n"
        "export fn unsafeConstruct() -> TaskResult<null,Never> {\n"
        "    const info = unsafePanicInfo()!\n"
        "    return TaskResult<null,Never>.Panic { info: info }\n"
        "}\n"
        "export fn rethrowA(result: TaskResult<i64,A>) throws(A) -> i64 {\n"
        "    return match (result) { TaskResult.Error { error } -> { throw error } _ -> 0 }\n"
        "}\n"
        "\n"
        "fn divideZero(value: i64) -> i64 { return 1 / value }\n"
        "fn fatalChild() -> i64 {\n"
        "    defer { print(\"fatal-cleanup\"); const ignored = divideZero(0) }\n"
        "    print(\"fatal-started\")\n"
        "    while (true) { Coro.yield() }\n"
        "    return 0\n"
        "}\n"
        "export fn fatalResult() -> TaskResult<i64,Never> {\n"
        "    var task = go fatalChild()\n"
        "    Coro.yield()\n"
        "    task.cancel()\n"
        "    return task.awaitResult()\n"
        "}\n"
        "export fn ownAwaitResult() -> TaskResult<i64,Never> {\n"
        "    var task = go spinning()\n"
        "    defer { print(\"parent-cleanup\"); task.cancel() }\n"
        "    try { return task.awaitResult() }\n"
        "    catch (error: Cancelled) { print(\"caught-own\"); return TaskResult<i64,Never>.Cancelled }\n"
        "}\n"
        "\n"
        "export fn replacedGraph() -> Error? {\n"
        "    var task = go cleanupPanic()\n"
        "    const result = task.awaitResult()\n"
        "    return match (result) { TaskResult.Panic { info } -> info.replacedError _ -> null }\n"
        "}\n"
        ;
#ifdef __cplusplus
    static_assert(sizeof(source)-1==3694,"Complete TaskResult Source bytes");
#else
    _Static_assert(sizeof(source)-1==3694,"Complete TaskResult Source bytes");
#endif
    XrSourceInput input;memset(&input,0,sizeof(input));input.kind=XR_SOURCE_INPUT_TEXT;
    input.value.text.namespace_id="installed-task-result";
    input.value.text.bytes=source;input.value.text.length=sizeof(source)-1;
    XrSourceCompileOptions compiler=xray_source_compile_options_default();compiler.stdlib_path=stdlib_path;
    XrSourceArtifact *program=NULL;XrSourceDiagnostic *diagnostic=NULL;XrSourceFailure failure;
    memset(&failure,0,sizeof(failure));
    CHECK(xray_source_build(&input,&compiler,&program,&failure,&diagnostic)==XR_SOURCE_OK && program && !diagnostic);
    memset(source,0xa5,sizeof(source)-1);
    XrSourceExecution *execution=sdk_execution_new();XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    SdkTaskOutput output;memset(&output,0,sizeof(output));runtime.write=sdk_task_output;runtime.context=&output;
    const char *names[]={"successResult","errorResult","panicResult","cancelledResult"};
    XrSourceValue *survivors[4]={NULL,NULL,NULL,NULL};
    for(size_t i=0;i<4;++i) {
        output.length=0;XrSourceInvocation invoke=request(names[i],NULL,0);XrSourceResult *result=NULL;
        CHECK(xray_source_invoke(execution,program,&invoke,&runtime,&result,NULL)==XR_SOURCE_OK && result);
        XrSourceEvent event;CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
        static const char root[]="child-cleanup\n0\n1\n2\n3\n42\n0\n0\n1\n";
        CHECK(output.length==sizeof(root)-1+(i==3?14:0) && !memcmp(output.bytes,root,sizeof(root)-1));
        XrSourceValue *original=sdk_task_value(result,XR_SOURCE_VALUE_ENUM);
        CHECK(xray_source_value_copy(original,&survivors[i])==XR_SOURCE_OK && survivors[i]);
        xray_source_value_free(original);xray_source_result_free(result);
    }
    xray_source_artifact_free(program);xray_source_execution_free(execution);
    for(size_t i=0;i<4;++i) {
        XrSourceValueKind kind;CHECK(xray_source_value_kind(survivors[i],&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_ENUM);
    }
    output.length=0;CHECK(xray_source_value_format(survivors[0],sdk_task_output,&output)==XR_SOURCE_OK);
    static const char success[]="TaskResult.Success(42)";
    CHECK(output.length==sizeof(success)-1 && !memcmp(output.bytes,success,output.length));
    output.length=0;CHECK(xray_source_value_format(survivors[3],sdk_task_output,&output)==XR_SOURCE_OK);
    static const char cancelled[]="TaskResult.Cancelled";
    CHECK(output.length==sizeof(cancelled)-1 && !memcmp(output.bytes,cancelled,output.length));
    for(size_t i=0;i<4;++i)xray_source_value_free(survivors[i]);
}
static void sdk_terminal_task_cases(const char *stdlib_path) {
    static const char source[]=
        "enum Failure{Bad}\n"
        "fn number()->i64{return 42}\nfn text()->string{return \"A\\0中\"}\n"
        "fn array()->Array<string>{return [\"A\\0中\",\"next\"]}\n"
        "fn unit()->(){return ()}\n"
        "fn nested() throws(Cancelled)->Task<i64>{const t=go number();try await t;return t}\n"
        "fn broken() throws(Failure)->i64{throw Failure.Bad}\n"
        "fn panicValue()->i64{assert(false,\"panic\\0中\");return 0}\n"
        "export fn numberTask() throws(Cancelled)->Task<i64>{const t=go number();try await t;return t}\n"
        "export fn textTask() throws(Cancelled)->Task<string>{const t=go text();try await t;return t}\n"
        "export fn arrayTask() throws(Cancelled)->Task<Array<string>>{const t=go array();try await t;return t}\n"
        "export fn unitTask() throws(Cancelled)->Task<()>{const t=go unit();try await t;return t}\n"
        "export fn nestedTask() throws(Cancelled)->Task<Task<i64>,Cancelled>{const t=go nested();try await t;return t}\n"
        "export fn errorTask()->Task<i64,Failure>{const t=go broken();try{try await t}catch(e){};return t}\n"
        "export fn panicTask() throws(Cancelled)->Task<i64>{const t=go panicValue();try{try await t}catch panic(p){};return t}\n";
    static const char *names[]={"numberTask","textTask","arrayTask","unitTask","nestedTask","errorTask","panicTask"};
    XrSourceCompileOptions options=xray_source_compile_options_default();
    options.stdlib_path=stdlib_path;
    XrSourceExecution *execution=sdk_execution_new();XrSourceArtifact *program=NULL;
    const XrSourceText text={NULL,"installed-terminal-tasks",source,sizeof(source)-1};
    XrSourceInput input;memset(&input,0,sizeof(input));input.kind=XR_SOURCE_INPUT_TEXT;input.value.text=text;
    CHECK(xray_source_build(&input,&options,&program,NULL,NULL)==XR_SOURCE_OK && program);
    XrSourceCallOptions runtime;
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    XrSourceValue *tasks[7]={NULL};XrSourceResult *survivors[7]={NULL};
    XrSourceInvocation missing_options=request(names[0],NULL,0);XrSourceResult *unpublished=NULL;
    CHECK(xray_source_invoke(execution,program,&missing_options,NULL,&unpublished,NULL)==XR_SOURCE_BAD_ARGUMENT && !unpublished);
    for(size_t i=0;i<7;++i) {
        XrSourceInvocation invocation=request(names[i],NULL,0);XrSourceResult *result=NULL;
        CHECK(xray_source_invoke(execution,program,&invocation,&runtime,&result,NULL)==XR_SOURCE_OK && result);
        XrSourceEvent event;
        CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
        tasks[i]=sdk_task_value(result,XR_SOURCE_VALUE_TASK);xray_source_result_free(result);
    }
    xray_source_artifact_free(program);xray_source_execution_free(execution);
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
    sdk_task_language_cases(stdlib_path);
    puts("Installed terminal Task outcomes: repeated owned data, nested Task42, thrown and NUL/UTF8 panic after all producers PASS");
}
