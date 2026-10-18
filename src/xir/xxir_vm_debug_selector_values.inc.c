/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_selector_values.inc.c - Pure projections of one observed Error
 */
typedef struct DebugSelector {
    const XrXirModule *module;
    const XrXirFunction *function;
    const XrXirSourceDebugSelector *selector;
    const XrXirValue *error;
    XrXirValue *values;
    XrXirDomain *observer;
    XrXirValueAdmission *actual;
    XrXirValueAdmission admission;
    uint32_t root,predecessor;
} DebugSelector;
static const XrXirValue *debug_selector_value(const DebugSelector *run,uint32_t operand) {
    if (operand==run->root) return run->error;
    uint32_t parameters=run->function->parameter_count;
    if (operand<parameters || operand-parameters<run->selector->first ||
        operand-parameters>=run->selector->accepted) return NULL;
    return &run->values[operand-parameters-run->selector->first];
}
static uint32_t debug_selector_block(const XrXirFunction *function,uint32_t instruction) {
    uint32_t first=0,end=function->block_count;
    while (first<end) {
        uint32_t middle=first+(end-first)/2;
        if (function->blocks[middle].first<=instruction) first=middle+1;else end=middle;
    }
    return first ? first-1 : UINT32_MAX;
}
static XrXirValueStatus debug_selector_operation(DebugSelector *run,const XrXirInstruction *op,XrXirValue *output) {
    const XrXirValue *left=NULL,*right=NULL;bool equal=false;
    switch (op->op) {
    case XR_XIR_CONST_INT:case XR_XIR_CONST_FLOAT:case XR_XIR_CONST_BOOL:case XR_XIR_CONST_RUNE:
        *output=(XrXirValue){(uint32_t)op->type,0,op->immediate};return XR_XIR_VALUE_OK;
    case XR_XIR_CONST_STRING: {
        if (!run->module->declarations || op->immediate<0 ||
            (uint64_t)op->immediate>=run->module->declarations->literal_count) return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirLiteral literal=run->module->declarations->literals[op->immediate];
        return xr_xir_string_new(run->observer,literal.bytes,literal.length,output);
    }
    case XR_XIR_PHI:
        for (uint32_t a=0;a<op->args[1];a+=2) {
            if (!run->admission.work) return XR_XIR_VALUE_LIMIT;
            --run->admission.work;
            if (run->function->operands[op->args[0]+a]!=run->predecessor) continue;
            left=debug_selector_value(run,run->function->operands[op->args[0]+a+1]);
            return left ? xr_xir_value_copy(left,output) : XR_XIR_VALUE_BAD_ARGUMENT;
        }
        return XR_XIR_VALUE_BAD_ARGUMENT;
    default:break;
    }
    left=debug_selector_value(run,op->args[0]);if (!left) return XR_XIR_VALUE_BAD_ARGUMENT;
    switch (op->op) {
    case XR_XIR_ERROR_IS: {
        XrXirValueStatus status=xr_xir_error_is(left,(XrXirType)op->immediate,&run->admission,&equal);
        if (status==XR_XIR_VALUE_OK) *output=(XrXirValue){XR_XIR_BOOL,0,equal};return status;
    }
    case XR_XIR_ERROR_NARROW:return xr_xir_error_narrow(left,op->type,&run->admission,output);
    case XR_XIR_ENUM_TAG: {
        uint32_t variant=0;XrXirValueStatus status=xr_xir_enum_variant(left,&variant);
        if (status==XR_XIR_VALUE_OK) *output=(XrXirValue){XR_XIR_I64,0,variant};return status;
    }
    case XR_XIR_ENUM_GET:return xr_xir_enum_get(left,(uint32_t)op->immediate,op->args[1],&run->admission,output);
    default:break;
    }
    right=debug_selector_value(run,op->args[1]);if (!right) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (op->op==XR_XIR_EQUAL) {
        XrXirValueStatus status=xr_xir_value_equal(left,right,(XrXirType)left->type,&run->admission,&equal);
        if (status==XR_XIR_VALUE_OK) *output=(XrXirValue){XR_XIR_BOOL,0,op->immediate ? !equal : equal};return status;
    }
    if (op->op!=XR_XIR_EQ_INT && op->op!=XR_XIR_GE_INT && op->op!=XR_XIR_LE_INT) return XR_XIR_VALUE_BAD_ARGUMENT;
    int compared=0;
    if (left->type!=right->type || xr_xir_integer_compare(xr_xir_integer_format((XrXirType)left->type),
        left->payload,right->payload,&compared)!=XR_XIR_RUN_OK) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=(XrXirValue){XR_XIR_BOOL,0,op->op==XR_XIR_EQ_INT ? compared==0 : op->op==XR_XIR_GE_INT ? compared>=0 : compared<=0};
    return XR_XIR_VALUE_OK;
}
