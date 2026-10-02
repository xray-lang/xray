/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_publication.h - Owned synchronous replacement of one named output
 */
#ifndef XTC_XIR_PUBLICATION_H
#define XTC_XIR_PUBLICATION_H
#include "../../base/xcompile_resources.h"

typedef struct XtcXirPublication XtcXirPublication;
typedef enum XtcXirPublicationStatus {
    XTC_XIR_PUBLICATION_OK, XTC_XIR_PUBLICATION_INVALID, XTC_XIR_PUBLICATION_UNRESOLVED,
    XTC_XIR_PUBLICATION_UNSUPPORTED, XTC_XIR_PUBLICATION_BUDGET,
    XTC_XIR_PUBLICATION_OUT_OF_MEMORY, XTC_XIR_PUBLICATION_IO, XTC_XIR_PUBLICATION_PENDING
} XtcXirPublicationStatus;
typedef enum XtcXirPublicationPhase {
    XTC_XIR_PUBLICATION_NEW, XTC_XIR_PUBLICATION_WRITING, XTC_XIR_PUBLICATION_READY,
    XTC_XIR_PUBLICATION_PUBLISHED, XTC_XIR_PUBLICATION_FAILED
} XtcXirPublicationPhase;
typedef enum XtcXirPublicationStage {
    XTC_XIR_PUBLICATION_PREPARE, XTC_XIR_PUBLICATION_PARENT, XTC_XIR_PUBLICATION_CREATE,
    XTC_XIR_PUBLICATION_WRITE, XTC_XIR_PUBLICATION_FLUSH, XTC_XIR_PUBLICATION_RENAME,
    XTC_XIR_PUBLICATION_CLEANUP
} XtcXirPublicationStage;
typedef struct XtcXirPublicationLimits {
    uint32_t path_bytes, create_attempts;
    uint64_t max_bytes;
} XtcXirPublicationLimits;
typedef struct XtcXirPublicationRequest {
    const char *destination;
    XtcXirPublicationLimits limits;
} XtcXirPublicationRequest;
typedef struct XtcXirPublicationDiagnostic {
    XtcXirPublicationStatus status;
    XtcXirPublicationStage stage;
    uint32_t os_error, cleanup_os_error;
    bool published, cleanup_pending;
} XtcXirPublicationDiagnostic;

/* Normalize and reserve memory on the mandatory original ledger. There are no
 * directory writes. Output must be NULL and remains unchanged on failure. */
XR_FUNC XtcXirPublicationStatus xtc_xir_publication_new(XrCompileResources *resources,
    const XtcXirPublicationRequest *request, XtcXirPublication **output);
/* Exactly one synchronous borrow. Success owns flushed bytes in an exclusive
 * temporary handle; the source and its producer may immediately be destroyed. */
XR_FUNC XtcXirPublicationStatus xtc_xir_publication_write(XtcXirPublication *owner,
    const void *bytes, size_t length);
/* READY only. Success replaces the authorized named entry. It does not preserve
 * the previous file identity, aliases, or metadata, promise crash durability,
 * or provide compare-and-swap against other writers. */
XR_FUNC XtcXirPublicationStatus xtc_xir_publication_commit(XtcXirPublication *owner);
XR_FUNC XtcXirPublicationPhase xtc_xir_publication_phase(const XtcXirPublication *owner);
XR_FUNC const XtcXirPublicationDiagnostic *xtc_xir_publication_diagnostic(const XtcXirPublication *owner);
/* No allocation or work admission. PENDING retains the owner and every cleanup
 * responsibility for retry. OK frees/nulls. Published output is never deleted.
 * Recoverable cleanup never exits; callers must retain pending owners. The
 * ancestor-lease dependency retains its existing fatal invariant checks. */
XR_FUNC XtcXirPublicationStatus xtc_xir_publication_close(XtcXirPublication **owner);
#endif // XTC_XIR_PUBLICATION_H
