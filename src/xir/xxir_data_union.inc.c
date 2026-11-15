/* Closed data-union normalization and membership, independent of Error sets. */
#include "xxir_data_union.h"
XR_FUNC bool xr_xir_data_union_atom(XrXirType type) {
    return type==XR_XIR_BOOL || type==XR_XIR_RUNE || type==XR_XIR_STRING || xr_xir_type_is_number(type);
}
XR_FUNC bool xr_xir_data_union_shape(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!node || node->kind!=XR_XIR_TYPE_DATA_UNION || node->parameter_count<2 ||
        node->parameter_count>XR_XIR_DATA_UNION_MAX_MEMBERS || !node->parameters ||
        node->element || node->result || node->flags || node->parameter_span ||
        node->nominal.declaration || node->nominal.arguments || node->nominal.argument_count ||
        node->nominal.fields || node->nominal.field_count) return false;
    bool integer=false,floating=false;
    for(uint32_t p=0;p<node->parameter_count;++p) {
        XrXirCallableParameter member=node->parameters[p];
        if(member.mode || member.escaping || !xr_xir_data_union_atom(member.type) ||
            (p && (uint32_t)node->parameters[p-1].type>=(uint32_t)member.type)) return false;
        if(xr_xir_type_is_integer(member.type)) {if(integer)return false;integer=true;}
        if(xr_xir_float_bits(member.type)) {if(floating)return false;floating=true;}
    }
    return true;
}
XR_FUNC bool xr_xir_data_union_contains(const XrXirTypes *types,XrXirType target,XrXirType member) {
    if(!xr_xir_data_union_shape(types,target))return false;
    const XrXirTypeNode *node=xr_xir_type_node(types,target);
    for(uint32_t p=0;p<node->parameter_count;++p)if(node->parameters[p].type==member)return true;
    return false;
}
XR_FUNC XrXirStatus xr_xir_compile_data_union(const XrXirCompileContext *context,
    const XrXirTypes *types,const XrXirType *members,uint32_t count,
    XrCompileRequestControl *control,XrXirDataUnion *output) {
    if(!xir_compile_context_valid(context) || !types || !output || !count || !members)return XR_XIR_BAD_STRUCTURE;
    if(count>context->limits.parameters)return XR_XIR_BUDGET;
    XrXirDataUnion result={0};
    for(uint32_t i=0;i<count;++i) {
        XrXirStatus status=control?xir_compile_resource_status(xr_compile_request_control_poll(control)):XR_XIR_OK;
        if(status!=XR_XIR_OK)return status;
        const XrXirTypeNode *node=xr_xir_type_node(types,members[i]);
        if(node && !xr_xir_data_union_shape(types,members[i]))return XR_XIR_BAD_TYPE;
        uint32_t n=node?node->parameter_count:1;
        for(uint32_t p=0;p<n;++p) {
            XrXirType member=node?node->parameters[p].type:members[i];
            if(!xr_xir_data_union_atom(member))return XR_XIR_BAD_TYPE;
            uint32_t at=0;while(at<result.count && (uint32_t)result.members[at]<(uint32_t)member)++at;
            if(at<result.count && result.members[at]==member)continue;
            if(result.count==XR_XIR_DATA_UNION_MAX_MEMBERS)return XR_XIR_BAD_TYPE;
            for(uint32_t q=result.count;q>at;--q)result.members[q]=result.members[q-1];
            result.members[at]=member;++result.count;
        }
    }
    bool integer=false,floating=false;
    for(uint32_t p=0;p<result.count;++p) {
        if(xr_xir_type_is_integer(result.members[p])) {if(integer)return XR_XIR_BAD_TYPE;integer=true;}
        if(xr_xir_float_bits(result.members[p])) {if(floating)return XR_XIR_BAD_TYPE;floating=true;}
    }
    *output=result;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_data_union_select(const XrXirTypes *types,XrXirType actual,
    XrXirType target,XrXirType *output) {
    if(!output || !xr_xir_data_union_shape(types,target))return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *node=xr_xir_type_node(types,target);
    for(uint32_t p=0;p<node->parameter_count;++p)
        if(node->parameters[p].type==actual) {*output=actual;return XR_XIR_OK;}
    XrXirType selected=XR_XIR_UNIT;
    for(uint32_t p=0;p<node->parameter_count;++p) {
        XrXirType member=node->parameters[p].type;
        bool numeric=(actual==XR_XIR_F32 && member==XR_XIR_F64) ||
            (xr_xir_type_is_integer(actual) && xr_xir_type_is_integer(member) &&
            xr_xir_integer_signed(actual)==xr_xir_integer_signed(member) &&
            xr_xir_integer_bits(actual)<xr_xir_integer_bits(member));
        if(!numeric)continue;
        if(selected!=XR_XIR_UNIT)return XR_XIR_BAD_TYPE;
        selected=member;
    }
    if(selected==XR_XIR_UNIT)return XR_XIR_BAD_TYPE;
    *output=selected;return XR_XIR_OK;
}
