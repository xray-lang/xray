/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_select_shape.h - Exact heterogeneous receive selection
 *
 * KEY CONCEPT:
 *   Actual Channel operands define every payload type. A result type alone
 *   neither grants a receive nor identifies the arm that may commit.
 */
#ifndef XXIR_CHANNEL_SELECT_SHAPE_H
#define XXIR_CHANNEL_SELECT_SHAPE_H
#include "xxir_operand_roles.h"
#include "xxir_receive_control.h"
#include "xxir_channel_select_arm.h"
/* Modes are no fallback, default, and timeout. The final timeout operand is
 * I64; every preceding value is an actual receive Channel. */
static inline XrXirStatus xir_channel_select_shape(const XrXirModule *module,
    const XrXirFunction *function,const XrXirInstruction *op,XrCompileRequestControl *control) {
    if(!module||!module->types||!function||!op||op->op!=XR_XIR_CHANNEL_SELECT||
        op->immediate<0||op->immediate>2||op->targets[0]||op->targets[1]||
        op->type_arguments[0]||op->type_arguments[1])return XR_XIR_BAD_STRUCTURE;
    uint32_t timeout=op->immediate==2?1u:0u;
    if(op->args[1]<=timeout||op->args[1]>65536u||!function->operands||
        op->args[0]>function->operand_count||op->args[1]>function->operand_count-op->args[0])
        return XR_XIR_BAD_STRUCTURE;
    uint32_t count=op->args[1]-timeout;
    const XrXirTypeNode *result=xr_xir_type_node(module->types,op->type);
    if(count>=65536u||!result||result->kind!=XR_XIR_TYPE_TUPLE||
        result->parameter_count!=count+1||!result->parameters||
        result->parameters[0].type!=XR_XIR_I64||result->parameters[0].mode!=XR_PARAM_READ)
        return XR_XIR_BAD_TYPE;
    uint32_t steps=0;
    XrXirStatus status=xir_receive_poll(control);if(status!=XR_XIR_OK)return status;
    for(uint32_t a=0;a<op->args[1];++a) {
        status=xir_receive_step(control,&steps);if(status!=XR_XIR_OK)return status;
        uint32_t value=function->operands[op->args[0]+a];
        if(value>=(uint64_t)function->parameter_count+function->instruction_count)return XR_XIR_BAD_VALUE;
        XrXirType actual=xr_xir_operand_type(function,value);
        if(a==count){if(actual!=XR_XIR_I64)return XR_XIR_BAD_TYPE;continue;}
        bool sending=false;actual=xir_channel_select_operand_type(module->types,actual,&sending);
        if(actual==XR_XIR_UNIT)return XR_XIR_BAD_TYPE;
        const XrXirCallableParameter *field=result->parameters+a+1;
        const XrXirTypeNode *nullable=xr_xir_type_node(module->types,field->type);
        if(field->mode!=XR_PARAM_READ||!nullable||nullable->kind!=XR_XIR_TYPE_NULLABLE||
            nullable->element!=xr_xir_channel_element(module->types,actual))return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
#endif // XXIR_CHANNEL_SELECT_SHAPE_H
