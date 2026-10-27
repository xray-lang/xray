/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_scalar.inc.c - Current primitive-only capture domains
 *
 * KEY CONCEPT:
 *   Every actual signature and canonical opcode/type run participates in this
 *   negative proof. Edges, root inputs and frontier completeness remain separate
 *   obligations; no empty relation or successful verdict is retained in Final.
 */
static bool effect_capture_scalar_type(uint32_t type) {
    return type<=XR_XIR_STRING || (type>=XR_XIR_I8 && type<=XR_XIR_RUNE);
}
static bool effect_capture_scalar_operation(XrXirOp opcode) {
    switch(opcode) {
    case XR_XIR_FUNCTION_REF: case XR_XIR_FUNCTION_WEAKEN: case XR_XIR_GENERATOR_NEW:
    case XR_XIR_CALL_INDIRECT: case XR_XIR_INVOKE_INDIRECT: case XR_XIR_CALL_REQUIREMENT:
    case XR_XIR_CELL_NEW: case XR_XIR_CELL_READ: case XR_XIR_CELL_WRITE:
    case XR_XIR_CELL_LOCAL_WRITE: case XR_XIR_CELL_STRING_APPEND: case XR_XIR_CELL_PROJECT:
    case XR_XIR_CELL_PLACE: case XR_XIR_OBJECT_PLACE: case XR_XIR_FIELD_PLACE: case XR_XIR_INDEX_PLACE:
    case XR_XIR_CLASS_NEW: case XR_XIR_CLASS_GET: case XR_XIR_CLASS_SET:
    case XR_XIR_SLOT_LOAD: case XR_XIR_SLOT_INIT: case XR_XIR_SLOT_STORE: case XR_XIR_SLOT_GROUP_INIT:
    case XR_XIR_GO:
        return false;
    default:
        return opcode>XR_XIR_INVALID && opcode<XR_XIR_OP_COUNT && !effect_invocation_iterator_operation(opcode) &&
            !(effect_physical_opcode(opcode)&EFFECT_PHYSICAL_BARRIER);
    }
}
static XrXirStatus effect_capture_scalar_domain(const EffectCaptureRead *read,bool *scalar) {
    if(!scalar) return XR_XIR_BAD_STRUCTURE;
    *scalar=false;
    const EffectBodyMaterial *material=read->final->material;
    EffectBodyCurrentRead current={0};
    if(!read->body.current) {
        XrXirStatus status=effect_body_current_open(read->work,material,&current);
        if(status!=XR_XIR_OK) return status;
    }
    for(uint32_t f=0;f<effect_body_count(material);++f) {
        const EffectBodyFunction *head=&material->functions[f];

        if(!effect_capture_scalar_type(head->result)) return XR_XIR_OK;
        for(uint32_t p=0;p<head->parameter_count;++p) {

            if(!effect_capture_scalar_type(head->parameters[p])) return XR_XIR_OK;
        }
        if(material->generics) {

            if(material->generics[f].parameter_count) return XR_XIR_OK;
        }
        EffectBodyRecordCursor cursor={0,0,UINT32_MAX};

        for(uint32_t r=0;r<head->run_count;++r) {
            const EffectBodyRun *run=NULL;
            XrXirStatus status=effect_body_range_ref(&read->body.read,head,&cursor,&run);
            if(status!=XR_XIR_OK) return status;

            bool writes=run->base.op==XR_XIR_LOCAL_WRITE || run->base.op==XR_XIR_SCALAR_LOCAL_WRITE ||
                run->base.op==XR_XIR_OWNED_LOCAL_WRITE;
            uint32_t points=(run->mask&(writes?UINT32_C(7):UINT32_C(3)))?run->count:1;
            for(uint32_t i=0;i<points;++i) {
                uint32_t opcode=XR_XIR_INVALID,type=XR_XIR_UNIT;
                status=effect_body_run_word(&read->body.read,run,run->first+i,0,&opcode);
                if(status==XR_XIR_OK) status=effect_body_run_word(&read->body.read,run,run->first+i,1,&type);
                if(status!=XR_XIR_OK) return status;
                if(opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
                if(!effect_capture_scalar_type(type) || !effect_capture_scalar_operation((XrXirOp)opcode))
                    return XR_XIR_OK;
                if(opcode==XR_XIR_LOCAL_WRITE || opcode==XR_XIR_SCALAR_LOCAL_WRITE || opcode==XR_XIR_OWNED_LOCAL_WRITE) {
                    uint32_t destination=0;
                    status=effect_body_run_word(&read->body.read,run,run->first+i,2,&destination);
                    if(status!=XR_XIR_OK) return status;
                    if(destination>=(uint64_t)head->parameter_count+head->instruction_count) return XR_XIR_BAD_STRUCTURE;
                }
            }
        }
        if(cursor.first!=head->instruction_count || cursor.run!=head->run_count) return XR_XIR_BAD_STRUCTURE;
    }

    *scalar=true;return XR_XIR_OK;
}
