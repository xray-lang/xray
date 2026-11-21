/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_literal.c - Exact contextual numeric checking before Built emission
 */
#include "xhir_literal.h"
#include "../xir/xxir_types.h"
#include "../xir/xxir_data_union.h"
#include "../xir/xxir_float.h"
#include "../xir/xxir_compile_memory.h"
#include "../shared/xr_decimal_float.h"
#include "../base/xchecks.h"
#include <string.h>

static XrXirStatus literal_failure(const char **reason,XrXirStatus status,const char *message) {
    if (reason)
        *reason = message;
    return status;
}
static XrXirStatus literal_poll(XrCompileRequestControl *control,const char **reason) {
    XrXirStatus status=control?xir_compile_resource_status(xr_compile_request_control_poll(control)):XR_XIR_OK;
    return status==XR_XIR_OK?status:literal_failure(reason,status,"source request interrupted");
}
static bool literal_integer_float(const XrHirLiteralInput *input,XrXirType type,uint64_t *bits) {
    int64_t magnitude=0,exact=0;memcpy(&magnitude,&input->magnitude,sizeof(magnitude));
    XrXirIntegerFormat format={64,false};
    if(xr_xir_integer_to_float(format,xr_xir_float_bits(type),magnitude,bits)!=XR_XIR_NUMERIC_OK ||
        xr_xir_float_to_integer(xr_xir_float_bits(type),format,*bits,&exact)!=XR_XIR_NUMERIC_OK ||
        (uint64_t)exact!=input->magnitude)return false;
    if (input->negative)
        *bits ^= UINT64_C(1) << (xr_xir_float_bits(type) - 1);
    return true;
}
XR_FUNC XrXirStatus xr_hir_literal_check(const XrXirTypes *types,const XrHirLiteralInput *input,
    XrCompileRequestControl *control,XrHirLiteral *output,const char **reason) {
    if(reason)*reason=NULL;
    if(!types || !input || !output)return literal_failure(reason,XR_XIR_BAD_STRUCTURE,"numeric literal input is missing");
    XrXirStatus status=literal_poll(control,reason);if(status!=XR_XIR_OK)return status;
    XrXirType expected=input->expected?input->expected_type:XR_XIR_UNIT;
    if(xr_xir_type_is_nullable(types,expected))expected=xr_xir_nullable_element(types,expected);
    const XrXirTypeNode *data=xr_xir_type_node(types,expected);
    if(data && data->kind==XR_XIR_TYPE_DATA_UNION) {
        if(!xr_xir_data_union_shape(types,expected))return literal_failure(reason,XR_XIR_BAD_TYPE,"invalid data-union literal context");
        /* A numeric literal has no fixed type: it takes the one member of its
         * own numeric family. Several members of that family make the literal
         * ambiguous; an integer literal with no integer member falls back to
         * the unique float member under the ordinary literal rules. There is no
         * implicit numeric conversion, so a family with no candidate is an
         * error rather than a widening attempt. */
        XrXirType integer=XR_XIR_UNIT,floating=XR_XIR_UNIT;
        uint32_t integers=0,floats=0;
        for(uint32_t p=0;p<data->parameter_count;++p) {
            XrXirType member=data->parameters[p].type;
            if(xr_xir_type_is_integer(member)) {integer=member;++integers;}
            if(xr_xir_float_bits(member)) {floating=member;++floats;}
        }
        if(input->decimal) {
            if(floats>1)return literal_failure(reason,XR_XIR_BAD_TYPE,"ambiguous: float literal has more than one data-union float member");
            if(!floats)return literal_failure(reason,XR_XIR_BAD_TYPE,"numeric literal has no member in its data-union context");
            expected=floating;
        } else {
            if(integers>1)return literal_failure(reason,XR_XIR_BAD_TYPE,"ambiguous: integer literal has more than one data-union integer member");
            if(integers==1)expected=integer;
            else if(floats>1)return literal_failure(reason,XR_XIR_BAD_TYPE,"ambiguous: integer literal has more than one data-union float member");
            else if(floats==1)expected=floating;
            else return literal_failure(reason,XR_XIR_BAD_TYPE,"numeric literal has no member in its data-union context");
        }
    }
    XrHirLiteral literal={.origin=input->origin,.kind=XR_HIR_LITERAL_INTEGER,.type=XR_XIR_I64};
    if(input->decimal) {
        literal.kind=XR_HIR_LITERAL_FLOAT;literal.type=input->expected && xr_xir_float_bits(expected)?expected:XR_XIR_F64;
        if(!input->decimal_text || !input->decimal_length)
            return literal_failure(reason,XR_XIR_BAD_STRUCTURE,"decimal spelling is missing");
        if(!xr_decimal_float_parse(input->decimal_text,input->decimal_length,xr_xir_float_bits(literal.type),&literal.bits))
            return literal_failure(reason,XR_XIR_BAD_TYPE,"invalid decimal literal");
        if(input->negative)literal.bits^=UINT64_C(1)<<(xr_xir_float_bits(literal.type)-1);
    } else if(input->expected && xr_xir_float_bits(expected)) {
        literal.kind=XR_HIR_LITERAL_FLOAT;literal.type=expected;
        if(!literal_integer_float(input,literal.type,&literal.bits))
            return literal_failure(reason,XR_XIR_BAD_TYPE,"integer literal is not exactly representable in its floating context");
    } else {
        literal.type=input->expected && xr_xir_type_is_integer(expected)?expected:XR_XIR_I64;
        uint32_t width=xr_xir_integer_bits(literal.type);bool sign=xr_xir_integer_signed(literal.type);
        uint64_t limit=sign?(UINT64_C(1)<<(width-1))-(input->negative?0:1):
            (width==64?UINT64_MAX:(UINT64_C(1)<<width)-1);
        if(input->magnitude>limit || (!sign && input->negative && input->magnitude))
            return literal_failure(reason,XR_XIR_BAD_TYPE,"integer literal is outside its contextual type");
        literal.bits=input->negative?UINT64_C(0)-input->magnitude:input->magnitude;
    }
    status=literal_poll(control,reason);if(status!=XR_XIR_OK)return status;
    *output=literal;return XR_XIR_OK;
}
XR_FUNC XrXirInstruction xr_hir_literal_instruction(const XrHirLiteral *literal) {
    XR_CHECK(literal && ((literal->kind==XR_HIR_LITERAL_INTEGER && xr_xir_type_is_integer(literal->type)) ||
        (literal->kind==XR_HIR_LITERAL_FLOAT && xr_xir_float_bits(literal->type))),"numeric HIR must be fully typed before lowering");
    int64_t payload=literal->bits<=INT64_MAX?(int64_t)literal->bits:-1-(int64_t)(UINT64_MAX-literal->bits);
    return (XrXirInstruction){literal->kind==XR_HIR_LITERAL_INTEGER?XR_XIR_CONST_INT:XR_XIR_CONST_FLOAT,
        literal->type,{0},{0},payload,{0}};
}
