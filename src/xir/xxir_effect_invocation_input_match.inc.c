/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_input_match.inc.c - Exact actual call environments
 *
 * KEY CONCEPT:
 *   Class construction compares the complete actual input key. This transient
 *   argument projection stores no completeness rank or receiving authority.
 */
/* The input builder is shared with production invocation admission. Old
 * smaller environments remain valid edges, but cannot execute this full key. */
static XrXirStatus effect_invocation_arguments_match(EffectInvocationFlow *flow,
    uint32_t e,const EffectInvocationClosureView *supplied,bool *matched) {
    const EffectInvocationRead *owner=flow->read;*matched=false;
    XrXirStatus status=effect_invocation_edge_shape_read(owner->work,owner->final,&flow->body,
        flow->body.domain.types,flow->node,e);
    if (status!=XR_XIR_OK) return status;
    EffectInvocationEdge edge=owner->final->edges[e];
    const EffectInvocationNode *target=&owner->final->nodes[edge.target];
    XrXirInstruction body_op_1;const XrXirInstruction *op=NULL;
    XrXirStatus body_read_1=effect_invocation_flow_instruction(flow,edge.instruction,&body_op_1,&op);
    if (body_read_1!=XR_XIR_OK) return body_read_1;
    EffectBodyFunctionHead target_head={0};
    status=effect_invocation_flow_head(flow,target->body,&target_head);
    if (status!=XR_XIR_OK) return status;
    if (target->parameter_count!=target_head.parameter_count) return XR_XIR_BAD_STRUCTURE;
    if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        if (target->parameter_count || target->input) return XR_XIR_BAD_STRUCTURE;
        *matched=true;return XR_XIR_OK;
    }
    const uint64_t *closure=NULL;
    if (edge.producer!=UINT32_MAX && op->op!=XR_XIR_FUNCTION_REF) {
        uint64_t value=effect_invocation_iterator_operation(op->op)?op->args[0]:(uint64_t)op->immediate;
        if (value>=flow->values || (supplied &&
            (!effect_invocation_iterator_operation(op->op) || supplied->value!=value))) return XR_XIR_BAD_STRUCTURE;
        closure=supplied?supplied->row:effect_invocation_row(flow,(uint32_t)value);
        if (!closure) return XR_XIR_BAD_STRUCTURE;
    }
    uint64_t *input=NULL;flow->instruction=edge.instruction;
    if (op->op==XR_XIR_FUNCTION_REF)
        status=effect_invocation_latent_input(flow,edge.instruction,edge.producer,op,&input);
    else status=effect_invocation_arguments(flow,op,target->body,edge.producer,closure,&input);
    uint64_t words=(uint64_t)target->parameter_count*flow->basis.row_words;
    if (status==XR_XIR_OK && words>SIZE_MAX/sizeof(*input)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK && words && (!input || !target->input)) status=XR_XIR_BAD_STRUCTURE;
    bool same=true;
    for (size_t w=0;status==XR_XIR_OK && w<(size_t)words;++w) {
        if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
        if (input[w]!=target->input[w]) same=false;
    }
    xr_compile_resources_free(input);if (status==XR_XIR_OK) *matched=same;return status;
}
