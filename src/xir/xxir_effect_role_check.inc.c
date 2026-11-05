/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_role_check.inc.c - Actual Input use projection for role proof
 *
 * The constructor and verifier read the same physical instruction rules.
 * Only the constructor has an edge index or a propagation queue.
 */
enum { EFFECT_ROLE_ALIAS, EFFECT_ROLE_FIXED, EFFECT_ROLE_CONDITIONAL, EFFECT_ROLE_CALLEE };
typedef struct EffectRoleIndex { uint32_t count; } EffectRoleIndex;
typedef struct EffectRoleCheck EffectRoleCheck;
struct EffectRoleCheck {
    const XrXirCompileContext *work;
    const EffectBodyMaterial *material;
    const EffectRoleProof *proof;
    const EffectBodyFunctionHead *active_head;
    uint32_t functions,total,active_function,function,instruction,ordinal,selected;
    bool dimensions_locked;
    void *receiver;
    XrXirStatus (*emit)(EffectRoleCheck *,uint32_t,uint32_t,uint32_t);
};
#include "xxir_effect_role_frame.inc.c"
#include "xxir_effect_current_input_read.inc.c"

static XrXirStatus effect_role_value(EffectRoleCheck *check,EffectRoleIndex *uses,
    uint32_t source,uint32_t target,uint32_t action) {
    uint32_t ordinal=check->ordinal++;
    if (source>=uses->count || (action==EFFECT_ROLE_ALIAS && target>=uses->count) ||
        (action==EFFECT_ROLE_CALLEE && target>=check->total) || !check->emit) return XR_XIR_BAD_STRUCTURE;
    if (check->selected!=UINT32_MAX && check->selected!=ordinal) return XR_XIR_OK;
    return check->emit(check,source,target,action);
}
static XrXirStatus effect_role_group(EffectRoleCheck *check,EffectRoleIndex *uses,
    const EffectBodyFunctionHead *head,const XrXirInstruction *op,uint32_t destination,uint32_t action) {
    if (op->args[0]>head->operand_count || op->args[1]>head->operand_count-op->args[0] ||
        (op->args[1] && !head->operands) || (op->op==XR_XIR_PHI && (op->args[1]&1)))
        return XR_XIR_BAD_STRUCTURE;
    uint32_t first=op->op==XR_XIR_PHI?1:0,step=op->op==XR_XIR_PHI?2:1;
    uint32_t limit=op->args[1];
    if (check->selected!=UINT32_MAX) {
        uint64_t chosen=(uint64_t)first+(uint64_t)check->selected*step;
        if (chosen>=limit) return XR_XIR_BAD_STRUCTURE;
        first=(uint32_t)chosen;limit=first+1;check->ordinal=check->selected;
    }
    for (uint32_t a=first;a<limit;a+=step) {

        XrXirStatus status=effect_role_value(check,uses,head->operands[op->args[0]+a],destination,action);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_role_call(EffectRoleCheck *check,EffectRoleIndex *uses,
    const EffectBodyFunctionHead *head,const XrXirInstruction *op,uint32_t destination) {
    if (op->immediate<0 || (uint64_t)op->immediate>=effect_body_count(check->material) ||
        op->args[0]>head->operand_count || op->args[1]>head->operand_count-op->args[0] ||
        (op->args[1] && !head->operands)) return XR_XIR_BAD_STRUCTURE;
    uint32_t target=(uint32_t)op->immediate,parameters=check->material->functions[target].parameter_count;
    if (op->args[1]>parameters) return XR_XIR_BAD_STRUCTURE;
    uint32_t begin=0,end=op->args[1],width=op->op==XR_XIR_FUNCTION_REF?2:1;
    if (check->selected!=UINT32_MAX) {
        begin=check->selected/width;if (begin>=end) return XR_XIR_BAD_STRUCTURE;
        end=begin+1;check->ordinal=begin*width;
    }
    for (uint32_t p=begin;p<end;++p) {

        uint32_t actual=head->operands[op->args[0]+p];
        XrXirStatus status=effect_role_value(check,uses,actual,check->proof->rows[target].formal_first+p,EFFECT_ROLE_CALLEE);
        if (status==XR_XIR_OK && op->op==XR_XIR_FUNCTION_REF)
            status=effect_role_value(check,uses,actual,destination,EFFECT_ROLE_ALIAS);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* A forbidden use already fixes every source formal. Its later aliases
 * cannot change that kind, so only non-forbidden copies need an SSA edge. */
static XrXirStatus effect_role_transfer(EffectRoleCheck *check,EffectRoleIndex *uses,
    const EffectBodyRequest *body,uint32_t f,const EffectBodyFunctionHead *head,
    uint32_t destination,const XrXirInstruction *op,bool *handled) {
    *handled=true;
    switch (op->op) {
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
    case XR_XIR_LOCAL_NEW: case XR_XIR_LOCAL_READ: case XR_XIR_SCALAR_LOCAL_NEW:
    case XR_XIR_SCALAR_LOCAL_READ: case XR_XIR_OWNED_LOCAL_NEW: case XR_XIR_OWNED_LOCAL_READ:
    case XR_XIR_CELL_READ: case XR_XIR_NULLABLE_UNWRAP: case XR_XIR_PLACE_READ:
    case XR_XIR_STRUCT_GET: case XR_XIR_CLASS_GET: case XR_XIR_ENUM_GET: case XR_XIR_TUPLE_FIELD:
        return effect_role_value(check,uses,op->args[0],destination,EFFECT_ROLE_ALIAS);
    case XR_XIR_LOCAL_WRITE: case XR_XIR_SCALAR_LOCAL_WRITE: case XR_XIR_OWNED_LOCAL_WRITE:
        return effect_role_value(check,uses,op->args[1],op->args[0],EFFECT_ROLE_ALIAS);
    case XR_XIR_PHI:return effect_role_group(check,uses,head,op,destination,EFFECT_ROLE_ALIAS);
    case XR_XIR_FUNCTION_REF: case XR_XIR_CALL: case XR_XIR_INVOKE: case XR_XIR_CLEANUP_REGISTER:
        return effect_role_call(check,uses,head,op,destination);
    case XR_XIR_CALL_REQUIREMENT:
        return effect_role_group(check,uses,head,op,0,EFFECT_ROLE_CONDITIONAL);
    case XR_XIR_CELL_PROJECT:return XR_XIR_OK;
    case XR_XIR_CELL_NEW:
        if (xr_xir_cell_is_unit(body->domain.types,op->type)) return XR_XIR_OK;
        return effect_role_value(check,uses,op->args[0],0,EFFECT_ROLE_FIXED);
    case XR_XIR_NULLABLE_SOME: case XR_XIR_FUNCTION_WEAKEN:
        return effect_role_value(check,uses,op->args[0],0,EFFECT_ROLE_FIXED);
    case XR_XIR_PLACE_WRITE:return effect_role_value(check,uses,op->args[1],0,EFFECT_ROLE_FIXED);
    case XR_XIR_CELL_WRITE: case XR_XIR_CELL_LOCAL_WRITE: {
        XrXirType type;XrXirStatus status=effect_body_request_type(body,f,op->args[0],&type);
        if (status!=XR_XIR_OK || xr_xir_cell_is_unit(body->domain.types,type)) return status;
        return effect_role_value(check,uses,op->args[1],0,EFFECT_ROLE_FIXED);
    }
    default:*handled=false;return XR_XIR_OK;
    }
}
static XrXirStatus effect_role_opaque(EffectRoleCheck *check,EffectRoleIndex *uses,
    const EffectBodyRequest *body,uint32_t f,const EffectBodyFunctionHead *head,
    const XrXirInstruction *op) {
    if (xr_xir_op_uses_operand_table(op->op))
        return effect_role_group(check,uses,head,op,0,EFFECT_ROLE_FIXED);
    uint32_t count=effect_parameter_operands[op->op];
    if (op->op==XR_XIR_RETURN) count=head->result!=XR_XIR_UNIT;
    if (op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_STORE) {
        if (!body->domain.declarations) return XR_XIR_BAD_STRUCTURE;
        count=xr_xir_slot_payload_operands(body->domain.declarations->slots,
            body->domain.declarations->slot_count,op);
    }
    if (op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_READ ||
        op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE) {
        XrXirType type=op->type;
        if (op->op!=XR_XIR_CELL_NEW) {
            XrXirStatus status=effect_body_request_type(body,f,op->args[0],&type);
            if (status!=XR_XIR_OK) return status;
        }
        count=xr_xir_cell_payload_operands(body->domain.types,type,op->op);
    }
    if (count>2) return XR_XIR_BAD_STRUCTURE;
    uint32_t begin=0;
    if (check->selected!=UINT32_MAX) {
        if (check->selected>=count) return XR_XIR_BAD_STRUCTURE;
        begin=check->selected;count=begin+1;check->ordinal=begin;
    }
    for (uint32_t a=begin;a<count;++a) {

        XrXirStatus status=effect_role_value(check,uses,op->args[a],0,EFFECT_ROLE_FIXED);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_role_project(EffectRoleCheck *check,uint32_t f,
    const EffectBodyFunctionHead *head,uint32_t i) {
    EffectBodyRequest body=effect_body_owned_request(check->work,check->material);
    EffectBodyInstructionShape shape={0};
    XrXirStatus status=effect_body_request_shape(&body,f,i,&shape);
    if (status!=XR_XIR_OK) return status;
    if (shape.op<=XR_XIR_INVALID || shape.op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    check->function=f;check->instruction=i;check->ordinal=0;
    if (shape.op==XR_XIR_CELL_PROJECT || (shape.op==XR_XIR_RETURN && head->result==XR_XIR_UNIT)) return XR_XIR_OK;
    if (!effect_parameter_operands[shape.op] && !xr_xir_op_uses_operand_table(shape.op) &&
        shape.op!=XR_XIR_RETURN && shape.op!=XR_XIR_FUNCTION_REF && shape.op!=XR_XIR_CALL &&
        shape.op!=XR_XIR_INVOKE && shape.op!=XR_XIR_CLEANUP_REGISTER) return XR_XIR_OK;
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    status=effect_body_request_instruction(&body,f,i,&storage,&op);
    if (status!=XR_XIR_OK) return status;
    if (!op || op->op!=shape.op || op->type!=shape.type) return XR_XIR_BAD_STRUCTURE;
    uint64_t values=(uint64_t)head->parameter_count+head->instruction_count;
    if (values>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    EffectRoleIndex uses={.count=(uint32_t)values};bool handled=false;
    status=effect_role_transfer(check,&uses,&body,f,head,head->parameter_count+i,op,&handled);
    if (status==XR_XIR_OK && !handled) status=effect_role_opaque(check,&uses,&body,f,head,op);
    return status;
}
#include "xxir_effect_role_proof.inc.c"
#include "xxir_effect_role_value.inc.c"
