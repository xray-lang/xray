/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_instance_bounds.inc.c - Real instance advertisements
 *
 * KEY CONCEPT:
 *   Complete provenance checking precedes this receiving owner. The source
 *   keeps original instruction advertisements; the authentic selected vector
 *   keeps physical formal bounds. Neither is reconstructed from final SSA flags.
 */

static XrXirStatus effect_invocation_instance_bound(const XrXirCompileContext *work,
    const XirEffectCallableBound *request,EffectInvocationValueBound *output) {
    if (!request || !output || !!request->arguments!=!!request->argument_count)
        return XR_XIR_BAD_STRUCTURE;

    XirEffectCallableBound bound=*request;
    uint32_t id=(uint32_t)bound.declared;
    if (id>=XR_XIR_TYPE_PARAMETER_BASE && id<XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t parameter=id-XR_XIR_TYPE_PARAMETER_BASE;
        if (parameter>=bound.argument_count) return XR_XIR_BAD_TYPE;
        bound.declared=bound.arguments[parameter];bound.source=bound.destination;
        bound.arguments=NULL;bound.argument_count=0;
    }
    const XrXirTypeNode *declared=xr_xir_callable_signature(bound.source,bound.declared);
    const XrXirTypeNode *actual=xr_xir_callable_signature(bound.destination,bound.actual);
    if (!!declared!=!!actual) return XR_XIR_BAD_TYPE;
    if (!declared) { *output=(EffectInvocationValueBound){0};return XR_XIR_OK; }
    XrXirStatus status=xir_effect_callable_bound_matches(work,&bound);
    if (status!=XR_XIR_OK) return status;
    *output=(EffectInvocationValueBound){.mask=declared->flags&
        (XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED),.callable=true};
    return XR_XIR_OK;
}

/* Private receiving construction after full source/body/origin/owner replay.
 * This copies bounds only. It does not seal equations, relax a checker or grant
 * Program/runtime permission. A failed or occupied request publishes nothing. */
