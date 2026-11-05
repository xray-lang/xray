/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_body_array_members.inc.c - Governed Array declaration and recipe records
 *
 * KEY CONCEPT:
 *   Immutable native declarations establish member identity. A caller-supplied
 *   operation number cannot authorize an Array recipe or change its role.
 */
static XrHirArrayRecipe body_array_recipe(XrNativeOperation operation) {
    switch(operation) {
    case XR_NATIVE_OPERATION_ARRAY_MAP:return XR_HIR_ARRAY_MAP;
    case XR_NATIVE_OPERATION_ARRAY_FILTER:return XR_HIR_ARRAY_FILTER;
    case XR_NATIVE_OPERATION_ARRAY_REDUCE:return XR_HIR_ARRAY_REDUCE;
    case XR_NATIVE_OPERATION_ARRAY_FOR_EACH:return XR_HIR_ARRAY_FOR_EACH;
    case XR_NATIVE_OPERATION_ARRAY_FIND:return XR_HIR_ARRAY_FIND;
    case XR_NATIVE_OPERATION_ARRAY_FIND_INDEX:return XR_HIR_ARRAY_FIND_INDEX;
    case XR_NATIVE_OPERATION_ARRAY_EVERY:return XR_HIR_ARRAY_EVERY;
    case XR_NATIVE_OPERATION_ARRAY_SOME:return XR_HIR_ARRAY_SOME;
    case XR_NATIVE_OPERATION_ARRAY_CONTAINS:return XR_HIR_ARRAY_CONTAINS;
    case XR_NATIVE_OPERATION_ARRAY_INDEX_OF:return XR_HIR_ARRAY_INDEX_OF;
    case XR_NATIVE_OPERATION_ARRAY_JOIN:return XR_HIR_ARRAY_JOIN;
    case XR_NATIVE_OPERATION_ARRAY_CLEAR:return XR_HIR_ARRAY_CLEAR;
    case XR_NATIVE_OPERATION_ARRAY_REVERSE:return XR_HIR_ARRAY_REVERSE;
    case XR_NATIVE_OPERATION_ARRAY_UNSHIFT:return XR_HIR_ARRAY_UNSHIFT;
    case XR_NATIVE_OPERATION_ARRAY_POP:return XR_HIR_ARRAY_POP;
    case XR_NATIVE_OPERATION_ARRAY_SHIFT:return XR_HIR_ARRAY_SHIFT;
    case XR_NATIVE_OPERATION_ARRAY_RESIZE:return XR_HIR_ARRAY_RESIZE;
    case XR_NATIVE_OPERATION_ARRAY_ENTRIES:return XR_HIR_ARRAY_ENTRIES;
    case XR_NATIVE_OPERATION_ARRAY_FILL:return XR_HIR_ARRAY_FILL;
    case XR_NATIVE_OPERATION_ARRAY_CONCAT:return XR_HIR_ARRAY_CONCAT;
    default:return XR_HIR_ARRAY_NONE;
    }
}
XR_FUNC XrXirStatus xr_hir_array_member_view(const XrNativeMemberDeclaration *member,
    XrCompileRequestControl *control,XrHirArrayMemberInfo *out,const char **reason) {
    if(reason)*reason=NULL;if(!out)return XR_XIR_BAD_STRUCTURE;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    if(!native||!native->members)return XR_XIR_BAD_STRUCTURE;
    for(uint32_t m=0;m<native->member_count;++m) {
        if(!(m%64)){XrXirStatus status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        if(&native->members[m]!=member)continue;
        if(!member->is_public||member->operation==XR_NATIVE_OPERATION_NONE)break;
        XrHirArrayMemberRole role=member->is_static?XR_HIR_ARRAY_STATIC:member->is_method?XR_HIR_ARRAY_INSTANCE:XR_HIR_ARRAY_PROPERTY;
        *out=(XrHirArrayMemberInfo){m,member->id,member->parameter_count,member->operation,member->receiver,body_array_recipe(member->operation),role};
        return XR_XIR_OK;
    }
    return body_array_failure(reason,"Array member has no admitted value-operation contract");
}
XR_FUNC XrHirArrayRecipe xr_hir_array_recipe(const XrNativeMemberDeclaration *member) {
    XrHirArrayMemberInfo info={0};
    return xr_hir_array_member_view(member,NULL,&info,NULL)==XR_XIR_OK?info.recipe:XR_HIR_ARRAY_NONE;
}
XR_FUNC XrXirStatus xr_hir_body_array_member(XrHirBody *body,const XrXirTypes *types,
    const XrNativeMemberDeclaration *member,XrHirArrayMemberRole role,XrCompileRequestControl *control,
    XrHirArrayMember *out,XrHirArrayMemberInfo *info,const char **reason) {
    if(!body||body->ended||body->array_sealed||types!=body->types||!out||!info)return XR_XIR_BAD_STRUCTURE;
    if(role!=XR_HIR_ARRAY_INSTANCE&&role!=XR_HIR_ARRAY_STATIC&&role!=XR_HIR_ARRAY_PROPERTY)return XR_XIR_BAD_STRUCTURE;
    XrHirArrayMemberInfo received={0};XrXirStatus status=xr_hir_array_member_view(member,control,&received,reason);
    if(status!=XR_XIR_OK)return status;
    if(received.role!=role)return body_array_failure(reason,role==XR_HIR_ARRAY_INSTANCE?"Array call requires an instance method":
        role==XR_HIR_ARRAY_STATIC?"Array static member requires its declared ordinary arguments":"Array member is not an instance property");
    for(uint32_t i=0;i<body->array_members.count;++i) {
        if(!(i%64)){status=body_poll(control);if(status!=XR_XIR_OK)return status;}
        XrHirArrayMemberInfo row=((XrHirArrayMemberInfo *)body->array_members.items)[i];
        if(row.ordinal==received.ordinal){*out=(XrHirArrayMember){body,i};*info=row;return XR_XIR_OK;}
    }
    status=body_vector_reserve(body,&body->array_members,sizeof(received),control);if(status!=XR_XIR_OK)return status;
    uint32_t index=body->array_members.count++;((XrHirArrayMemberInfo *)body->array_members.items)[index]=received;
    *out=(XrHirArrayMember){body,index};*info=received;return XR_XIR_OK;
}
XR_FUNC bool xr_hir_body_array_member_info(const XrHirBody *body,XrHirArrayMember handle,XrHirArrayMemberInfo *out) {
    if(!body||body->ended||handle.owner!=body||handle.index>=body->array_members.count||!out)return false;
    *out=((XrHirArrayMemberInfo *)body->array_members.items)[handle.index];return true;
}
