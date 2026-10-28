/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_array_format_public.c - Format a real escaped Array payload after code death
 */
#include "xray_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
typedef struct Capture {
    char bytes[512]; size_t used, writes, fail_at;
    XrSourceWriteStatus failure;
} Capture;
static XrSourceWriteStatus capture_write(void *context, XrSourceOutputStream stream,
    const char *bytes, size_t length) {
    Capture *out = context;
    CHECK(stream==XR_SOURCE_STDERR);
    if (out->writes++==out->fail_at) return out->failure;
    CHECK(length<=sizeof(out->bytes)-out->used);
    memcpy(out->bytes+out->used,bytes,length); out->used+=length;
    return XR_SOURCE_WRITE_OK;
}
static void check_bytes(const Capture *out) {
    static const char expected[] = "[Uncaught Error] Failure.Bad([-9223372036854775808, 9223372036854775807], [\"A\0\xE4\xB8\xAD\"], [[1, 2], []])\n";
    CHECK(out->used==sizeof(expected)-1 && !memcmp(out->bytes,expected,sizeof(expected)-1));
}
int main(void) {
    XrSourceCompilerOptions options;
    CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_FORMAT_STDLIB;
    options.allocated_bytes=UINT64_C(64)<<20; options.live_bytes=UINT64_C(8)<<20;
    options.work=UINT64_C(128000000);
    XrSourceCompiler *compiler=NULL;
    CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);
    XrSourceProgram *program=NULL; XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_compile_file(compiler,XR_FORMAT_FIXTURE,&program,&diagnostic);
    if (diagnostic) {
        XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};
        CHECK((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
        fprintf(stderr,"compile status=%d %s\n",status,info.message);
    }
    CHECK(status==XR_SOURCE_OK && program && !diagnostic);
    xray_source_compiler_free(compiler);
    XrSourceCallOptions call_options;
    CHECK(xray_source_call_options_init(&call_options,sizeof(call_options))==XR_SOURCE_OK);
    Capture output={.fail_at=SIZE_MAX}; call_options.write=capture_write; call_options.context=&output;
    XrSourceResult *result=NULL;
    CHECK(xray_source_execute(program,&call_options,&result,NULL)==XR_SOURCE_OK && result && !output.writes);
    xray_source_program_free(program);
    CHECK(xray_source_result_format(result,capture_write,&output)==XR_SOURCE_OK); check_bytes(&output);
    const XrSourceWriteStatus failures[]={XR_SOURCE_WRITE_OOM,XR_SOURCE_WRITE_LIMIT,XR_SOURCE_WRITE_IO};
    const XrSourceStatus statuses[]={XR_SOURCE_OUT_OF_MEMORY,XR_SOURCE_LIMIT,XR_SOURCE_IO};
    for (size_t kind=0;kind<3;++kind) for (size_t site=0;site<output.writes;++site) {
        Capture failed={.fail_at=site,.failure=failures[kind]};
        CHECK(xray_source_result_format(result,capture_write,&failed)==statuses[kind] && failed.writes==site+1);
    }
    output=(Capture){.fail_at=SIZE_MAX};
    CHECK(xray_source_result_format(result,capture_write,&output)==XR_SOURCE_OK); check_bytes(&output);
    xray_source_result_free(result);
    puts("Public Source Array result: exact escaped bytes, code-owner death and every write failure passed");
    return 0;
}
