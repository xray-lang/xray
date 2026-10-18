/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_binding_history_support.h - Actual paired publication and scalar observations
 */
#ifndef SOURCE_BINDING_HISTORY_SUPPORT_H
#define SOURCE_BINDING_HISTORY_SUPPORT_H
static XrSourceStatus reads_publish(SubmissionOwners *owned,const char *source) {
    XrXirSourceText text={NULL,source,strlen(source)};XrXirSourceDiagnostic diagnostic={0};
    XrSourceStatus status=xr_source_submission_prepare(owned->owner,&text,&diagnostic);
    if(status!=XR_SOURCE_OK) {
        if(status!=XR_SOURCE_BUDGET && status!=XR_SOURCE_OUT_OF_MEMORY)
            fprintf(stderr,"reader prepare=%u source=%u line=%d message=%s\n",status,diagnostic.status,diagnostic.line,diagnostic.message);
        return status;
    }
    XrXirCallStatus terminal=submission_finish(owned);
    status=xr_source_submission_take(owned->owner,&owned->result);if(status!=XR_SOURCE_OK)return status;
    if(terminal!=XR_XIR_CALL_RETURNED) {
        fprintf(stderr,"history execution terminal=%u panic=%u\n",(unsigned)terminal,owned->result.panic.detail.code);
        return submission_call(terminal);
    }
    size_t attempts=runtime_attempts;status=xr_source_submission_publish(owned->owner);
    CHECK(runtime_attempts==attempts);xr_xir_call_result_drop(&owned->result);return status;
}
static void reads_scalar(SubmissionOwners *owned,const char *name,uint32_t type,int64_t payload) {
    XrXirValue value={0};CHECK(xr_source_submission_read(owned->owner,name,&value)==XR_SOURCE_OK);
    CHECK(value.type==type && !value.reserved && value.payload==payload);xr_xir_value_drop(&value);
}
#endif // SOURCE_BINDING_HISTORY_SUPPORT_H
