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
 * body. Only its live entry Slice parameters and actual nonescaping callable
 * formal receivers produce pairs in the same unpublished ROOT owner. This
 * records a condition; neither it nor completion grants an entry or call. */
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
#endif
