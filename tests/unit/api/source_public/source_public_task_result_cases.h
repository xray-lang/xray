/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_task_result_cases.h - Real terminal Task data without execution permission
 */
#ifndef SOURCE_PUBLIC_TASK_RESULT_CASES_H
#define SOURCE_PUBLIC_TASK_RESULT_CASES_H
#include "source_public_task_result_fixture.h"
static XrSourceValue *task_result_handle(XrSourceProgram *program,const char *name) {
    XrSourceInvocation request=invocation(name,NULL,0);
    XrSourceValue *value=project_result(invoke(program,&request,NULL));
    XrSourceValueKind kind;
    CHECK(xray_source_value_kind(value,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_TASK);return value;
}
static void task_result_cases(void) {
    XrSourceCompiler *compiler=compiler_new();XrSourceProgram *program=compile(compiler,task_result_source,"public-task-results");
    static const char *names[]={"numberTask","textTask","arrayTask","unitTask","nestedTask","errorTask","panicTask"};
    XrSourceValue *tasks[7]={0};XrSourceResult *results[7]={0},*copies[7]={0};
    for(size_t i=0;i<7;++i)tasks[i]=task_result_handle(program,names[i]);
    xray_source_program_free(program);xray_source_compiler_free(compiler);
    for(size_t i=0;i<7;++i) {
        CHECK(xray_source_value_task_result(tasks[i],&results[i])==XR_SOURCE_OK && results[i]);
        CHECK(xray_source_value_task_result(tasks[i],&copies[i])==XR_SOURCE_OK && copies[i]);
        XrSourceResult *occupied=results[i];
        CHECK(xray_source_value_task_result(tasks[i],&occupied)==XR_SOURCE_BAD_ARGUMENT && occupied==results[i]);
        xray_source_value_free(tasks[i]);xray_source_result_free(results[i]);
        XrSourceEvent event;
        CHECK(xray_source_result_event(copies[i],&event)==XR_SOURCE_OK);
        CHECK(event.state==(i==5?XR_SOURCE_RUN_THROWN:i==6?XR_SOURCE_RUN_PANICKED:XR_SOURCE_RUN_RETURNED));
    }
    int64_t number=0;CHECK(xray_source_result_i64(copies[0],&number)==XR_SOURCE_OK && number==42);
    const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_result_bytes(copies[1],&bytes,&length)==XR_SOURCE_OK && length==sizeof("A\0中")-1 && !memcmp(bytes,"A\0中",length));
    Output formatted={0};CHECK(xray_source_result_format(copies[2],collect,&formatted)==XR_SOURCE_OK);
    static const char array_text[]="[\"A\0中\", \"next\"]";
    CHECK(formatted.length==sizeof(array_text)-1 && !memcmp(formatted.bytes,array_text,formatted.length));
    XrSourceValue *value=project_result(copies[3]);copies[3]=NULL;XrSourceValueKind kind;
    CHECK(xray_source_value_kind(value,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_UNIT);
    XrSourceResult *refused=NULL;
    CHECK(xray_source_value_task_result(value,&refused)==XR_SOURCE_INVALID && !refused);xray_source_value_free(value);
    value=project_result(copies[4]);copies[4]=NULL;
    CHECK(xray_source_value_task_result(value,&refused)==XR_SOURCE_OK && refused);xray_source_value_free(value);
    CHECK(xray_source_result_i64(refused,&number)==XR_SOURCE_OK && number==42);xray_source_result_free(refused);refused=NULL;
    value=project_result(copies[5]);copies[5]=NULL;
    CHECK(xray_source_value_kind(value,&kind)==XR_SOURCE_OK && kind==XR_SOURCE_VALUE_ENUM);xray_source_value_free(value);
    CHECK(xray_source_result_bytes(copies[6],&bytes,&length)==XR_SOURCE_OK && length==sizeof("panic\0中")-1 && !memcmp(bytes,"panic\0中",length));
    for(size_t i=0;i<7;++i)xray_source_result_free(copies[i]);
    CHECK(xray_source_value_task_result(NULL,&refused)==XR_SOURCE_BAD_ARGUMENT && !refused);
    puts("public terminal Task results: repeated owned data, nested Task42, typed Error and exact NUL/UTF8 panic after producers/Task die");
}
#endif // SOURCE_PUBLIC_TASK_RESULT_CASES_H
