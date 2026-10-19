/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_context_replay.inc.c - Current ordered Error call frontiers
 *
 * Every real frontier and indirect call reads its complete physical row.
 * Other instructions require only their current opcode and canonical shape.
 */
static XrXirStatus error_invocation_context_row(EffectInvocationFlow *replay,
    ErrorInvocationContexts *contexts,const EffectInvocationCoverage *coverage,
    uint32_t i,const XrXirInstruction *instruction) {
    if (!instruction || replay->node>=contexts->count || i>=replay->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    ErrorInvocationCall *call=&contexts->calls[contexts->nodes[replay->node].calls+i];
    bool indirect=error_invocation_indirect_opcode(instruction->op);
    if (call->frontier && (!call->handled || call->head>=contexts->link_count ||
        (!indirect && contexts->links[call->head].next!=UINT32_MAX))) return XR_XIR_BAD_STRUCTURE;
    if (!indirect) return XR_XIR_OK;
    XrXirStatus status=error_invocation_indirect_check(replay,contexts,coverage,i,instruction,&call->unknown);
    if (status==XR_XIR_OK) call->handled=true;
    return status;
}

static XrXirStatus error_invocation_context_scalar(EffectInvocationFlow *replay,
    ErrorInvocationContexts *contexts,const EffectInvocationCoverage *coverage) {
    EffectBodyRecordCursor cursor;
    XrXirStatus status=effect_body_request_sequence_begin(&replay->body,&cursor);
    for (uint32_t i=0;status==XR_XIR_OK && i<replay->function->instruction_count;++i) {
        if (!xir_compile_work(replay->read->work,1)) return XR_XIR_BUDGET;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_body_request_sequence_instruction(&replay->body,
            replay->read->final->nodes[replay->node].body,i,&cursor,&storage,&op);
        if (status==XR_XIR_OK) status=error_invocation_context_row(replay,contexts,coverage,i,op);
    }
    return status==XR_XIR_OK?effect_body_request_sequence_finish(&replay->body,
        replay->read->final->nodes[replay->node].body,&cursor):status;
}

/* Complete canonical range checks include every column and the actual EOF.
 * Opcode selection excludes no stored frontier: those rows still enter the
 * full handler even when their opcode is direct or the input is unmatched. */
static XrXirStatus error_invocation_context_replay(EffectInvocationFlow *replay,
    ErrorInvocationContexts *contexts,const EffectInvocationCoverage *coverage) {
    const EffectInvocationRead *owner=replay->read;
    if (!effect_body_request_owned_matches(owner->work,&replay->body,owner->final->material))
        return error_invocation_context_scalar(replay,contexts,coverage);
    uint32_t f=owner->final->nodes[replay->node].body;
    const EffectBodyMaterial *material=replay->body.owner;
    if (f>=effect_body_count(material) || replay->node>=contexts->count || contexts->nodes[replay->node].body!=f)
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *function=&material->functions[f];
    if (function->instruction_count!=replay->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRecordCursor cursor;
    XrXirStatus status=effect_body_request_sequence_begin(&replay->body,&cursor);
    if (status!=XR_XIR_OK) return status;
    EffectBodyRecordCursor finish={0,0,UINT32_MAX};
    while (cursor.first<function->instruction_count) {
        const EffectBodyRun *ref=NULL;
        status=effect_body_range_ref(&replay->body.read,function,&cursor,&ref);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(owner->work,sizeof(finish))) return XR_XIR_BUDGET;
        finish=(EffectBodyRecordCursor){ref->first,cursor.run-1,cursor.first-1};
        uint32_t opcode=XR_XIR_INVALID;
        for (uint32_t i=ref->first;i-ref->first<ref->count;++i) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            if (i==ref->first || (ref->mask&UINT32_C(1))) {
                status=effect_body_run_word(&replay->body.read,ref,i,0,&opcode);
                if (status!=XR_XIR_OK) return status;
                if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            }
            if (!error_invocation_indirect_opcode((XrXirOp)opcode) &&
                !contexts->calls[contexts->nodes[replay->node].calls+i].frontier) continue;
            XrXirInstruction operation;
            status=effect_body_instruction_ref_value(&replay->body.read,ref,i,&operation);
            if (status!=XR_XIR_OK) return status;
            if (operation.op!=(XrXirOp)opcode) return XR_XIR_BAD_STRUCTURE;
            status=error_invocation_context_row(replay,contexts,coverage,i,&operation);
            if (status!=XR_XIR_OK) return status;
        }
    }
    if (cursor.first!=function->instruction_count || cursor.run!=function->run_count) return XR_XIR_BAD_STRUCTURE;
    return effect_body_request_sequence_finish(&replay->body,f,&finish);
}
