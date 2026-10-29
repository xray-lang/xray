/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_binary.c - Complete binary operator checking before emission
 */
#include "xhir_binary.h"
#include "../xir/xxir_types.h"
#include "../base/xchecks.h"

static XrXirStatus binary_failure(const char **reason,XrXirStatus status,const char *message) {
    if(reason)*reason=message;return status;
}
static XrXirStatus binary_operation(const XrHirConversionContext *context,
    const XrHirBinaryInput *input,XrHirBinary *node,XrXirType *target,const char **reason) {
    XrXirType left=input->left,right=input->right;
    XrHirBinaryOperator operation=input->operation;
    *target=left;
    if(operation==XR_HIR_BINARY_EQ || operation==XR_HIR_BINARY_NE) {
        if(left!=right) {
            if(xr_xir_type_is_nullable(context->types,left) && xr_xir_nullable_element(context->types,left)==right)*target=left;
            else if(xr_xir_type_is_nullable(context->types,right) && xr_xir_nullable_element(context->types,right)==left)*target=right;
            else return binary_failure(reason,XR_XIR_BAD_TYPE,"equality requires the same declared operand type");
        }
        if(!context->proof)return binary_failure(reason,XR_XIR_BAD_STRUCTURE,"equality declaration proof is missing");
        XrXirStatus status=xr_xir_compile_type_markers_prove(context->compile,context->proof,*target,XR_XIR_CONSTRAINT_EQUAL);
        if(status!=XR_XIR_OK)return binary_failure(reason,status,"equality requires its declaration predicate");
        node->operation=XR_XIR_EQUAL;node->result=XR_XIR_BOOL;node->immediate=operation==XR_HIR_BINARY_NE;
    } else if(left==XR_XIR_STRING && right==XR_XIR_STRING && operation==XR_HIR_BINARY_ADD) {
        node->operation=XR_XIR_CONCAT_STRING;node->result=XR_XIR_STRING;
    } else if(left==XR_XIR_STRING && right==XR_XIR_STRING) {
        switch(operation) {
        case XR_HIR_BINARY_LT:node->operation=XR_XIR_LT_STRING;break;
        case XR_HIR_BINARY_LE:node->operation=XR_XIR_LE_STRING;break;
        case XR_HIR_BINARY_GT:node->operation=XR_XIR_GT_STRING;break;
        case XR_HIR_BINARY_GE:node->operation=XR_XIR_GE_STRING;break;
        default:return binary_failure(reason,XR_XIR_BAD_TYPE,"operator is not defined for string operands");
        }
        node->result=XR_XIR_BOOL;
    } else if(xr_xir_float_bits(left) && xr_xir_float_bits(right)) {
        *target=left==XR_XIR_F64 || right==XR_XIR_F64?XR_XIR_F64:XR_XIR_F32;
        switch(operation) {
        case XR_HIR_BINARY_ADD:node->operation=XR_XIR_ADD_FLOAT;break;
        case XR_HIR_BINARY_SUB:node->operation=XR_XIR_SUB_FLOAT;break;
        case XR_HIR_BINARY_MUL:node->operation=XR_XIR_MUL_FLOAT;break;
        case XR_HIR_BINARY_DIV:node->operation=XR_XIR_DIV_FLOAT;break;
        case XR_HIR_BINARY_LT:node->operation=XR_XIR_LT_FLOAT;break;
        case XR_HIR_BINARY_LE:node->operation=XR_XIR_LE_FLOAT;break;
        case XR_HIR_BINARY_GT:node->operation=XR_XIR_GT_FLOAT;break;
        case XR_HIR_BINARY_GE:node->operation=XR_XIR_GE_FLOAT;break;
        default:return binary_failure(reason,XR_XIR_BAD_TYPE,"operator is not defined for floating operands");
        }
        node->result=operation>=XR_HIR_BINARY_LT?XR_XIR_BOOL:*target;
    } else {
        if(!xr_xir_type_is_integer(left) || !xr_xir_type_is_integer(right))
            return binary_failure(reason,XR_XIR_BAD_TYPE,"operator requires a declared concrete operand contract");
        bool shift=operation==XR_HIR_BINARY_SHL || operation==XR_HIR_BINARY_SHR;
        if(!shift) {
            if(xr_xir_integer_signed(left)!=xr_xir_integer_signed(right))
                return binary_failure(reason,XR_XIR_BAD_TYPE,"integer signedness requires an explicit cast");
            *target=xr_xir_integer_bits(left)<xr_xir_integer_bits(right)?right:left;
        }
        switch(operation) {
        case XR_HIR_BINARY_ADD:node->operation=XR_XIR_ADD_INT;break;
        case XR_HIR_BINARY_SUB:node->operation=XR_XIR_SUB_INT;break;
        case XR_HIR_BINARY_MUL:node->operation=XR_XIR_MUL_INT;break;
        case XR_HIR_BINARY_DIV:node->operation=XR_XIR_DIV_INT;break;
        case XR_HIR_BINARY_REM:node->operation=XR_XIR_REM_INT;break;
        case XR_HIR_BINARY_AND:node->operation=XR_XIR_AND_INT;break;
        case XR_HIR_BINARY_OR:node->operation=XR_XIR_OR_INT;break;
        case XR_HIR_BINARY_XOR:node->operation=XR_XIR_XOR_INT;break;
        case XR_HIR_BINARY_SHL:node->operation=XR_XIR_SHL_INT;break;
        case XR_HIR_BINARY_SHR:node->operation=XR_XIR_SHR_INT;break;
        case XR_HIR_BINARY_LT:node->operation=XR_XIR_LT_INT;break;
        case XR_HIR_BINARY_LE:node->operation=XR_XIR_LE_INT;break;
        case XR_HIR_BINARY_GT:node->operation=XR_XIR_GT_INT;break;
        case XR_HIR_BINARY_GE:node->operation=XR_XIR_GE_INT;break;
        default:return binary_failure(reason,XR_XIR_BAD_STRUCTURE,"unknown arithmetic operator");
        }
        node->result=operation>=XR_HIR_BINARY_LT?XR_XIR_BOOL:*target;
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_binary_check(const XrHirConversionContext *context,
    const XrHirBinaryInput *input,XrHirBinary *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!context || !context->compile || !context->types || !input || !output)
        return binary_failure(reason,XR_XIR_BAD_STRUCTURE,"binary operator input is missing");
    if(input->operation<XR_HIR_BINARY_ADD || input->operation>XR_HIR_BINARY_GE)
        return binary_failure(reason,XR_XIR_BAD_STRUCTURE,"unknown arithmetic operator");
    XrHirBinary node={.origin=input->origin};XrXirType target;
    XrXirStatus status=binary_operation(context,input,&node,&target,reason);
    if(status!=XR_XIR_OK)return status;
    XrHirConversionInput conversion={.origin=input->origin,.actual=input->left,.expected=target,
        .has_expected=true,.argument_bound=input->argument_bound};
    status=xr_hir_conversion_check(context,&conversion,&node.left,reason);
    if(status!=XR_XIR_OK)return status;
    bool shift=input->operation==XR_HIR_BINARY_SHL || input->operation==XR_HIR_BINARY_SHR;
    conversion.actual=input->right;conversion.expected=shift?input->right:target;
    status=xr_hir_conversion_check(context,&conversion,&node.right,reason);
    if(status==XR_XIR_OK)*output=node;
    return status;
}
XR_FUNC XrXirInstruction xr_hir_binary_instruction(const XrHirBinary *node,uint32_t left,uint32_t right) {
    XR_CHECK(node && node->operation!=XR_XIR_INVALID,"binary HIR must be fully typed before lowering");
    return (XrXirInstruction){node->operation,node->result,{left,right},{0},node->immediate,{0}};
}
