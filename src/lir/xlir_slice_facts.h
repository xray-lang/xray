/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_slice_facts.h - Owned numeric contracts for readonly Array loans
 *
 * KEY CONCEPT:
 *   The receiving module owns the existing checked WRITE and ORIGIN summary.
 *   Physical keepers implement those loans without becoming their authority.
 */
#ifndef XLIR_SLICE_FACTS_H
#define XLIR_SLICE_FACTS_H
#include "xlir_view.h"

typedef enum XrLirSliceStorage {
    XR_LIR_SLICE_STORAGE_NONE,
    XR_LIR_SLICE_STORAGE_BORROWED_FORMAL,
    XR_LIR_SLICE_STORAGE_LOCAL_KEEPER
} XrLirSliceStorage;

typedef enum XrLirSliceEffectKind {
    XR_LIR_SLICE_EFFECT_SLOT,
    XR_LIR_SLICE_EFFECT_CALLBACK,
    XR_LIR_SLICE_EFFECT_CAPTURE,
    XR_LIR_SLICE_EFFECT_UNKNOWN,
    XR_LIR_SLICE_EFFECT_FIELD,
    XR_LIR_SLICE_EFFECT_PARAMETER
} XrLirSliceEffectKind;

typedef struct XrLirSliceEffect {
    XrLirSliceEffectKind kind;
    /* SLOT is an actual receiving State slot. PARAMETER/CAPTURE/CALLBACK use
     * the actual physical parameter ordinal, including capture prefixes. */
    uint32_t identity;
} XrLirSliceEffect;

typedef struct XrLirSliceEffectSpan { size_t first, count; } XrLirSliceEffectSpan;

/* The actual caller must discharge CALLBACK(callback_parameter) against the
 * live readonly entry loan PARAMETER/CAPTURE(loan_parameter). This relation
 * carries no runtime address, capture environment or permission by itself. */
typedef struct XrLirSliceCallbackLoan {
    uint32_t callback_parameter;
    XrLirSliceEffectKind loan_kind;
    uint32_t loan_parameter;
} XrLirSliceCallbackLoan;

typedef struct XrLirSliceFunctionFact {
    uint32_t function, parameter_count;
    /* UINT32_MAX for an ordinary result or an unresolved borrowed origin.
     * A Slice result requires one exact same-typed READ formal. The loan
     * verifier also checks every actual returned source against that origin. */
    uint32_t result_parameter;
    bool available;
    XrLirSliceEffectSpan access, write, result_origin, callback_loans;
} XrLirSliceFunctionFact;

typedef struct XrLirSliceFactsView {
    /* Indexed by one-based function id, including the empty zero row. An
     * absent row has function zero; unavailable WRITE is never empty proof. */
    const XrLirSliceFunctionFact *functions;
    const XrLirSliceEffect *terms;
    const XrLirSliceCallbackLoan *callback_loans;
    uint32_t function_count;
    size_t term_count, callback_loan_count;
} XrLirSliceFactsView;

typedef struct XrLirSliceFacts XrLirSliceFacts;

/* Copy the exact finished module's already received numeric summaries. No
 * Effects, Artifact, AST, compiler pool or per-value origin table is retained.
 * Scalar modules have no owner and leave an empty output NULL. A receiving
 * copy survives source-module retirement. Failure publishes nothing. */
XR_FUNC XrLirStatus xr_lir_slice_facts_create(const XrLirModule *, XrLirSliceFacts **empty);
XR_FUNC void xr_lir_slice_facts_destroy(XrLirSliceFacts *);
XR_FUNC XrLirStatus xr_lir_slice_facts_view(const XrLirSliceFacts *, XrLirSliceFactsView *);
/* The view borrows this exact immutable module; it is not an external raw
 * execution interface or a caller-selectable loan permission. */
XR_FUNC XrLirStatus xr_lir_slice_function_fact(const XrLirModule *, uint32_t function,
    XrLirSliceFunctionFact *);
#endif // XLIR_SLICE_FACTS_H
