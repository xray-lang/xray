/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_legacy_script.h - Original script assertions through public owners
 */
#ifndef SOURCE_LEGACY_SCRIPT_H
#define SOURCE_LEGACY_SCRIPT_H
#include "xray_source.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int legacy_source_script(const char *source,const char *absolute_path,
    const char *namespace_id) {
    XrSourceCompilerOptions options;
    if(xray_source_compiler_options_init(&options,sizeof(options))!=XR_SOURCE_OK)return -1;
    options.stdlib_path=XR_PUBLIC_STDLIB;
    XrSourceCompiler *compiler=NULL;
    XrSourceStatus status=xray_source_compiler_new(&options,&compiler);
    if(status!=XR_SOURCE_OK)return -1;
    XrSourceProgram *program=NULL;XrSourceDiagnostic *diagnostic=NULL;
    XrSourceText text={NULL,namespace_id,source,source?strlen(source):0};
    status=absolute_path?xray_source_compile_file(compiler,absolute_path,&program,&diagnostic):
        xray_source_compile_text(compiler,&text,&program,&diagnostic);
    if(diagnostic) {
        XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};
        if((xray_source_diagnostic_report(diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK)
            fprintf(stderr,"original script compile status=%u line=%d path=%s message=%s\n",
                (unsigned)status,info.primary.line,info.primary.path?info.primary.path:"",info.message?info.message:"");
    }
    xray_source_diagnostic_free(diagnostic);
    xray_source_compiler_free(compiler);
    if(status!=XR_SOURCE_OK) {xray_source_program_free(program);return -1;}
    XrSourceCallOptions runtime;XrSourceResult *result=NULL;XrSourceEvent failure={0};
    status=xray_source_call_options_init(&runtime,sizeof(runtime));
    if(status==XR_SOURCE_OK)status=xray_source_execute(program,&runtime,&result,&failure);
    xray_source_program_free(program);
    XrSourceEvent event={0};int64_t value=-1;
    bool returned=status==XR_SOURCE_OK && result &&
        xray_source_result_event(result,&event)==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_RETURNED &&
        xray_source_result_i64(result,&value)==XR_SOURCE_OK && value==0;
    if(!returned)fprintf(stderr,"original script execution status=%u event=%u engine=%u\n",
        (unsigned)status,(unsigned)failure.state,failure.engine_status);
    xray_source_result_free(result);return returned?0:-1;
}
#endif // SOURCE_LEGACY_SCRIPT_H
