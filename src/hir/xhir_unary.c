/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_unary.c - Numeric unary checking before mechanical Built emission
 */
#include "xhir_unary.h"
#include "../xir/xxir_types.h"
#include "../xir/xxir_compile_memory.h"
#include "../base/xchecks.h"

static XrXirStatus unary_failure(const char **reason,XrXirStatus status,const char *message) {
    if(reason)*reason=message;return status;
}
XR_FUNC XrXirStatus xr_hir_unary_check(const XrXirTypes *types,const XrHirUnaryInput *input,
    XrCompileRequestControl *control,XrHirUnary *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!types || !input || !output)
        return unary_failure(reason,XR_XIR_BAD_STRUCTURE,"unary operator input is missing");
    if(control) {
        XrXirStatus status=xir_compile_resource_status(xr_compile_request_control_poll(control));
        if(status!=XR_XIR_OK)return unary_failure(reason,status,"source request interrupted");
    }
    if(input->operation!=XR_HIR_UNARY_NEGATE && input->operation!=XR_HIR_UNARY_COMPLEMENT)
        return unary_failure(reason,XR_XIR_BAD_STRUCTURE,"unknown unary arithmetic operator");
    XrHirUnary node={.origin=input->origin,.type=input->operand};
    if(input->operation==XR_HIR_UNARY_NEGATE && xr_xir_float_bits(input->operand)) {
        node.operation=XR_XIR_NEG_FLOAT;
    } else {
        if(!xr_xir_type_is_integer(input->operand))
            return unary_failure(reason,XR_XIR_BAD_TYPE,"unary arithmetic requires an integer");
        node.has_seed=true;
        node.operation=input->operation==XR_HIR_UNARY_NEGATE?XR_XIR_SUB_INT:XR_XIR_XOR_INT;
        if(input->operation==XR_HIR_UNARY_COMPLEMENT) {
            unsigned width=xr_xir_integer_bits(input->operand);
            node.seed=xr_xir_integer_signed(input->operand) || width==64 ?
                -1 : (int64_t)((UINT64_C(1)<<width)-1);
        }
    }
    *output=node;return XR_XIR_OK;
}
XR_FUNC XrXirInstruction xr_hir_unary_seed_instruction(const XrHirUnary *node) {
    XR_CHECK(node && node->has_seed,"integer unary HIR must own its seed");
    return (XrXirInstruction){XR_XIR_CONST_INT,node->type,{0},{0},node->seed,{0}};
}
XR_FUNC XrXirInstruction xr_hir_unary_instruction(const XrHirUnary *node,uint32_t operand,uint32_t seed) {
    XR_CHECK(node && node->operation!=XR_XIR_INVALID,"unary HIR must be fully typed before lowering");
    return (XrXirInstruction){node->operation,node->type,
        {node->has_seed?seed:operand,node->has_seed?operand:0},{0},0,{0}};
}
