/* Closed data-union normalization and membership, independent of Error sets. */
#include "xxir_data_union.h"
#include "xxir_compile_memory.h"
#include "xxir_class_hierarchy_internal.h"
XR_FUNC bool xr_xir_data_union_atom(XrXirType type) {
    return type==XR_XIR_BOOL || type==XR_XIR_RUNE || type==XR_XIR_STRING || xr_xir_type_is_number(type);
}
/* A member is either a closed built-in atom or one Class identity. Distinct
 * final classes are told apart by nominal identity. Two integer members or two
 * float members are admitted: the synthesized enum's tag distinguishes them, so
 * no numeric-family exclusivity rule applies any more. */
static bool data_union_member(const XrXirTypes *types,XrXirType type) {
    return xr_xir_data_union_atom(type) || xr_xir_type_is_class(types,type);
}
XR_FUNC bool xr_xir_data_union_shape(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!node || node->kind!=XR_XIR_TYPE_DATA_UNION || node->parameter_count<2 ||
        !node->parameters || node->element || node->result || node->flags || node->parameter_span ||
        node->nominal.declaration || node->nominal.arguments || node->nominal.argument_count ||
        node->nominal.fields || node->nominal.field_count) return false;
    for(uint32_t p=0;p<node->parameter_count;++p) {
        XrXirCallableParameter member=node->parameters[p];
        if(member.mode || member.escaping || !data_union_member(types,member.type) ||
            (p && (uint32_t)node->parameters[p-1].type>=(uint32_t)member.type)) return false;
    }
    return true;
}
XR_FUNC bool xr_xir_data_union_contains(const XrXirTypes *types,XrXirType target,XrXirType member) {
    if(!xr_xir_data_union_shape(types,target))return false;
    const XrXirTypeNode *node=xr_xir_type_node(types,target);
    for(uint32_t p=0;p<node->parameter_count;++p)if(node->parameters[p].type==member)return true;
    return false;
}
XR_FUNC bool xr_xir_data_union_member_index(const XrXirTypes *types,XrXirType target,
    XrXirType member,uint32_t *index) {
    if(!index || !xr_xir_data_union_shape(types,target))return false;
    const XrXirTypeNode *node=xr_xir_type_node(types,target);
    for(uint32_t p=0;p<node->parameter_count;++p)
        if(node->parameters[p].type==member) {*index=p;return true;}
    return false;
}
XR_FUNC bool xr_xir_data_union_all_classes(const XrXirTypes *types,XrXirType type) {
    if(!xr_xir_data_union_shape(types,type))return false;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    for(uint32_t p=0;p<node->parameter_count;++p)
        if(!xr_xir_type_is_class(types,node->parameters[p].type))return false;
    return true;
}
/* Member order is type identity order, so the same member set always interns to
 * the same constructed type regardless of spelling order or module. */
static bool data_union_insert(XrXirType *members,uint32_t *count,uint32_t capacity,XrXirType member) {
    uint32_t at=0;
    while(at<*count && (uint32_t)members[at]<(uint32_t)member)++at;
    if(at<*count && members[at]==member)return true;
    if(*count==capacity)return false;
    for(uint32_t q=*count;q>at;--q)members[q]=members[q-1];
    members[at]=member;++*count;
    return true;
}
XR_FUNC XrXirStatus xr_xir_compile_data_union(const XrXirCompileContext *context,
    const XrXirTypes *types,const XrXirType *members,uint32_t count,
    XrCompileRequestControl *control,XrXirDataUnion *output) {
    if(!xir_compile_context_valid(context) || !types || !output || !count || !members)return XR_XIR_BAD_STRUCTURE;
    if(count>context->limits.parameters)return XR_XIR_BUDGET;
    /* Flattening nested unions bounds the distinct members by the summed member
     * counts, which is checked before any allocation. */
    uint64_t total=0;
    for(uint32_t i=0;i<count;++i) {
        XrXirStatus status=control?xir_compile_resource_status(xr_compile_request_control_poll(control)):XR_XIR_OK;
        if(status!=XR_XIR_OK)return status;
        const XrXirTypeNode *node=xr_xir_type_node(types,members[i]);
        bool nested=node && xr_xir_data_union_shape(types,members[i]);
        if(node && !nested && !data_union_member(types,members[i]))return XR_XIR_BAD_TYPE;
        total+=nested?node->parameter_count:1u;
        if(total>UINT32_MAX)return XR_XIR_OVERFLOW;
    }
    if(total>context->limits.parameters)return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    XrXirType *vector=(XrXirType *)xir_compile_calloc(context,(size_t)total,sizeof(*vector),&status);
    if(!vector)return status;
    uint32_t used=0;
    for(uint32_t i=0;i<count;++i) {
        const XrXirTypeNode *node=xr_xir_type_node(types,members[i]);
        bool nested=node && xr_xir_data_union_shape(types,members[i]);
        uint32_t n=nested?node->parameter_count:1u;
        for(uint32_t p=0;p<n;++p) {
            XrXirType member=nested?node->parameters[p].type:members[i];
            if(!data_union_member(types,member) ||
                !data_union_insert(vector,&used,(uint32_t)total,member)) {
                xr_compile_resources_free(vector);
                return XR_XIR_BAD_TYPE;
            }
        }
    }
    /* A union whose Class members are each other's ancestor is rejected rather
     * than normalized. Keeping only the base would lose the dynamic identity
     * `is Derived` needs; keeping only the derived would reject base values.
     * Rejection is the unambiguous, fail-closed reading of the exclusivity
     * rule: distinct Class identities in one union must be unrelated. This also
     * covers a Class and an interface it implements. */
    if(types->nominals) for(uint32_t a=0;a<used;++a) {
        if(!xr_xir_type_is_class(types,vector[a]))continue;
        for(uint32_t b=a+1;b<used;++b) {
            if(!xr_xir_type_is_class(types,vector[b]))continue;
            if(xr_xir_class_is_subtype(types,vector[a],vector[b]) ||
                xr_xir_class_is_subtype(types,vector[b],vector[a])) {
                xr_compile_resources_free(vector);
                return XR_XIR_BAD_TYPE;
            }
        }
    }
    *output=(XrXirDataUnion){vector,used};return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_data_union_select(const XrXirTypes *types,XrXirType actual,
    XrXirType target,XrXirType *output) {
    if(!output || !xr_xir_data_union_shape(types,target))return XR_XIR_BAD_TYPE;
    if(!xr_xir_data_union_contains(types,target,actual))return XR_XIR_BAD_TYPE;
    *output=actual;return XR_XIR_OK;
}
