/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_array_print_public.c - Real Source argument snapshots and bounded display
 */
#include "xray_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
typedef struct PrintCapture { char bytes[2048]; size_t used, calls; } PrintCapture;
static XrSourceWriteStatus capture(void *context, XrSourceOutputStream stream, const char *bytes,size_t length) {
    PrintCapture *out=context; CHECK(stream==XR_SOURCE_STDOUT && length<=sizeof(out->bytes)-out->used);
    memcpy(out->bytes+out->used,bytes,length); out->used+=length; ++out->calls; return XR_SOURCE_WRITE_OK;
}
static void source_case(const char *source, const char *expected, size_t length, size_t lines) {
    XrSourceCompilerOptions options;
    CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_FORMAT_STDLIB;
    options.allocated_bytes=UINT64_C(64)<<20; options.live_bytes=UINT64_C(8)<<20; options.work=UINT64_C(128000000);
    XrSourceCompiler *compiler=NULL;
    CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);
    XrSourceText text={NULL,"array-print-entry",source,strlen(source)};
    XrSourceProgram *program=NULL; XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_compile_text(compiler,&text,&program,&diagnostic);
    if (diagnostic) {
        XrSourceDiagnosticInfo info; CHECK(xray_source_diagnostic_info(diagnostic,&info)==XR_SOURCE_OK);
        if (!expected) CHECK(status==XR_SOURCE_REJECTED && !program &&
            !strcmp(info.message,"print requires an admitted display type"));
        else fprintf(stderr,"compile status=%d %s\n",status,info.message);
    }
    if (!expected) {
        CHECK(diagnostic && !program); xray_source_diagnostic_free(diagnostic);
        xray_source_compiler_free(compiler); return;
    }
    CHECK(status==XR_SOURCE_OK && program && !diagnostic); xray_source_compiler_free(compiler);
    XrSourceCallOptions call; CHECK(xray_source_call_options_init(&call,sizeof(call))==XR_SOURCE_OK);
    PrintCapture out={0}; call.write=capture; call.context=&out;
    XrSourceResult *result=NULL; CHECK(xray_source_execute(program,&call,&result,NULL)==XR_SOURCE_OK && result);
    xray_source_program_free(program);
    XrSourceEvent event; CHECK(xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED);
    CHECK(out.calls==lines && out.used==length && !memcmp(out.bytes,expected,length));
    xray_source_result_free(result);
}
int main(void) {
    static const char order[]="[1, 2] 7 [9, 2]\n[9, 2]\n";
    source_case("var a: Array<i64> = [1, 2]\nfn later():i64 { a[0]=9; return 7 }\nprint(a,later(),a)\nprint(a)\n",
        order,sizeof(order)-1,2);
    static const char quoted[]="[\"A\0\xE4\xB8\xAD\", \"Z\"]\n[[[[...]]]]\n";
    source_case("print([\"A\\0中\",\"Z\"])\nvar a:Array<Array<Array<Array<Array<i64>>>>> = [[[[[5]]]]]\nprint(a)\n",
        quoted,sizeof(quoted)-1,2);
    static const char clipped[]="[7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, ...(1 more)]\n";
    source_case("print(Array<i64>(33,7))\n",clipped,sizeof(clipped)-1,1);
    static const char leaves[]="[true, false]\n[1.5, -0.0]\n['\xE4\xB8\xAD', 'A']\n[DisplayCase.Empty, DisplayCase.Payload(-32768, [1, 2])]\n[null, 5]\n";
    source_case("print([true,false])\nvar f:Array<f32>=[1.5,-0.0]\nprint(f)\nprint(['中','A'])\n"
        "enum DisplayCase { Empty\nPayload { n:i16, values:Array<i64> } }\n"
        "var e:Array<DisplayCase>=[DisplayCase.Empty,DisplayCase.Payload { n:-32768,values:[1,2] }]\nprint(e)\n"
        "var o:Array<i64?>=[null,5]\nprint(o)\n",leaves,sizeof(leaves)-1,5);
    source_case("fn f() {}\nprint([f])\n",NULL,0,0);
    puts("Real Source Array print: ordered argument snapshots, NUL/UTF8, depth/element bounds, exact group writes and unsupported leaves rejected");
    return 0;
}
