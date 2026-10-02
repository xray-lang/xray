/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_inference_result.inc.c - Late result-context evidence
 *
 * KEY CONCEPT:
 *   Real context fills only unresolved result dependencies after value evidence;
 *   the original call result still undergoes the existing authorized conversion.
 */
#include "xxir_type_inference.h"
static bool source_inference_result(SourceContext *ctx, AstNode *site,
    XrXirInferenceState *state, XrXirType formal, SourceExpectedType context) {
    /* Presence is independent of the expected type, including Unit. */
    if (!context.present) return true;
    XrXirType expected = context.type;
    XrXirInferenceKnown known = {0};
    XrXirStatus status = xr_xir_compile_inference_expected_known(state, &ctx->types, formal, &known);
    if (status != XR_XIR_OK) return source_fail(ctx,site,status,"inferred result context is invalid");
    /* Preserve numeric widening and root callable weakening of known results. */
    if (known.known) return true;
    XrXirType evidence = formal;
    const XrXirTypeNode *from = xr_xir_callable_signature(&ctx->types,formal);
    const XrXirTypeNode *to = xr_xir_callable_signature(&ctx->types,expected);
    if (from && to && from->flags != to->flags) {
        if (from->flags != XR_XIR_CALLABLE_NO_SUSPEND || to->flags)
            return source_fail(ctx,site,XR_XIR_BAD_TYPE,"result context cannot strengthen a callable promise");
        XrXirTypeNode ordinary = *from; ordinary.flags = 0;
        if (!source_intern_type(ctx,ordinary,&evidence)) return false;
    }
    status = xr_xir_compile_inference_observe(state, &ctx->types, (XrXirInferencePair){evidence,expected});
    if (status != XR_XIR_OK) return source_fail(ctx,site,status,"argument and result type evidence conflict");
    return true;
}
