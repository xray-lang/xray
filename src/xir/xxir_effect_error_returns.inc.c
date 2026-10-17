/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_returns.inc.c - Latent Error values returned by actual factories
 *
 * KEY CONCEPT:
 *   Returning a Task or Iterator transports its errors without escaping them.
 *   The sole Error CFG joins current sealed targets; opaque inputs stay unknown.
 */
static XrXirStatus error_invocation_return(ErrorFlow *flow,const XrXirInstruction *op) {
    ErrorInvocationContexts *contexts=flow->invocations;
    if (!contexts) return XR_XIR_OK;
    if (flow->invocation>=contexts->count || contexts->resources!=flow->remaining->resources ||
        contexts->nodes[flow->invocation].body!=flow->function_index) return XR_XIR_BAD_STRUCTURE;
    uint32_t row=contexts->nodes[flow->invocation].returned;
    if (row==UINT32_MAX) return XR_XIR_OK;
    if (row>=flow->summary_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t words=flow->effects->outcomes_work.words;
    if (!xir_compile_work(flow->remaining,words)) return XR_XIR_BUDGET;
    uint64_t *to=flow->effects->outcomes_work.errors+(size_t)row*words;
    flow->summary_changed|=error_join(to,error_read(flow,flow->work,op->args[0]),words);
    return XR_XIR_OK;
}
static bool error_result_call(XrXirOp op) {
    return op==XR_XIR_CALL || op==XR_XIR_INVOKE || op==XR_XIR_CALL_DEFAULT ||
        op==XR_XIR_INVOKE_DEFAULT || op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT;
}
static XrXirStatus error_invocation_result(ErrorFlow *flow,const XrXirInstruction *value,uint64_t *set) {
    const XrXirTypeNode *type=xr_xir_type_node(flow->body.domain.types,value->type);
    if (!type || (type->kind!=XR_XIR_TYPE_TASK && type->kind!=XR_XIR_TYPE_ITERATOR)) {
        error_type(flow,value->type,set);return XR_XIR_OK;
    }
    uint32_t instruction=flow->instruction;
    XrXirInstruction storage;const XrXirInstruction *op=value;
    if (op->op==XR_XIR_INVOKE_RESULT) {
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        instruction=(uint32_t)op->immediate;
        XrXirStatus status=error_operation(flow,flow->function_index,instruction,&storage,&op);
        if (status!=XR_XIR_OK) return status;
    }
    ErrorInvocationContexts *contexts=flow->invocations;
    if (!error_result_call(op->op) || !contexts) {
        error_type(flow,value->type,set);return XR_XIR_OK;
    }
    if (contexts->resources!=flow->remaining->resources || flow->invocation>=contexts->count ||
        contexts->nodes[flow->invocation].body!=flow->function_index ||
        instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const ErrorInvocationCall *call=&contexts->calls[contexts->nodes[flow->invocation].calls+instruction];
    if (call->unknown) error_add(set,0);
    if (call->head==UINT32_MAX) { error_type(flow,value->type,set);return XR_XIR_OK; }
    uint32_t walked=0;
    for (uint32_t e=call->head;e!=UINT32_MAX;e=contexts->links[e].next) {
        if (!xir_compile_work(flow->remaining,3)) return XR_XIR_BUDGET;
        if (e>=contexts->link_count || ++walked>contexts->link_count ||
            contexts->links[e].caller!=flow->invocation || contexts->links[e].target>=contexts->count)
            return XR_XIR_BAD_STRUCTURE;
        uint32_t row=contexts->nodes[contexts->links[e].target].returned;
        if (row==UINT32_MAX) { error_type(flow,value->type,set);continue; }
        if (row>=flow->summary_count) return XR_XIR_BAD_STRUCTURE;
        const uint64_t *from=flow->effects->outcomes_work.errors+(size_t)row*flow->effects->outcomes_work.words;
        XrXirStatus status=error_call_summary(flow,op,from,set);
        if (status!=XR_XIR_OK || flow->restart) return status;
    }
    return XR_XIR_OK;
}
