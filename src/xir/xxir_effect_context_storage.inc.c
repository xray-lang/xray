/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_storage.inc.c - Exact physical receivers for callable carriers
 *
 * KEY CONCEPT:
 *   A concrete Array storage use owns its complete element type. Exact carrier
 *   edges retain that physical advertisement independently of semantic bounds.
 */
static XrXirStatus effect_scalar_storage_require(EffectScalarFlow *flow,
    uint32_t value,XrXirType expected) {
    if (value>=flow->value_count) return XR_XIR_BAD_VALUE;
    if (!xr_xir_callable_signature(&flow->terms->types,expected)) return XR_XIR_BAD_TYPE;
    if (value<flow->function->parameter_count && (!flow->function->parameters ||
        flow->function->parameters[value]!=expected)) return XR_XIR_BAD_TYPE;
    if (flow->fixed[value]&EFFECT_SCALAR_STORAGE_PIN)
        return flow->declared[value]==expected?XR_XIR_OK:XR_XIR_BAD_TYPE;
    /* The receiver cannot supply its own producer signature. This current
     * SSA still carries the authentic substituted shape before this constraint. */
    XrXirType original=xr_xir_operand_type(flow->function,value);
    XrXirStatus status=xr_xir_compile_callable_weakening(flow->terms->remaining,
        &flow->terms->types,original,expected);
    if (status!=XR_XIR_OK) return status;
    flow->declared[value]=expected;
    flow->fixed[value]|=EFFECT_SCALAR_STORAGE_PIN;
    flow->changed=true;
    return XR_XIR_OK;
}
static XrXirStatus effect_scalar_storage_link(EffectScalarFlow *flow,
    uint32_t left,uint32_t right) {
    if (left>=flow->value_count || right>=flow->value_count) return XR_XIR_BAD_VALUE;
    if (flow->fixed[left]&EFFECT_SCALAR_STORAGE_PIN)
        return effect_scalar_storage_require(flow,right,flow->declared[left]);
    if (flow->fixed[right]&EFFECT_SCALAR_STORAGE_PIN)
        return effect_scalar_storage_require(flow,left,flow->declared[right]);
    return XR_XIR_OK;
}
static XrXirStatus effect_scalar_storage_prepare(EffectScalarFlow *flow) {
    const XrXirFunction *function=flow->function;
    for (uint32_t i=0;i<function->instruction_count;++i) {
        const XrXirInstruction *op=&function->instructions[i];
        bool construct=op->op==XR_XIR_ARRAY_NEW || op->op==XR_XIR_ARRAY_REPEAT;
        if (!construct && op->op!=XR_XIR_ARRAY_PUSH && op->op!=XR_XIR_ARRAY_SET) continue;
        bool table=xr_xir_op_uses_operand_table(op->op);
        if (table && ((op->args[1] && !function->operands) ||
            op->args[0]>function->operand_count || op->args[1]>function->operand_count-op->args[0]))
            return XR_XIR_BAD_STRUCTURE;
        if (table && !construct && !op->args[1]) return XR_XIR_BAD_STRUCTURE;
        uint32_t receiver=table && !construct?function->operands[op->args[0]]:op->args[0];
        if (!construct && receiver>=flow->value_count) return XR_XIR_BAD_VALUE;
        XrXirType array=construct?op->type:xr_xir_operand_type(function,receiver);
        if (!xr_xir_type_is_array(&flow->terms->types,array)) return XR_XIR_BAD_TYPE;
        XrXirType expected=xr_xir_array_element(&flow->terms->types,array);
        if (!xr_xir_callable_signature(&flow->terms->types,expected)) continue;
        uint32_t first=0,count=0;
        if (op->op==XR_XIR_ARRAY_NEW) count=op->args[1];
        else if (op->op==XR_XIR_ARRAY_SET) { first=2;count=3;if (op->args[1]!=3) return XR_XIR_BAD_STRUCTURE; }
        else { first=1;count=2; }
        for (uint32_t a=first;a<count;++a) {
            uint32_t value=table?function->operands[op->args[0]+a]:op->args[a];
            XrXirStatus status=effect_scalar_storage_require(flow,value,expected);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_scalar_storage_physical(EffectScalarFlow *flow,
    uint32_t value,XrXirType actual,XrXirType *output) {
    *output=actual;
    if (!(flow->fixed[value]&EFFECT_SCALAR_STORAGE_PIN)) return XR_XIR_OK;
    XrXirType expected=flow->declared[value];
    XrXirStatus status=xr_xir_compile_callable_weakening(flow->terms->remaining,
        &flow->terms->types,actual,expected);
    if (status==XR_XIR_OK) *output=expected;
    return status;
}
