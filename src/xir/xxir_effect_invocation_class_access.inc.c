/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_class_access.inc.c - Current sealed access instructions
 *
 * KEY CONCEPT:
 *   A completed equation proves every terminal contribution. Actual Class
 *   publication is checked by the live caller while its exclusive loan is held.
 */
static bool effect_class_access_opcode(XrXirOp op,XirEffectClassAccessKind kind) {
    switch (kind) {
    case XIR_EFFECT_ACCESS_CELL_READ: return op==XR_XIR_CELL_READ;
    case XIR_EFFECT_ACCESS_CELL_WRITE:
        return op==XR_XIR_CELL_WRITE || op==XR_XIR_CELL_LOCAL_WRITE;
    case XIR_EFFECT_ACCESS_PROJECT: return op==XR_XIR_CELL_PROJECT;
    case XIR_EFFECT_ACCESS_PATH_READ: return op==XR_XIR_PLACE_READ || op==XR_XIR_ARRAY_GET ||
        (op>=XR_XIR_MAP_GET && op<=XR_XIR_MAP_VALUE_AT);
    case XIR_EFFECT_ACCESS_PATH_WRITE:
        return op==XR_XIR_PLACE_WRITE || op==XR_XIR_ARRAY_SET || op==XR_XIR_STRUCT_SET || op==XR_XIR_RECORD_SET ||
        op==XR_XIR_MAP_SET || op==XR_XIR_SET_ADD || op==XR_XIR_HASH_DELETE || op==XR_XIR_HASH_CLEAR;
    case XIR_EFFECT_ACCESS_PUSH: return op==XR_XIR_ARRAY_PUSH;
    case XIR_EFFECT_ACCESS_LENGTH: return op==XR_XIR_ARRAY_LEN;
    case XIR_EFFECT_ACCESS_CAPACITY: return op==XR_XIR_ARRAY_CAPACITY;
    default: return false;
    }
}

/* Only place descriptors are followed. Their true SSA IDs decrease toward an
 * owning root; no value, PHI, local payload or signature supplies permission. */
static XrXirStatus effect_class_access_root(const EffectInvocationCertificate *certificate,
    const XirEffectClassAccessRequest *request,const EffectBodyFunction *body,
    uint32_t value,const EffectBodyRead *read) {
    uint32_t walked=0;
    while (value>=body->parameter_count) {
        if (++walked>body->instruction_count) return XR_XIR_BAD_STRUCTURE;
        XrXirInstruction op;
        XrXirStatus status=effect_body_instruction_get(read,body,value-body->parameter_count,&op);
        if (status!=XR_XIR_OK) return status;
        if (op.op==XR_XIR_OBJECT_PLACE) {
            XrXirType type;
            status=effect_body_operand_type(read,body,op.args[0],&type);
            if (status!=XR_XIR_OK) return status;
            return xr_xir_type_is_class(certificate->body->domain.types,type) &&
                type==request->root_type && type==request->input_type?XR_XIR_OK:XR_XIR_BAD_TYPE;
        }
        if (op.op==XR_XIR_CELL_PLACE) {
            XrXirType type;
            status=effect_body_operand_type(read,body,op.args[0],&type);
            if (status!=XR_XIR_OK) return status;
            return xr_xir_type_is_cell(certificate->body->domain.types,type) &&
                type==request->input_type?XR_XIR_OK:XR_XIR_BAD_TYPE;
        }
        if ((op.op!=XR_XIR_FIELD_PLACE && op.op!=XR_XIR_INDEX_PLACE) || op.args[0]>=value)
            return XR_XIR_BAD_STRUCTURE;
        value=op.args[0];
    }
    return XR_XIR_BAD_STRUCTURE;
}

XR_FUNC XrXirStatus xir_effects_class_access(const XrXirEffects *effects,
    const XirEffectClassAccessRequest *request) {
    if (!effects || !effects->invocations || !request || !request->read.work)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCertificate *certificate=effects->invocations;
    const EffectInvocationFinal *owner=certificate->final;
    if (!effect_body_material_shape(certificate->body) || !owner || certificate->resources!=effects->resources ||
        certificate->body->resources!=certificate->resources || (!certificate->final) ||
        request->caller>=effect_body_count(certificate->body) ||
        !xr_xir_type_is_class(certificate->body->domain.types,request->root_type))
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *body=&certificate->body->functions[request->caller];
    if (request->instruction>=body->instruction_count ||
        (request->contextual && (request->node>=owner->count ||
            owner->nodes[request->node].body!=request->caller))) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_runtime_work(&request->read,12);
    if (status!=XR_XIR_OK) return status;
    EffectBodyRead read=effect_body_runtime_read(&request->read);XrXirInstruction op;
    status=effect_body_instruction_get(&read,body,request->instruction,&op);
    if (status!=XR_XIR_OK) return status;
    if (!effect_class_access_opcode(op.op,request->kind)) return XR_XIR_BAD_STRUCTURE;
    uint32_t value=op.args[0];
    if (op.op==XR_XIR_ARRAY_SET || op.op==XR_XIR_MAP_SET) {
        if (!body->operands || value>=body->operand_count || op.args[1]!=3)
            return XR_XIR_BAD_STRUCTURE;
        value=body->operands[value];
    }
    if (request->kind==XIR_EFFECT_ACCESS_CELL_READ || request->kind==XIR_EFFECT_ACCESS_CELL_WRITE) {
        XrXirType type;
        status=effect_body_operand_type(&read,body,value,&type);
        if (status==XR_XIR_OK && (!xr_xir_type_is_cell(certificate->body->domain.types,type) ||
            type!=request->input_type)) status=XR_XIR_BAD_TYPE;
    } else status=effect_class_access_root(certificate,request,body,value,&read);
    if (status!=XR_XIR_OK || !request->pure) return status;
    XirEffectInvocationRequest parent={.caller=request->caller,.node=request->node,
        .contextual=request->contextual,.read=request->read};
    uint32_t selected=UINT32_MAX;
    /* The unique sealed terminal replay is the proof. A local runtime object
     * cannot clear a residual UNKNOWN or an unbound formal dependency. */
    return effect_runtime_parent(certificate,&parent,&selected);
}
