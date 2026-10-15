/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_deferred.inc.c - Complete delayed invocation inputs
 *
 * KEY CONCEPT:
 *   An unbound callable leaves its actual physical arguments in the same
 *   equation owner. A boolean UNKNOWN never stands in for this mapping.
 */
static XrXirStatus effect_invocation_deferred_arguments(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirTypeNode *signature,uint64_t **output) {
    if (!output || *output || !signature || instruction>=flow->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationRead *owner=flow->read;
    XrXirInstruction body_op_1;const XrXirInstruction *op=NULL;
    XrXirStatus body_read_1=effect_invocation_flow_instruction(flow,instruction,&body_op_1,&op);
    if (body_read_1!=XR_XIR_OK) return body_read_1;
    if (op->args[1]!=signature->parameter_count ||
        op->args[0]>flow->function->operand_count ||
        op->args[1]>flow->function->operand_count-op->args[0] ||
        (op->args[1] && !flow->function->operands)) return XR_XIR_BAD_STRUCTURE;
    uint64_t words=(uint64_t)signature->parameter_count*flow->basis.row_words;
    if (words>SIZE_MAX/sizeof(uint64_t)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint64_t *input=words ? xir_compile_calloc(owner->work,(size_t)words,sizeof(*input),&status) : NULL;
    if (words && !input) return status;
    XrXirTypeMatchScratch scratch={owner->work->resources,NULL};
    for (uint32_t p=0;p<signature->parameter_count && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
        const XrXirCallableParameter *parameter=&signature->parameters[p];
        uint32_t actual=flow->function->operands[op->args[0]+p];
        if (actual>=flow->values || !xr_xir_callable_parameter_storage_valid(flow->body.domain.types,parameter)) {
            status=XR_XIR_BAD_TYPE;break;
        }
        XrXirType actual_type=0;status=effect_invocation_flow_type(flow,actual,&actual_type);
        if (status==XR_XIR_OK)
            status=effect_invocation_value_matches(flow,parameter->type,actual_type,&scratch);
        if (status!=XR_XIR_OK) break;
        uint64_t *row=input+(size_t)p*flow->basis.row_words;
        if (xr_xir_type_is_cell(flow->body.domain.types,parameter->type))
            status=effect_invocation_cell(flow,actual,row+flow->basis.fn_words,NULL);
        else if (xr_xir_type_is_class(flow->body.domain.types,parameter->type))
            status=effect_invocation_class_word(flow,actual,row+flow->basis.fn_words);
        else if (xr_xir_callable_signature(flow->body.domain.types,parameter->type)) {
            if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row))) {
                status=XR_XIR_BUDGET;break;
            }
            const uint64_t *actual_row=effect_invocation_row(flow,actual);
            if (!actual_row) { status=XR_XIR_BAD_STRUCTURE;break; }
            memcpy(row,actual_row,
                (size_t)flow->basis.fn_words*sizeof(*row));
            bool fixed=false;status=effect_invocation_flag_get(flow,actual,false,&fixed);
            if (status!=XR_XIR_OK) break;
            if (fixed) {
                uint32_t mask=0;
                status=effect_invocation_bounds_mask(owner->work,owner->declared,
                    owner->final->nodes[flow->node].body,actual,&mask);
                if (status!=XR_XIR_OK) break;
                if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row)+3)) {
                    status=XR_XIR_BUDGET;break;
                }
                memset(row,0,(size_t)flow->basis.fn_words*sizeof(*row));
                uint32_t opaque=owner->final->site_count+flow->basis.parameters;
                row[opaque/64]|=UINT64_C(1)<<(opaque%64);
                if (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)
                    row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
                if (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)
                    row[(opaque+2)/64]|=UINT64_C(1)<<((opaque+2)%64);
            }
        }
    }
    xr_xir_type_match_scratch_free(&scratch);
    if (status!=XR_XIR_OK) { xr_compile_resources_free(input);return status; }
    *output=input;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_deferred_record(EffectInvocationFlow *flow,
    uint32_t instruction,uint32_t parameter,const XrXirTypeNode *signature) {
    const EffectInvocationRead *owner=flow->read;
    if (flow->node>=owner->final->count || parameter>=flow->basis.parameters ||
        instruction>=flow->function->instruction_count || !signature) return XR_XIR_BAD_STRUCTURE;
    uint64_t *input=NULL;
    XrXirStatus status=effect_invocation_deferred_arguments(flow,instruction,signature,&input);
    if (status==XR_XIR_UNRESOLVED) return XR_XIR_OK;
    if (status!=XR_XIR_OK) return status;
    uint64_t words=(uint64_t)signature->parameter_count*flow->basis.row_words;
    for (uint32_t d=0;d<owner->final->deferred_count;++d) {
        if (!xir_compile_work(owner->work,3)) { status=XR_XIR_BUDGET;break; }
        EffectInvocationDeferred *record=&owner->final->deferred_calls[d];
        if (record->node!=flow->node || record->instruction!=instruction || record->parameter!=parameter)
            continue;
        if (record->count!=signature->parameter_count) { status=XR_XIR_BAD_STRUCTURE;break; }
        for (size_t w=0;w<(size_t)words;++w) {
            if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
            if (flow->precision ? (record->arguments[w]&~input[w])!=0 : record->arguments[w]!=input[w]) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
        }
        if (status==XR_XIR_OK && flow->precision) {
            if (!xir_compile_work(owner->work,words*sizeof(*input))) status=XR_XIR_BUDGET;
            else if (words) memcpy(record->arguments,input,(size_t)words*sizeof(*input));
        }
        xr_compile_resources_free(input);return status;
    }
    if (status==XR_XIR_OK && owner->final->deferred_count==owner->final->deferred_capacity) {
        uint32_t capacity=owner->final->deferred_capacity ? owner->final->deferred_capacity : 8;
        if (capacity>flow->owner->maximum) capacity=flow->owner->maximum;
        else if (owner->final->deferred_capacity) capacity=capacity>flow->owner->maximum/2 ? flow->owner->maximum : capacity*2;
        if (capacity<=owner->final->deferred_count || (uint64_t)capacity>SIZE_MAX/sizeof(EffectInvocationDeferred))
            status=XR_XIR_BUDGET;
        EffectInvocationDeferred *records=status==XR_XIR_OK ? xir_compile_alloc(owner->work,
            (size_t)capacity*sizeof(*records),&status) : NULL;
        if (status==XR_XIR_OK && !records) status=XR_XIR_OUT_OF_MEMORY;
        if (status==XR_XIR_OK && !xir_compile_work(owner->work,
                (uint64_t)owner->final->deferred_count*sizeof(*records))) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) {
            if (owner->final->deferred_count) memcpy(records,owner->final->deferred_calls,
                (size_t)owner->final->deferred_count*sizeof(*records));
            xr_compile_resources_free(owner->final->deferred_calls);flow->owner->final.deferred_calls=records;
            flow->owner->final.deferred_capacity=capacity;
        } else xr_compile_resources_free(records);
    }
    if (status==XR_XIR_OK && !xir_compile_work(owner->work,sizeof(EffectInvocationDeferred)+1))
        status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(input);return status; }
    owner->final->deferred_calls[owner->final->deferred_count]=(EffectInvocationDeferred){flow->node,instruction,
        parameter,signature->parameter_count,input};
    ++flow->owner->final.deferred_count;return XR_XIR_OK;
}
