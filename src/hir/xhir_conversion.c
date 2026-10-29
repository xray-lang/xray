/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_conversion.c - Complete contextual conversion checking before emission
 */
#include "xhir_conversion.h"
#include "../xir/xxir_types.h"
#include "../xir/xxir_compile_memory.h"
#include "../base/xchecks.h"

static XrXirStatus conversion_failure(const char **reason,XrXirStatus status,const char *message) {
    if(reason)*reason=message;return status;
}
static XrXirStatus conversion_poll(XrCompileRequestControl *control,const char **reason) {
    XrXirStatus status=control?xir_compile_resource_status(xr_compile_request_control_poll(control)):XR_XIR_OK;
    return status==XR_XIR_OK?status:conversion_failure(reason,status,"source request interrupted");
}
static XrXirStatus conversion_contains(const XrXirTypes *types,XrXirType type,
    XrCompileRequestControl *control,uint32_t depth,bool *output,const char **reason) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!node) {*output=false;return XR_XIR_OK;}
    XrXirStatus status=conversion_poll(control,reason);if(status!=XR_XIR_OK)return status;
    if(depth>=128)return conversion_failure(reason,XR_XIR_BUDGET,"callable flow nesting exhausted");
    if(node->kind==XR_XIR_TYPE_CALLABLE) {*output=true;return XR_XIR_OK;}
    bool contains=false;
    status=conversion_contains(types,node->element,control,depth+1,&contains,reason);
    if(status!=XR_XIR_OK || contains) {*output=contains;return status;}
    for(uint32_t p=0;p<node->parameter_count;++p) {
        status=conversion_contains(types,node->parameters[p].type,control,depth+1,&contains,reason);
        if(status!=XR_XIR_OK || contains) {*output=contains;return status;}
    }
    for(uint32_t a=0;a<node->nominal.argument_count;++a) {
        status=conversion_contains(types,node->nominal.arguments[a],control,depth+1,&contains,reason);
        if(status!=XR_XIR_OK || contains) {*output=contains;return status;}
    }
    *output=false;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_hir_conversion_contains_callable(const XrXirTypes *types,XrXirType type,
    XrCompileRequestControl *control,bool *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!types || !output)return conversion_failure(reason,XR_XIR_BAD_STRUCTURE,"conversion type pool is missing");
    bool contains=false;XrXirStatus status=conversion_contains(types,type,control,0,&contains,reason);
    if(status==XR_XIR_OK)*output=contains;return status;
}
XR_FUNC XrXirStatus xr_hir_conversion_check(const XrHirConversionContext *context,
    const XrHirConversionInput *input,XrHirConversion *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!context || !context->compile || !context->types || !input || !output)
        return conversion_failure(reason,XR_XIR_BAD_STRUCTURE,"contextual conversion input is missing");
    XrXirStatus status=conversion_poll(context->control,reason);if(status!=XR_XIR_OK)return status;
    XrHirConversion node={.origin=input->origin,.source=input->actual,.target=input->actual,
        .declared_bound=input->actual};
    XrXirType expected=input->expected;
    if(input->has_expected) {
        if(input->infer_result) {
            const XrXirTypeNode *wanted=xr_xir_callable_signature(context->types,expected);
            const XrXirTypeNode *provided=xr_xir_callable_signature(context->types,input->actual);
            if(!wanted || !provided)
                return conversion_failure(reason,XR_XIR_BAD_TYPE,"open callable result requires two callable types");
            XrXirTypeNode resolved=*wanted;resolved.result=provided->result;
            if(!context->intern)return conversion_failure(reason,XR_XIR_BAD_STRUCTURE,"conversion type interner is missing");
            status=context->intern(context->intern_owner,&resolved,&expected);
            if(status!=XR_XIR_OK)return conversion_failure(reason,status,"conversion result type could not be interned");
        }
        if(input->argument_bound && xr_xir_callable_signature(context->types,input->actual) &&
            xr_xir_callable_signature(context->types,expected)) {
            status=xr_xir_compile_callable_weakening(context->compile,context->types,input->actual,expected);
            if(status!=XR_XIR_OK)return conversion_failure(reason,status,"callback argument requires exact shape and an admitted advertised bound");
            node.kind=XR_HIR_CONVERSION_COPY;node.root=XR_HIR_CONVERSION_ROOT_ARGUMENT;node.declared_bound=expected;
        } else if(input->actual==expected) {
            if(!input->inferred && xr_xir_callable_signature(context->types,input->actual)) {
                status=xr_xir_compile_callable_weakening(context->compile,context->types,input->actual,expected);
                if(status!=XR_XIR_OK)return conversion_failure(reason,status,"fixed callable conversion requires its complete advertised shape");
                node.kind=XR_HIR_CONVERSION_CALLABLE;node.root=XR_HIR_CONVERSION_ROOT_FIXED;
            } else {
                bool contains=false;status=conversion_contains(context->types,input->actual,context->control,0,&contains,reason);
                if(status!=XR_XIR_OK)return status;
                if(contains) {node.kind=XR_HIR_CONVERSION_COPY;node.root=input->inferred?XR_HIR_CONVERSION_ROOT_INFERRED:XR_HIR_CONVERSION_ROOT_FIXED;}
            }
        } else {
            node.target=expected;
            if(xr_xir_type_is_nullable(context->types,expected) && xr_xir_nullable_element(context->types,expected)==input->actual) {
                node.kind=XR_HIR_CONVERSION_NULLABLE;
                if(!input->inferred) {
                    status=conversion_contains(context->types,input->actual,context->control,0,&node.copy_nullable_element,reason);
                    if(status!=XR_XIR_OK)return status;
                }
            } else if(xr_xir_type_is_callable(context->types,expected) && xr_xir_type_is_callable(context->types,input->actual)) {
                status=xr_xir_compile_callable_weakening(context->compile,context->types,input->actual,expected);
                if(status!=XR_XIR_OK)return conversion_failure(reason,status,"callable conversion may only weaken its outer execution contract");
                node.kind=XR_HIR_CONVERSION_CALLABLE;node.root=input->inferred?XR_HIR_CONVERSION_ROOT_INFERRED:XR_HIR_CONVERSION_ROOT_FIXED;
            } else if(expected==XR_XIR_ERROR) {
                if(!context->proof)return conversion_failure(reason,XR_XIR_BAD_STRUCTURE,"Error conversion declaration proof is missing");
                status=xr_xir_compile_type_markers_prove(context->compile,context->proof,input->actual,XR_XIR_CONSTRAINT_ERROR);
                if(status!=XR_XIR_OK)return conversion_failure(reason,status,"Error conversion requires an enum proof");
                node.kind=XR_HIR_CONVERSION_ERROR;
            } else {
                if(!(input->actual==XR_XIR_F32 && expected==XR_XIR_F64) &&
                    (!xr_xir_type_is_integer(input->actual) || !xr_xir_type_is_integer(expected) ||
                     xr_xir_integer_signed(input->actual)!=xr_xir_integer_signed(expected) ||
                     xr_xir_integer_bits(input->actual)>xr_xir_integer_bits(expected)))
                    return conversion_failure(reason,XR_XIR_BAD_TYPE,"expression cannot satisfy its declared type");
                node.kind=XR_HIR_CONVERSION_NUMBER;
            }
        }
    }
    status=conversion_poll(context->control,reason);if(status!=XR_XIR_OK)return status;
    *output=node;return XR_XIR_OK;
}
XR_FUNC XrXirInstruction xr_hir_conversion_instruction(const XrHirConversion *node,uint32_t operand) {
    XR_CHECK(node && node->kind>XR_HIR_CONVERSION_IDENTITY && node->kind<=XR_HIR_CONVERSION_NUMBER,
        "conversion HIR must be fully typed before lowering");
    static const XrXirOp operations[]={XR_XIR_INVALID,XR_XIR_COPY,XR_XIR_NULLABLE_SOME,
        XR_XIR_FUNCTION_WEAKEN,XR_XIR_ERROR_ERASE,XR_XIR_CONVERT_NUMBER};
    uint32_t payload=node->kind==XR_HIR_CONVERSION_NULLABLE && node->source==XR_XIR_UNIT?0:operand;
    return (XrXirInstruction){operations[node->kind],node->target,{payload,0},{0},0,{0}};
}
