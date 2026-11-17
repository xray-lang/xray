/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_callback_loan_internal.h - Conditional entry-loan obligations
 */
#ifndef XXIR_EFFECT_CALLBACK_LOAN_INTERNAL_H
#define XXIR_EFFECT_CALLBACK_LOAN_INTERNAL_H
#include "xxir_effect_module_export_internal.h"
struct XrXirViewLiveEvent;

/* The existing whole-module liveness visit observes an admitted immutable
 * body. Actual nonescaping formal receivers and live readonly Slice values
 * produce body seeds in the same unpublished ROOT owner. Its existing ORIGIN
 * equations project them to entry pairs during the ordinary close, including
 * derived views whose original formal is no longer live. This records a
 * condition; neither it nor completion grants an entry or call. */
XR_FUNC XrXirStatus xir_effects_callback_loan_observe(const XrXirCompileContext *,
    const XrXirModule *,const XrXirEffects *,const struct XrXirViewLiveEvent *);
XR_FUNC XrXirStatus xir_effects_callback_loan_observed(const XrXirCompileContext *,
    const XrXirModule *,const XrXirEffects *);

/* A pair is usable only after its producer and actual-call substitution
 * protocol have both closed. Unknown or absent pairs leave the output false;
 * there is no Source-accepted bit and no writable caller-selected table. */
XR_FUNC XrXirStatus xir_effects_callback_loan_contract(const XrXirCompileContext *,
    const XrXirModule *,const XrXirEffects *,uint32_t function,uint32_t callback,
    XirModuleEffectTermKind loan_kind,uint32_t loan,bool *);

typedef struct XirCallbackLoanApplication {
    uint32_t function,instruction,callback,loan;
} XirCallbackLoanApplication;
typedef XrXirStatus (*XirCallbackLoanApplicationVisitor)(void *,const XirCallbackLoanApplication *);
/* Visits this actual call's completed ROOT argument/capture applications.
 * Both values are real caller-local physical SSA ordinals; a missing or
 * foreign representative is unresolved, never an invented caller binding.
 * Each callback expires synchronously and conveys no borrowed permission. */
XR_FUNC XrXirStatus xir_effects_callback_loan_applications(const XrXirCompileContext *,
    const XrXirModule *,const XrXirEffects *,uint32_t function,uint32_t instruction,
    XirCallbackLoanApplicationVisitor,void *);
XR_FUNC XrXirStatus xir_effects_callback_loan_receiver(const XrXirCompileContext *,
    const XrXirModule *,const XrXirEffects *,uint32_t function,uint32_t value,uint32_t *formal,bool *);
#endif
