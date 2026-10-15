/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_submission.h - Paired Source and Instance candidate publication
 */
#ifndef XSOURCE_SUBMISSION_H
#define XSOURCE_SUBMISSION_H
#include "../../include/xray_source.h"
#include "../base/xcompile_state.h"
#include "../xir/xxir_source_generation.h"
#include "../xir/xxir_instance_generation.h"

typedef struct XrSourceSubmission XrSourceSubmission;
typedef struct XrSourceSubmissionConfig {
    XrCompileState *state;
    XrXirSourceGenerationConfig source;
    XrXirTarget target;
    XrXirInstanceConfig runtime;
} XrSourceSubmissionConfig;
/* State and source.context must name the same cumulative ledger. The owner
 * retains state and copies runtime configuration; provider contexts, Catalog
 * and lockfile stay caller-owned until drop. One candidate excludes another
 * preparation. This coordinator does not synthesize historical source bindings. */
XR_FUNC XrSourceStatus xr_source_submission_new(const XrSourceSubmissionConfig *, XrSourceSubmission **);
XR_FUNC XrSourceStatus xr_source_submission_prepare(XrSourceSubmission *, const XrXirSourceText *, XrXirSourceDiagnostic *);
/* Poll output borrows until the next successful state change. Take copies one
 * genuine terminal outcome; failed copy preserves the terminal and output. */
XR_FUNC XrSourceStatus xr_source_submission_poll(XrSourceSubmission *, uint64_t quantum, XrXirInstanceResult *);
XR_FUNC XrSourceStatus xr_source_submission_wait(const XrSourceSubmission *, uint64_t epoch, uint64_t wake, XrXirWaitRequest *);
XR_FUNC XrSourceStatus xr_source_submission_resume(XrSourceSubmission *, uint64_t epoch, uint64_t wake);
XR_FUNC XrSourceStatus xr_source_submission_cancel(XrSourceSubmission *);
XR_FUNC XrSourceStatus xr_source_submission_take(XrSourceSubmission *, XrXirCallResult *);
/* Only the actual normally returned candidate can publish. Both commits spend
 * no resources. Abort drains the Instance before releasing the Source record;
 * old state and cumulative charges remain. All callback reentry returns BUSY. */
XR_FUNC XrSourceStatus xr_source_submission_publish(XrSourceSubmission *);
XR_FUNC XrSourceStatus xr_source_submission_abort(XrSourceSubmission *, XrXirCallStatus *cleanup);
XR_FUNC XrSourceStatus xr_source_submission_lookup(const XrSourceSubmission *, const char *, XrXirSourceGenerationBinding *);
XR_FUNC XrSourceStatus xr_source_submission_drop(XrSourceSubmission **, XrXirCallStatus *cleanup);
#endif // XSOURCE_SUBMISSION_H
