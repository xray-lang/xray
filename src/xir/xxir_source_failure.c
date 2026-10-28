/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_failure.c - Independently owned Source failure display facts
 */
#include "xxir_source_query.h"
#include <string.h>
XR_FUNC bool xr_xir_compile_source_failure_empty(const XrXirSourceFailure *failure) {
    return !failure||(!failure->report&&!failure->path&&failure->origin==XR_XIR_SOURCE_DIAGNOSTIC_DEFAULT&&
        !failure->has_utf16_position&&!failure->utf16_line&&!failure->utf16_column);
}
XR_FUNC void xr_xir_compile_source_failure_free(XrXirSourceFailure *failure) {
    if(!failure)return;
    xr_frontend_report_free(failure->report);
    xr_compile_resources_free(failure->path);*failure=(XrXirSourceFailure){0};
}
XR_FUNC XrCompileResourceStatus xr_xir_compile_source_failure_record(XrCompileResources *resources,
    XrXirSourceFailure *failure,const XrXirSourceDiagnostic *fact,const char *path,
    const char *text,size_t length) {
    if(!failure||!fact)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if(failure->report)return XR_COMPILE_RESOURCE_OK;
    XrFrontendReport *report=NULL;uint32_t source=0;
    XrCompileResourceStatus status=xr_frontend_report_new(resources,&report);
    if(status==XR_COMPILE_RESOURCE_OK)status=xr_frontend_report_source(report,path,text,length,NULL,&source);
    if(status==XR_COMPILE_RESOURCE_OK) {
        XrFrontendLocation location=xr_frontend_report_location(report,source,fact->line,fact->column,NULL);
        if(failure->has_utf16_position) {
            location.flags|=XR_FRONTEND_LOCATION_UTF16;
            location.utf16_line=location.utf16_end_line=failure->utf16_line;
            location.utf16_column=location.utf16_end_column=failure->utf16_column;
        }
        status=xr_frontend_report_add(report,0,fact->message[0]?fact->message:"source checking failed",location,NULL,0);
    }
    if(status!=XR_COMPILE_RESOURCE_OK){xr_frontend_report_free(report);return status;}
    xr_frontend_report_seal(report,false);failure->report=report;return status;
}
