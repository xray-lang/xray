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
#include "../base/xcompile_resources.h"
#include "../xir/xxir_source_generation.h"
#include "../xir/xxir_instance_generation.h"
#include "../xir/xxir_instance_binding.h"

typedef struct XrSourceSubmission XrSourceSubmission;
typedef struct XrSourceSubmissionConfig {
    XrCompileResources *resources;
    XrXirSourceGenerationConfig source;
    XrXirTarget target;
    XrXirInstanceConfig runtime;
} XrSourceSubmissionConfig;
/* Resources and source.context name one persistent semantic memory owner.
 * The owner copies runtime configuration; provider contexts, Catalog
 * and lockfile stay caller-owned until drop. One candidate excludes another
 * preparation. This coordinator does not synthesize historical source bindings. */
XR_FUNC XrSourceStatus xr_source_submission_new(const XrSourceSubmissionConfig *, XrSourceSubmission **);
XR_FUNC XrSourceStatus xr_source_submission_prepare(XrSourceSubmission *, const XrXirSourceText *, XrXirSourceDiagnostic *);
/* Shares the authentic immutable parser owner into the same Source/Instance
 * transaction. Failure preserves the input owner and published state. Success
 * permits producer disposal; history retains the AST until coordinator drop. */
XR_FUNC XrSourceStatus xr_source_submission_prepare_ast(XrSourceSubmission *, const XrXirSourceAstOwner *, XrXirSourceDiagnostic *);
/* Poll output borrows until the next successful state change. Take copies one
 * genuine terminal outcome; failed copy preserves the terminal and output. */
/* Same transaction with genuine interactive Source tail/result ownership. */
XR_FUNC XrSourceStatus xr_source_submission_prepare_repl_ast(XrSourceSubmission *owner,
    const XrXirSourceReplInput *input, XrXirSourceDiagnostic *diagnostic);
/* Temporary type inspection uses real expression execution and must abort. */
XR_FUNC XrSourceStatus xr_source_submission_prepare_repl_type_ast(XrSourceSubmission *,
    const XrXirSourceReplInput *, XrXirSourceDiagnostic *);
/* Borrows the exact pending compiler fact; publication/abort clears it. */
XR_FUNC bool xr_source_submission_repl_result(const XrSourceSubmission *owner);
XR_FUNC XrSourceStatus xr_source_submission_poll(XrSourceSubmission *, uint64_t quantum, XrXirInstanceResult *);
XR_FUNC XrSourceStatus xr_source_submission_wait(const XrSourceSubmission *, uint64_t epoch, uint64_t wake, XrXirWaitRequest *);
XR_FUNC XrSourceStatus xr_source_submission_resume(XrSourceSubmission *, uint64_t epoch, uint64_t wake);
XR_FUNC XrSourceStatus xr_source_submission_cancel(XrSourceSubmission *);
XR_FUNC XrSourceStatus xr_source_submission_take(XrSourceSubmission *, XrXirCallResult *);
/* Only the actual normally returned candidate can publish. Both commits spend
 * no resources. Abort drains the Instance before releasing the Source record;
 * published semantic state remains. All callback reentry returns BUSY. */
XR_FUNC XrSourceStatus xr_source_submission_publish(XrSourceSubmission *);
XR_FUNC XrSourceStatus xr_source_submission_abort(XrSourceSubmission *, XrXirCallStatus *cleanup);
XR_FUNC XrSourceStatus xr_source_submission_lookup(const XrSourceSubmission *, const char *, XrXirSourceGenerationBinding *);
/* Selects the latest published lexical definition and its exact paired birth
 * Program/storage handle. Unit, templates and other nonphysical declarations
 * do not fall through to an older physical definition. Handles borrow the
 * coordinator; actual runtime access still requires its ordinary birth view.
 * Output must be empty and is preserved on failure. */
XR_FUNC XrSourceStatus xr_source_submission_lookup_runtime(const XrSourceSubmission *, const char *,
    const XrXirInstanceBinding **output);
/* Reads the latest published binding by executing its actual checked reader in
 * its birth Program. No candidate may be pending. Output must be canonical Unit
 * and remains unchanged on refusal. The owned value keeps its real arena and
 * survives the coordinator; its normal runtime ownership rules still apply.
 * Reading performs no initializer replay or metadata-based slot access. */
XR_FUNC XrSourceStatus xr_source_submission_read(XrSourceSubmission *, const char *, XrXirValue *output);
/* Borrows an exact typed value for the selected mutable declaration's ordinary
 * birth writer. Instance admission retains arena identity and permissions.
 * No input is published. Cleanup failure stops the real Instance. */
XR_FUNC XrSourceStatus xr_source_submission_write(XrSourceSubmission *, const char *,
    const XrXirValue *, XrXirCallStatus *cleanup);
/* Immutable prepared/published display observations, never runtime authority. */
XR_FUNC XrSourceStatus xr_source_submission_names(const XrSourceSubmission *, bool pending,
    const XrXirSourceGenerationName **, size_t *);
XR_FUNC XrSourceStatus xr_source_submission_drop(XrSourceSubmission **, XrXirCallStatus *cleanup);
#endif // XSOURCE_SUBMISSION_H
