/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_repl_original_support.h - Authentic owners for original REPL expectations
 */
#ifndef SOURCE_REPL_ORIGINAL_SUPPORT_H
#define SOURCE_REPL_ORIGINAL_SUPPORT_H
#include "../test_framework.h"
#include "api/xsource_api.c"
#include "api/xsource_submission.c"
#include <string.h>

typedef struct OriginalReplFixture {
    XrSourceRepl *owner;
    XrSourceResult *results[16];
    XrSourceValue *values[16];
    size_t result_count,value_count,output_length;
    XrSourceDiagnostic *diagnostic;
    XrSourceEvent event;
    char output[4096];
} OriginalReplFixture;
static OriginalReplFixture fixture;
static XrSourceWriteStatus original_output(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    OriginalReplFixture *f=context;
    if(stream!=XR_SOURCE_STDOUT || length>=sizeof(f->output)-f->output_length)return XR_SOURCE_WRITE_LIMIT;
    memcpy(f->output+f->output_length,bytes,length);f->output_length+=length;f->output[f->output_length]=0;return XR_SOURCE_WRITE_OK;
}
static void original_open(void) {
    XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    ASSERT_EQ_INT(xray_source_compiler_options_init(&compiler,sizeof(compiler)),XR_SOURCE_OK);
    ASSERT_EQ_INT(xray_source_call_options_init(&runtime,sizeof(runtime)),XR_SOURCE_OK);
    compiler.stdlib_path=XR_ORIGINAL_REPL_STDLIB;runtime.write=original_output;runtime.context=&fixture;
    ASSERT_EQ_INT(xray_source_repl_new(&compiler,"repl-fixture-v1",&runtime,&fixture.owner),XR_SOURCE_OK);
}
static void original_close(void) {
    XrSourceEvent cleanup={0};
    XrSourceStatus status=xray_source_repl_drop(&fixture.owner,&cleanup);
    for(size_t i=0;i<fixture.result_count;++i)xray_source_result_free(fixture.results[i]);
    for(size_t i=0;i<fixture.value_count;++i)xray_source_value_free(fixture.values[i]);
    xray_source_diagnostic_free(fixture.diagnostic);
    memset(&fixture,0,sizeof(fixture));
    ASSERT_EQ_INT(status,XR_SOURCE_OK);ASSERT_EQ_INT(cleanup.state,XR_SOURCE_RUN_READY);
}
static XrSourceStatus original_submit(const char *source,bool query) {
    xray_source_diagnostic_free(fixture.diagnostic);fixture.diagnostic=NULL;
    fixture.event=(XrSourceEvent){0};const XrSourceText input={NULL,"repl-fixture-v1",source,source?strlen(source):0};
    XrSourceStatus status=query?xray_source_repl_begin_type(fixture.owner,&input,&fixture.diagnostic):
        xray_source_repl_begin(fixture.owner,&input,&fixture.diagnostic);
    if(status!=XR_SOURCE_OK)return status;
    unsigned polls=0;
    do{status=xray_source_repl_poll(fixture.owner,16,&fixture.event);}
    while(status==XR_SOURCE_OK && fixture.event.state==XR_SOURCE_RUN_READY && ++polls<100000);
    if(status!=XR_SOURCE_OK)return status;
    if(fixture.event.state==XR_SOURCE_RUN_READY || fixture.event.state==XR_SOURCE_RUN_SUSPENDED)return XR_SOURCE_BAD_STATE;
    if(fixture.result_count==sizeof(fixture.results)/sizeof(*fixture.results))return XR_SOURCE_LIMIT;
    status=xray_source_repl_take(fixture.owner,&fixture.results[fixture.result_count]);
    if(status==XR_SOURCE_OK)++fixture.result_count;
    return status;
}
static bool original_eval(const char *source) {
    XrSourceStatus status=original_submit(source,false);
    if(status!=XR_SOURCE_OK || fixture.event.state!=XR_SOURCE_RUN_RETURNED) {
        XrSourceDiagnosticInfo info={0};
        if(fixture.diagnostic && xray_source_diagnostic_info(fixture.diagnostic,&info)==XR_SOURCE_OK)
            fprintf(stderr,"original input status=%u at%d:%d: %s; source=%s\n",(unsigned)status,info.line,info.column,info.message,source);
        else fprintf(stderr,"original input status=%u event=%u; source=%s\n",(unsigned)status,(unsigned)fixture.event.state,source);
        return false;
    }
    return true;
}
static const XrSourceReplBindingInfo *original_find(const char *name) {
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    if(xray_source_repl_bindings(fixture.owner,&rows,&count)!=XR_SOURCE_OK)return NULL;
    for(size_t i=0;i<count;++i)if(!strcmp(rows[i].name,name))return &rows[i];
    return NULL;
}
static size_t original_names(void) {
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0,names=0;
    if(xray_source_repl_bindings(fixture.owner,&rows,&count)!=XR_SOURCE_OK)return SIZE_MAX;
    for(size_t i=0;i<count;++i)if(!rows[i].implicit_result)++names;
    return names;
}
static bool original_integer(const char *name,int64_t expected) {
    XrSourceValue *value=NULL;XrSourceScalar scalar={0};
    XrSourceStatus status=xray_source_repl_read(fixture.owner,name,&value);
    if(status==XR_SOURCE_OK)status=xray_source_value_scalar(value,&scalar);
    bool equal=status==XR_SOURCE_OK && scalar.kind==XR_SOURCE_VALUE_I64 && scalar.value.signed_value==expected;
    xray_source_value_free(value);return equal;
}
static XrSourceValue *original_scalar(int64_t number) {
    if(fixture.value_count==sizeof(fixture.values)/sizeof(*fixture.values))return NULL;
    XrSourceScalar scalar={.kind=XR_SOURCE_VALUE_I64};scalar.value.signed_value=number;
    XrSourceValue *value=NULL;
    if(xray_source_value_new_scalar(fixture.owner->compiler,&scalar,&value)!=XR_SOURCE_OK)return NULL;
    fixture.values[fixture.value_count++]=value;return value;
}
static bool original_string(const char *name,const char *expected) {
    XrSourceValue *value=NULL;const char *bytes=NULL;size_t length=0;
    XrSourceStatus status=xray_source_repl_read(fixture.owner,name,&value);
    if(status==XR_SOURCE_OK)status=xray_source_value_bytes(value,&bytes,&length);
    bool equal=status==XR_SOURCE_OK && length==strlen(expected) && !memcmp(bytes,expected,length);
    xray_source_value_free(value);return equal;
}
static size_t original_births(bool results_only,uint64_t *latest) {
    size_t count=0;if(latest)*latest=0;
    for(const SubmissionBirth *birth=fixture.owner->submission->published_births;birth;birth=birth->previous) {
        if(latest && birth->source>*latest)*latest=birth->source;
        XrXirSourceGenerationView view={0};
        if(xr_xir_compile_source_generation_view(fixture.owner->submission->source,birth->source,&view)!=XR_XIR_OK)return SIZE_MAX;
        if(!results_only || view.repl_result)++count;
    }
    return count;
}
#undef RUN_TEST
#define RUN_TEST(name) do { \
    unsigned previous=(unsigned)xr_tests_failed; ++xr_tests_run; \
    printf("  %-55s ",#name);fflush(stdout);test_##name();original_close(); \
    if((unsigned)xr_tests_failed==previous){++xr_tests_passed;puts("PASS");} \
} while(0)
#endif // SOURCE_REPL_ORIGINAL_SUPPORT_H
