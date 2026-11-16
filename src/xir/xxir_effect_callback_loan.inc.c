/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_callback_loan.inc.c - Paired entry-loan facts in the ROOT owner
 */
#include "xxir_effect_callback_loan_internal.h"
#include "xxir_view_liveness_internal.h"

static XrXirStatus effect_callback_loan_context(const XrXirCompileContext *memory,
    const XrXirModule *module,const XrXirEffects *effects,EffectCheckedRoots **output) {
    if (!module || !module->functions || !module->declarations || !module->declarations->functions ||
        !effects || !effects->checked_roots ||
        !xir_effects_context_matches(memory,effects,module->function_count)) return XR_XIR_BAD_STRUCTURE;
    EffectCheckedRoots *roots=effects->checked_roots;
    if (roots->templates!=module->function_count) return XR_XIR_BAD_STRUCTURE;
    *output=roots;return XR_XIR_OK;
}
static bool effect_callback_loan_formal(const XrXirModule *module,uint32_t function,uint32_t *value) {
    const XrXirFunction *body=module->functions+function;
    for (uint32_t depth=0;*value>=body->parameter_count && depth<body->instruction_count;++depth) {
        uint32_t instruction=*value-body->parameter_count;
        if (instruction>=body->instruction_count) return false;
        const XrXirInstruction *op=body->instructions+instruction;
        if ((op->op!=XR_XIR_COPY && op->op!=XR_XIR_OWNED_RETAIN && op->op!=XR_XIR_FUNCTION_WEAKEN) ||
            op->args[0]>=*value || !xr_xir_callable_signature(module->types,op->type)) return false;
        *value=op->args[0];
    }
    return *value<body->parameter_count &&
        *value>=module->declarations->functions[function].capture_count &&
        !xr_xir_function_parameter_escaping(body,*value) &&
        xr_xir_callable_signature(module->types,body->parameters[*value]) &&
        !xr_xir_type_is_c_callable(module->types,body->parameters[*value]);
}
static XrXirStatus effect_callback_loan_record(const XrXirCompileContext *memory,EffectCheckedRoots *roots,
    uint32_t function,XirModuleCallbackLoan relation) {
    if (roots->callback_loans==UINT32_MAX) return XR_XIR_OVERFLOW;
    uint32_t fields[]={function,relation.callback_parameter,(uint32_t)relation.loan_kind,relation.loan_parameter};
    uint32_t hash=xr_hash_bytes(fields,sizeof(fields));
    uint32_t slot=roots->callback_loan_index_capacity ? hash&(roots->callback_loan_index_capacity-1u) : 0;
    while (roots->callback_loan_index_capacity && roots->callback_loan_index[slot]) {
        uint32_t link=roots->callback_loan_index[slot];
        if (link>roots->callback_loans) return XR_XIR_BAD_STRUCTURE;
        EffectCheckCallbackLoan row=roots->callback_loan[link-1];
        if (row.relation.callback_parameter==relation.callback_parameter &&
            row.function==function && row.relation.loan_kind==relation.loan_kind &&
            row.relation.loan_parameter==relation.loan_parameter) {
            if (relation.witness_instruction<row.relation.witness_instruction)
                roots->callback_loan[link-1].relation=relation;
            return XR_XIR_OK;
        }
        slot=(slot+1u)&(roots->callback_loan_index_capacity-1u);
    }
    XrXirStatus status=XR_XIR_OK;
    if (!roots->callback_loan_index_capacity || roots->callback_loans>=roots->callback_loan_index_capacity/2u) {
        uint64_t capacity=roots->callback_loan_index_capacity ? (uint64_t)roots->callback_loan_index_capacity*2u : 16u;
        if (capacity>UINT32_MAX) return XR_XIR_OVERFLOW;
        uint32_t *index=xir_compile_calloc(memory,(size_t)capacity,sizeof(*index),&status);
        if (!index) return status;
        for (uint32_t n=0;n<roots->callback_loans;++n) {
            EffectCheckCallbackLoan row=roots->callback_loan[n];
            uint32_t key[]={row.function,row.relation.callback_parameter,(uint32_t)row.relation.loan_kind,
                row.relation.loan_parameter};
            uint32_t position=xr_hash_bytes(key,sizeof(key))&((uint32_t)capacity-1u);
            while (index[position]) position=(position+1u)&((uint32_t)capacity-1u);
            index[position]=n+1u;
        }
        xr_compile_resources_free(roots->callback_loan_index);
        roots->callback_loan_index=index;roots->callback_loan_index_capacity=(uint32_t)capacity;
        slot=hash&(roots->callback_loan_index_capacity-1u);
        while (index[slot]) slot=(slot+1u)&(roots->callback_loan_index_capacity-1u);
    }
    if (roots->callback_loans==roots->callback_loan_capacity) {
        EffectCheckCallbackLoan *grown=xir_compile_array_grow(memory,roots->callback_loan,
            roots->callback_loans,sizeof(*grown),&roots->callback_loan_capacity,&status);
        if (!grown) return status;
        roots->callback_loan=grown;
    }
    uint32_t head=roots->function[function].callback_loans;
    roots->callback_loan[roots->callback_loans++]=(EffectCheckCallbackLoan){relation,function,head};
    roots->function[function].callback_loans=roots->callback_loans;
    roots->callback_loan_index[slot]=roots->callback_loans;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_effects_callback_loan_observe(const XrXirCompileContext *memory,
    const XrXirModule *module,const XrXirEffects *effects,const XrXirViewLiveEvent *event) {
    if (module && (!module->declarations || !module->declarations->functions)) return XR_XIR_OK;
    EffectCheckedRoots *roots=NULL;
    XrXirStatus status=effect_callback_loan_context(memory,module,effects,&roots);
    if (status!=XR_XIR_OK || !event) return status!=XR_XIR_OK?status:XR_XIR_BAD_STRUCTURE;
    if (effect_final_outcomes(effects) || event->kind!=XR_XIR_VIEW_LIVE_INSTRUCTION) return XR_XIR_OK;
    if (!xir_effects_prepared_execution(effects) || event->function>=roots->templates)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=module->functions+event->function;
    if (event->instruction>=body->instruction_count ||
        event->before.words<((uint64_t)event->before.count+63u)/64u ||
        (event->before.count && (!event->before.values || !event->before.bits))) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=body->instructions+event->instruction;
    if (op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) return XR_XIR_OK;
    if (op->immediate<0 || (uint64_t)op->immediate>UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    uint32_t callback=(uint32_t)op->immediate;
    if (!effect_callback_loan_formal(module,event->function,&callback)) return XR_XIR_OK;
    for (uint32_t p=0;p<event->before.count;++p) {
        if (!(event->before.bits[p/64u]&(UINT64_C(1)<<(p%64u)))) continue;
        uint32_t loan=event->before.values[p];
        if (loan>=body->parameter_count) continue;
        const XrXirTypeNode *type=xr_xir_type_node(module->types,body->parameters[loan]);
        if (!type || type->kind!=XR_XIR_TYPE_SLICE ||
            (type->element!=XR_XIR_I64 && type->element!=XR_XIR_U8)) continue;
        XirModuleEffectTermKind kind=loan<module->declarations->functions[event->function].capture_count ?
            XIR_MODULE_EFFECT_CAPTURE:XIR_MODULE_EFFECT_PARAMETER;
        status=effect_callback_loan_record(memory,roots,event->function,
            (XirModuleCallbackLoan){callback,kind,loan,event->function,event->instruction});
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_effects_callback_loan_observed(const XrXirCompileContext *memory,
    const XrXirModule *module,const XrXirEffects *effects) {
    if (module && (!module->declarations || !module->declarations->functions)) return XR_XIR_OK;
    EffectCheckedRoots *roots=NULL;
    XrXirStatus status=effect_callback_loan_context(memory,module,effects,&roots);
    if (status==XR_XIR_OK && !effect_final_outcomes(effects)) roots->callback_loans_observed=true;
    return status;
}
XR_FUNC XrXirStatus xir_effects_callback_loan_contract(const XrXirCompileContext *memory,
    const XrXirModule *module,const XrXirEffects *effects,uint32_t function,uint32_t callback,
    XirModuleEffectTermKind kind,uint32_t loan,bool *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output=false;EffectCheckedRoots *roots=NULL;
    XrXirStatus status=effect_callback_loan_context(memory,module,effects,&roots);
    if (status!=XR_XIR_OK) return status;
    if (function>=roots->templates) return XR_XIR_BAD_STRUCTURE;
    if (!roots->callback_loans_observed || !roots->callback_loans_closed) return XR_XIR_OK;
    uint32_t visited=0;
    for (uint32_t link=roots->function[function].callback_loans;link;) {
        if (link>roots->callback_loans || visited++>=roots->callback_loans) return XR_XIR_BAD_STRUCTURE;
        EffectCheckCallbackLoan row=roots->callback_loan[link-1];
        if (row.function!=function) return XR_XIR_BAD_STRUCTURE;
        if (row.relation.callback_parameter==callback && row.relation.loan_kind==kind &&
            row.relation.loan_parameter==loan) {*output=true;return XR_XIR_OK;}
        link=row.next;
    }
    return XR_XIR_OK;
}
