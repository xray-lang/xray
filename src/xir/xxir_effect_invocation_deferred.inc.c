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
    EffectInvocationOwner *owner=flow->owner;
    const XrXirInstruction *op=&flow->function->instructions[instruction];
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
        if (actual>=flow->values || !xr_xir_callable_parameter_storage_valid(owner->module->types,parameter)) {
            status=XR_XIR_BAD_TYPE;break;
        }
        status=effect_invocation_value_matches(flow,parameter->type,
            xr_xir_operand_type(flow->function,actual),&scratch);
        if (status!=XR_XIR_OK) break;
        uint64_t *row=input+(size_t)p*flow->basis.row_words;
        if (xr_xir_type_is_cell(owner->module->types,parameter->type))
            status=effect_invocation_cell(flow,actual,row+flow->basis.fn_words,NULL);
        else if (xr_xir_callable_signature(owner->module->types,parameter->type)) {
            if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row))) {
                status=XR_XIR_BUDGET;break;
            }
            memcpy(row,flow->rows+(size_t)actual*flow->basis.fn_words,
                (size_t)flow->basis.fn_words*sizeof(*row));
            if (flow->fixed[actual]) {
                uint32_t mask=0;
                status=effect_invocation_bounds_mask(owner->work,owner->declared,
                    owner->nodes[flow->node].body,actual,&mask);
                if (status!=XR_XIR_OK) break;
                if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row)+3)) {
                    status=XR_XIR_BUDGET;break;
                }
                memset(row,0,(size_t)flow->basis.fn_words*sizeof(*row));
                uint32_t opaque=owner->site_count+flow->basis.parameters;
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
    EffectInvocationOwner *owner=flow->owner;
    if (flow->node>=owner->count || parameter>=flow->basis.parameters ||
        instruction>=flow->function->instruction_count || !signature) return XR_XIR_BAD_STRUCTURE;
    uint64_t *input=NULL;
    XrXirStatus status=effect_invocation_deferred_arguments(flow,instruction,signature,&input);
    if (status!=XR_XIR_OK) return status;
    uint64_t words=(uint64_t)signature->parameter_count*flow->basis.row_words;
    for (uint32_t d=0;d<owner->deferred_count;++d) {
        if (!xir_compile_work(owner->work,3)) { status=XR_XIR_BUDGET;break; }
        EffectInvocationDeferred *record=&owner->deferred_calls[d];
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
    if (status==XR_XIR_OK && owner->deferred_count==owner->deferred_capacity) {
        uint32_t capacity=owner->deferred_capacity ? owner->deferred_capacity : 8;
        if (capacity>owner->maximum) capacity=owner->maximum;
        else if (owner->deferred_capacity) capacity=capacity>owner->maximum/2 ? owner->maximum : capacity*2;
        if (capacity<=owner->deferred_count || (uint64_t)capacity>SIZE_MAX/sizeof(EffectInvocationDeferred))
            status=XR_XIR_BUDGET;
        EffectInvocationDeferred *records=status==XR_XIR_OK ? xir_compile_alloc(owner->work,
            (size_t)capacity*sizeof(*records),&status) : NULL;
        if (status==XR_XIR_OK && !records) status=XR_XIR_OUT_OF_MEMORY;
        if (status==XR_XIR_OK && !xir_compile_work(owner->work,
                (uint64_t)owner->deferred_count*sizeof(*records))) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) {
            if (owner->deferred_count) memcpy(records,owner->deferred_calls,
                (size_t)owner->deferred_count*sizeof(*records));
            xr_compile_resources_free(owner->deferred_calls);owner->deferred_calls=records;
            owner->deferred_capacity=capacity;
        } else xr_compile_resources_free(records);
    }
    if (status==XR_XIR_OK && !xir_compile_work(owner->work,sizeof(EffectInvocationDeferred)+1))
        status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(input);return status; }
    owner->deferred_calls[owner->deferred_count]=(EffectInvocationDeferred){flow->node,instruction,
        parameter,signature->parameter_count,input};
    ++owner->deferred_count;return XR_XIR_OK;
}
