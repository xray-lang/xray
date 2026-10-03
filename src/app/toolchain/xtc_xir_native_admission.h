/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_native_admission.h - Admit one owned local native transaction
 */
#ifndef XTC_XIR_NATIVE_ADMISSION_H
#define XTC_XIR_NATIVE_ADMISSION_H
#include "xtc_xir_native_operation.h"

typedef enum XtcXirNativeAdmissionStatus {
    XTC_XIR_ADMISSION_OK, XTC_XIR_ADMISSION_INVALID, XTC_XIR_ADMISSION_UNRESOLVED,
    XTC_XIR_ADMISSION_UNSUPPORTED, XTC_XIR_ADMISSION_MISMATCH,
    XTC_XIR_ADMISSION_BUDGET, XTC_XIR_ADMISSION_OUT_OF_MEMORY,
    XTC_XIR_ADMISSION_IO
} XtcXirNativeAdmissionStatus;
typedef enum XtcXirNativeAdmissionDomain {
    XTC_XIR_ADMISSION_SELF, XTC_XIR_ADMISSION_RESOURCE,
    XTC_XIR_ADMISSION_OPERATION, XTC_XIR_ADMISSION_SDK,
    XTC_XIR_ADMISSION_PE, XTC_XIR_ADMISSION_BINDING,
    XTC_XIR_ADMISSION_XIR
} XtcXirNativeAdmissionDomain;
typedef struct XtcXirNativeAdmissionDiagnostic {
    XtcXirNativeAdmissionStatus status;
    XtcXirNativeAdmissionDomain domain;
    int code;
    XrXirInvocationStage stage;
    XtcXirNativeOperationDiagnostic operation;
} XtcXirNativeAdmissionDiagnostic;

/* Borrow three live owners synchronously on their one shared ledger. Operation
 * must be READY. Admit only the controlled local Windows x64 MSVC C11/MD recipe;
 * installed toolchain/search directories are trusted not to change concurrently.
 * This does not authenticate a provider or prove negative filesystem lookups.
 * Read the held OUTPUT once, then seal the existing native artifact. No provider
 * runs, source emission or file reopens occur here. Output must initially be NULL
 * and is preserved on failure. A failed output read keeps Operation's first error
 * and FAILED state. Other admission failures do not change that owner.
 * The result independently owns its context and bytes. The caller must finish
 * operation close, preserving it on PENDING, before publishing/executing bytes. */
XR_FUNC XtcXirNativeAdmissionStatus xtc_xir_native_admit(
    XtcXirNativeOperation *operation, const XrXirNativeProjection *projection,
    const XrXirRuntimeSdk *sdk, uint64_t byte_limit,
    XrXirNativeArtifact **output, XtcXirNativeAdmissionDiagnostic *diagnostic);
#endif // XTC_XIR_NATIVE_ADMISSION_H