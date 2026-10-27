/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_iterators.inc.c - Actual Iterator consumption edges
 */
/* Advancing a parked value executes its genuine body under the current
 * complete capture relation. Constructing or closing it has no such edge. */
static XrXirStatus effect_invocation_iterator(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op,bool *changed) {
    if (!flow || !op || !changed || !effect_invocation_iterator_operation(op->op) ||
        op->args[0]>=flow->values) return XR_XIR_BAD_STRUCTURE;
    XrXirType actual_type=0;
    XrXirStatus status=effect_invocation_flow_type(flow,op->args[0],&actual_type);
    if (status!=XR_XIR_OK) return status;
    if (!effect_physical_iterator(flow->body.domain.types,actual_type)) return XR_XIR_BAD_TYPE;
    const uint64_t *row=effect_invocation_row(flow,op->args[0]);
    if (!row) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationRead *owner=flow->read;
    for (uint32_t s=0;s<owner->final->site_count;++s) {

        if (!(row[s/64]&(UINT64_C(1)<<(s%64)))) continue;
        const EffectInvocationSite site=owner->final->sites[s];
        XrXirInstruction storage;const XrXirInstruction *producer=NULL;
        status=effect_body_request_operation(&flow->body,site.function,site.instruction,&storage,&producer);
        if (status!=XR_XIR_OK) return status;
        if (producer->op!=XR_XIR_GENERATOR_NEW || producer->immediate!=(int64_t)site.target ||
            producer->args[1]!=site.captures) return XR_XIR_BAD_STRUCTURE;
        EffectBodyFunctionHead head={0};status=effect_invocation_flow_head(flow,site.target,&head);
        if (status!=XR_XIR_OK) return status;
        if (site.captures!=head.parameter_count || !effect_physical_iterator(flow->body.domain.types,head.result))
            return XR_XIR_BAD_STRUCTURE;
        XrXirTypeMatchScratch scratch={owner->work->resources,NULL};
        status=effect_invocation_value_matches(flow,head.result,actual_type,&scratch);
        xr_xir_type_match_scratch_free(&scratch);
        if (status!=XR_XIR_OK) return status;

        EffectInvocationActual actual={site.target,s,row};
        status=effect_invocation_connect(flow,instruction,op,&actual,&head,changed);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Ordinary calls cannot intern an actual Iterator from a temporary bottom.
 * If discovery never supplies an identity, the real call remains conditional. */
static XrXirStatus effect_invocation_iterator_pending(EffectInvocationFlow *flow,
    const XrXirInstruction *op,bool *output) {
    *output=false;
    if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE &&
        op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) return XR_XIR_OK;
    if (op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] ||
        (op->args[1] && !flow->function->operands)) return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->values) return XR_XIR_BAD_STRUCTURE;
        const uint64_t *closure=effect_invocation_row(flow,(uint32_t)op->immediate);
        if (!closure) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t s=0;s<flow->read->final->site_count;++s) {

            if (!(closure[s/64]&(UINT64_C(1)<<(s%64)))) continue;
            const EffectInvocationSite *site=&flow->read->final->sites[s];EffectBodyFunctionHead head={0};
            XrXirStatus status=effect_invocation_flow_head(flow,site->target,&head);
            if (status!=XR_XIR_OK) return status;
            if (site->captures>head.parameter_count) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t p=0;p<site->captures;++p) {

                if (!effect_physical_iterator(flow->body.domain.types,head.parameters[p])) continue;
                const uint64_t *row=closure+flow->basis.origin_words+
                    (size_t)(site->binding+p)*(flow->basis.origin_words+flow->basis.cell_words);
                bool present=false;
                for (uint32_t w=0;w<flow->basis.origin_words;++w) {

                    present|=row[w]!=0;
                }
                if (!present) *output=true;
            }
        }
    }
    for (uint32_t p=0;p<op->args[1];++p) {

        uint32_t value=flow->function->operands[op->args[0]+p];XrXirType type=0;
        XrXirStatus status=effect_invocation_flow_type(flow,value,&type);
        if (status!=XR_XIR_OK) return status;
        if (!effect_physical_iterator(flow->body.domain.types,type)) continue;
        const uint64_t *row=effect_invocation_row(flow,value);if (!row) return XR_XIR_BAD_STRUCTURE;
        bool present=false;
        for (uint32_t w=0;w<flow->basis.origin_words;++w) {

            present|=row[w]!=0;
        }
        if (!present) *output=true;
    }
    return XR_XIR_OK;
}
