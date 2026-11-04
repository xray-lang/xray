/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_sys_process_native.h - Complete process declarations and exact carriers
 */
#ifndef XXIR_SYS_PROCESS_NATIVE_H
#define XXIR_SYS_PROCESS_NATIVE_H
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"
#include <string.h>
static inline bool xir_sys_process_id(uint32_t id) {
    return id>=XR_NATIVE_CALLABLE_SYS_PROCESS_SPAWN&&id<=XR_NATIVE_CALLABLE_SYS_PROCESS_KILL;
}
static inline bool xir_sys_process_term(const XrXirTypes *types,XrXirType type,XrNativeTypeTerm term) {
    if(term==XR_NATIVE_TERM_I64)return type==XR_XIR_I64;
    if(term==XR_NATIVE_TERM_BOOL)return type==XR_XIR_BOOL;
    if(term==XR_NATIVE_TERM_STRING)return type==XR_XIR_STRING;
    if(term==XR_NATIVE_TERM_ARRAY_STRING)
        return xr_xir_type_is_array(types,type)&&xr_xir_array_element(types,type)==XR_XIR_STRING;
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!node||node->kind!=XR_XIR_TYPE_NULLABLE||node->parameter_span)return false;
    if(term==XR_NATIVE_TERM_NULLABLE_I64)return node->element==XR_XIR_I64;
    if(term==XR_NATIVE_TERM_NULLABLE_STRING)return node->element==XR_XIR_STRING;
    return term==XR_NATIVE_TERM_NULLABLE_ARRAY_STRING&&
        xir_sys_process_term(types,node->element,XR_NATIVE_TERM_ARRAY_STRING);
}
static inline bool xir_sys_process_signature(uint32_t id,const XrXirTypes *types,
    const XrXirType *parameters,uint32_t count,XrXirType result,XrXirType error) {
    if(!xir_sys_process_id(id)||!types||error!=XR_XIR_NEVER)return false;
    static const char *names[]={"__processSpawn","__processTryWait","__processKill"};
    static const char *signatures[]={
        "(program: string, args: Array<string>, cwd: string?, envKeys: Array<string>?, envValues: Array<string>?, stdinRead: i64?, stdoutWrite: i64?, stderrWrite: i64?, detached: bool): i64",
        "(id: i64): i64?","(id: i64, signal: i64): bool"};
    static const char *results[]={"i64","i64?","bool"};
    static const char *spawn_names[]={"program","args","cwd","envKeys","envValues","stdinRead","stdoutWrite","stderrWrite","detached"};
    static const char *spawn_text[]={"string","Array<string>","string?","Array<string>?","Array<string>?","i64?","i64?","i64?","bool"};
    static const XrNativeTypeTerm spawn_terms[]={XR_NATIVE_TERM_STRING,XR_NATIVE_TERM_ARRAY_STRING,
        XR_NATIVE_TERM_NULLABLE_STRING,XR_NATIVE_TERM_NULLABLE_ARRAY_STRING,XR_NATIVE_TERM_NULLABLE_ARRAY_STRING,
        XR_NATIVE_TERM_NULLABLE_I64,XR_NATIVE_TERM_NULLABLE_I64,XR_NATIVE_TERM_NULLABLE_I64,XR_NATIVE_TERM_BOOL};
    static const XrNativeTypeTerm result_terms[]={XR_NATIVE_TERM_I64,XR_NATIVE_TERM_NULLABLE_I64,XR_NATIVE_TERM_BOOL};
    uint32_t at=id-XR_NATIVE_CALLABLE_SYS_PROCESS_SPAWN,wanted=at==0?9:at==1?1:2;
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if(!native||native->owner||native->error_e_type||native->ctfe||!native->namespace_id||strcmp(native->namespace_id,"sys")||
        !native->source_path||strcmp(native->source_path,"stdlib/defs/core.def")||!native->member)return false;
    const XrNativeMemberDeclaration *member=native->member;
    if(!member->name||strcmp(member->name,names[at])||!member->signature||strcmp(member->signature,signatures[at])||
        !member->result_text||strcmp(member->result_text,results[at])||member->result!=result_terms[at]||
        member->is_public||member->is_method||member->is_static||member->lowered||
        member->receiver!=XR_NATIVE_RECEIVER_READ||member->operation!=XR_NATIVE_OPERATION_NONE||
        member->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN||!member->failures||*member->failures||
        member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED||member->parameter_count!=wanted||count!=wanted||
        !parameters||!member->parameters||!xir_sys_process_term(types,result,result_terms[at]))return false;
    for(uint32_t p=0;p<wanted;++p) {
        const XrNativeParameter *parameter=member->parameters+p;
        const char *name=at==0?spawn_names[p]:p?"signal":"id";
        const char *text=at==0?spawn_text[p]:"i64";
        XrNativeTypeTerm term=at==0?spawn_terms[p]:XR_NATIVE_TERM_I64;
        if(!parameter->name||strcmp(parameter->name,name)||!parameter->type_text||strcmp(parameter->type_text,text)||
            parameter->type!=term||parameter->optional||parameter->variadic||!xir_sys_process_term(types,parameters[p],term))return false;
    }
    return true;
}
#endif // XXIR_SYS_PROCESS_NATIVE_H
