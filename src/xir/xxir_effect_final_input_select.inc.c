/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_final_input_select.inc.c - Current opcode selection for proof reads
 *
 * KEY CONCEPT:
 *   Every pass reaches actual canonical EOF. Only an actual opcode excludes
 *   an irrelevant row; selected operations still consume their complete row.
 */
enum { EFFECT_FINAL_SELECT_REFERENCE=1,EFFECT_FINAL_SELECT_EDGE=2,
    EFFECT_FINAL_SELECT_PRODUCER=3,EFFECT_FINAL_SELECT_INDIRECT=4,
    EFFECT_FINAL_SELECT_CAPTURE=5,EFFECT_FINAL_SELECT_WRITE=6,
    EFFECT_FINAL_SELECT_RETURN=7,EFFECT_FINAL_SELECT_BARRIER=8 };
typedef struct EffectFinalInputSelect {
    EffectBodyRecordCursor cursor;
    const EffectBodyRun *run;
    uint32_t next,end,selection;
} EffectFinalInputSelect;
static bool effect_final_input_selected(uint32_t selection,XrXirOp op) {
    if(selection==EFFECT_FINAL_SELECT_REFERENCE) return (op==XR_XIR_FUNCTION_REF || op==XR_XIR_GENERATOR_NEW);
    if(selection==EFFECT_FINAL_SELECT_EDGE) return op==XR_XIR_CALL || op==XR_XIR_INVOKE ||
        op==XR_XIR_CLEANUP_REGISTER || op==XR_XIR_GO || op==XR_XIR_GENERATOR_NEW || op==XR_XIR_CALL_DEFAULT || op==XR_XIR_INVOKE_DEFAULT;
    if(selection==EFFECT_FINAL_SELECT_INDIRECT) return op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT;
    if(selection==EFFECT_FINAL_SELECT_CAPTURE) return effect_invocation_iterator_operation(op) || (op==XR_XIR_FUNCTION_REF || op==XR_XIR_GENERATOR_NEW) || op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT;
    if(selection==EFFECT_FINAL_SELECT_WRITE) return op==XR_XIR_LOCAL_WRITE || op==XR_XIR_SCALAR_LOCAL_WRITE || op==XR_XIR_OWNED_LOCAL_WRITE;
    if(selection==EFFECT_FINAL_SELECT_RETURN) return op==XR_XIR_RETURN;
    if(selection==EFFECT_FINAL_SELECT_BARRIER) return (effect_physical_opcode(op)&EFFECT_PHYSICAL_BARRIER)!=0;
    if(selection!=EFFECT_FINAL_SELECT_PRODUCER) return false;
    return (op==XR_XIR_FUNCTION_REF || op==XR_XIR_GENERATOR_NEW) || op==XR_XIR_COPY || op==XR_XIR_SCALAR_COPY || op==XR_XIR_OWNED_RETAIN ||
        op==XR_XIR_LOCAL_NEW || op==XR_XIR_SCALAR_LOCAL_NEW || op==XR_XIR_OWNED_LOCAL_NEW ||
        op==XR_XIR_LOCAL_READ || op==XR_XIR_SCALAR_LOCAL_READ || op==XR_XIR_OWNED_LOCAL_READ ||
        op==XR_XIR_LOCAL_WRITE || op==XR_XIR_SCALAR_LOCAL_WRITE || op==XR_XIR_OWNED_LOCAL_WRITE ||
        op==XR_XIR_PHI || op==XR_XIR_RETURN;
}
static XrXirStatus effect_final_input_select_begin(const EffectBodyRequest *body,
    uint32_t selection,EffectFinalInputSelect *cursor) {
    if(!cursor || selection<EFFECT_FINAL_SELECT_REFERENCE || selection>EFFECT_FINAL_SELECT_BARRIER ||
        !effect_body_request_sequence_owned(body)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_request_sequence_binding(body);if(status!=XR_XIR_OK) return status;
    if(!(((&body->read)->execution ? effect_body_execution_work((&body->read)->execution, sizeof(*cursor)) : xir_compile_context_valid((&body->read)->owner)))) return XR_XIR_BUDGET;
    *cursor=(EffectFinalInputSelect){.cursor={0,0,UINT32_MAX},.selection=selection};return XR_XIR_OK;
}
/* The returned ordinal and full operation live only in the enclosing read.
 * No previous result, physical-use mask or stored empty claim selects a row. */
static XrXirStatus effect_final_input_select_next(const EffectBodyRequest *body,
    uint32_t function,EffectFinalInputSelect *cursor,uint32_t *instruction,
    XrXirInstruction *operation,bool *found) {
    if(!body || !cursor || !instruction || !operation || !found ||
        function>=body->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_request_sequence_binding(body);if(status!=XR_XIR_OK) return status;
    const EffectBodyMaterial *material=body->owner;const EffectBodyFunction *head=&material->functions[function];
    if(!head->instruction_count && (head->run_count || head->run_capacity || head->runs))
        return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(body->read.owner,head->runs,head->run_capacity,sizeof(*head->runs));
    if(status!=XR_XIR_OK) return status;
    *found=false;
    for(;;) {
        if(cursor->next==cursor->end) {
            if(cursor->cursor.run==head->run_count) {
                return cursor->cursor.first==head->instruction_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
            }
            status=effect_body_range_ref(&body->read,head,&cursor->cursor,&cursor->run);
            if(status!=XR_XIR_OK) return status;
            cursor->next=cursor->run->first;cursor->end=cursor->next+cursor->run->count;
            if(!(cursor->run->mask&UINT32_C(1))) {
                uint32_t opcode=XR_XIR_INVALID;
                status=effect_body_run_word(&body->read,cursor->run,cursor->next,0,&opcode);
                if(status!=XR_XIR_OK) return status;
                if(opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
                if(!effect_final_input_selected(cursor->selection,(XrXirOp)opcode)) {
                    cursor->next=cursor->end;continue;
                }
            }
        }
        uint32_t at=cursor->next++,opcode=XR_XIR_INVALID;
        status=effect_body_run_word(&body->read,cursor->run,at,0,&opcode);
        if(status!=XR_XIR_OK) return status;
        if(opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        if(!effect_final_input_selected(cursor->selection,(XrXirOp)opcode)) continue;
        status=effect_body_instruction_ref_value(&body->read,cursor->run,at,operation);
        if(status!=XR_XIR_OK) return status;
        *instruction=at;*found=true;return XR_XIR_OK;
    }
}
