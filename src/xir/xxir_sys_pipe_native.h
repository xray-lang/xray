/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_sys_pipe_native.h - Exact private pipe signatures and suspension identity
 */
#ifndef XXIR_SYS_PIPE_NATIVE_H
#define XXIR_SYS_PIPE_NATIVE_H
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"
#include <string.h>
static inline bool xir_sys_pipe_id(uint32_t id) {
    return id>=XR_NATIVE_CALLABLE_SYS_PIPE_OPEN && id<=XR_NATIVE_CALLABLE_SYS_PIPE_CLOSE;
}
static inline bool xir_sys_pipe_can_suspend(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_SYS_PIPE_READ || id==XR_NATIVE_CALLABLE_SYS_PIPE_WRITE;
}
static inline bool xir_sys_pipe_array(const XrXirTypes *types,XrXirType type) {
    return xr_xir_type_is_array(types,type) && xr_xir_array_element(types,type)==XR_XIR_U8;
}
static inline bool xir_sys_pipe_signature(uint32_t id,const XrXirTypes *types,
    const XrXirType *parameters,uint32_t count,XrXirType result,XrXirType error) {
    if(!xir_sys_pipe_id(id) || !types || error!=XR_XIR_NEVER)return false;
    static const char *names[]={"__pipeOpen","__pipeRead","__pipeWrite","__pipeClose"};
    static const char *signatures[]={"(): (i64, i64)?","(handle: i64, maxBytes: i64): Array<u8>?",
        "(handle: i64, data: Array<u8>): i64","(handle: i64): bool"};
    static const char *results[]={"(i64, i64)?","Array<u8>?","i64","bool"};
    static const XrNativeTypeTerm terms[]={XR_NATIVE_TERM_NULLABLE_I64_PAIR,XR_NATIVE_TERM_NULLABLE_ARRAY_U8,
        XR_NATIVE_TERM_I64,XR_NATIVE_TERM_BOOL};
    uint32_t at=id-XR_NATIVE_CALLABLE_SYS_PIPE_OPEN,wanted=at==0?0:at==3?1:2;
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if(!native || native->owner || native->error_e_type || native->ctfe || !native->namespace_id ||
        strcmp(native->namespace_id,"sys") || !native->source_path || strcmp(native->source_path,"stdlib/defs/core.def") ||
        !native->member)return false;
    const XrNativeMemberDeclaration *member=native->member;
    if(!member->name || strcmp(member->name,names[at]) || !member->signature || strcmp(member->signature,signatures[at]) ||
        !member->result_text || strcmp(member->result_text,results[at]) || member->result!=terms[at] ||
        member->is_public || member->is_method || member->is_static || member->lowered ||
        member->receiver!=XR_NATIVE_RECEIVER_READ || member->operation!=XR_NATIVE_OPERATION_NONE ||
        member->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN || !member->failures || *member->failures ||
        member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED || count!=wanted || member->parameter_count!=wanted ||
        (wanted?(!parameters || !member->parameters):(parameters || member->parameters)))return false;
    for(uint32_t p=0;p<wanted;++p) {
        bool bytes=at==2 && p==1;const XrNativeParameter *parameter=member->parameters+p;
        const char *name=p?(bytes?"data":"maxBytes"):"handle";
        if(!parameter->name || strcmp(parameter->name,name) || !parameter->type_text ||
            strcmp(parameter->type_text,bytes?"Array<u8>":"i64") || parameter->optional || parameter->variadic ||
            parameter->type!=(bytes?XR_NATIVE_TERM_ARRAY_U8:XR_NATIVE_TERM_I64) ||
            (bytes?!xir_sys_pipe_array(types,parameters[p]):parameters[p]!=XR_XIR_I64))return false;
    }
    if(at>=2)return result==(at==2?XR_XIR_I64:XR_XIR_BOOL);
    const XrXirTypeNode *nullable=xr_xir_type_node(types,result);
    if(!nullable || nullable->kind!=XR_XIR_TYPE_NULLABLE || nullable->parameter_span)return false;
    if(at==1)return xir_sys_pipe_array(types,nullable->element);
    const XrXirTypeNode *pair=xr_xir_type_node(types,nullable->element);
    return pair && pair->kind==XR_XIR_TYPE_TUPLE && !pair->parameter_span && pair->parameter_count==2 && pair->parameters &&
        pair->parameters[0].type==XR_XIR_I64 && !pair->parameters[0].mode &&
        pair->parameters[1].type==XR_XIR_I64 && !pair->parameters[1].mode;
}
#endif // XXIR_SYS_PIPE_NATIVE_H
