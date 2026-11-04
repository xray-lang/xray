/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_os_exec_native.h - Exact private endpoint cleanup for shell capture
 */
#ifndef XXIR_OS_EXEC_NATIVE_H
#define XXIR_OS_EXEC_NATIVE_H
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"
#include <string.h>
static inline bool xir_os_exec_close_id(uint32_t id) {return id==XR_NATIVE_CALLABLE_OS_EXEC_PIPE_CLOSE;}
static inline bool xir_os_exec_shell_id(uint32_t id) {return id==XR_NATIVE_CALLABLE_OS_EXEC_SHELL_SPAWN;}
static inline bool xir_os_exec_native_id(uint32_t id) {return xir_os_exec_close_id(id)||xir_os_exec_shell_id(id);}
static inline bool xir_os_exec_close_signature(uint32_t id,const XrXirType *parameters,
    uint32_t count,XrXirType result,XrXirType error) {
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if(!xir_os_exec_close_id(id)||!native||native->owner||native->error_e_type||native->ctfe||
        !native->namespace_id||strcmp(native->namespace_id,"os")||!native->source_path||
        strcmp(native->source_path,"stdlib/defs/core.def")||!native->member)return false;
    const XrNativeMemberDeclaration *m=native->member;
    if(!m->name||strcmp(m->name,"__execPipeClose")||!m->signature||
        strcmp(m->signature,"(readToken: i64, writeToken: i64): bool")||
        !m->result_text||strcmp(m->result_text,"bool")||m->is_public||m->is_method||m->is_static||m->lowered||
        m->receiver!=XR_NATIVE_RECEIVER_READ||m->operation!=XR_NATIVE_OPERATION_NONE||
        m->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN||!m->failures||*m->failures||
        m->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED||m->result!=XR_NATIVE_TERM_BOOL||
        m->parameter_count!=2||!m->parameters||count!=2||!parameters||result!=XR_XIR_BOOL||error!=XR_XIR_NEVER)return false;
    for(uint32_t p=0;p<2;++p) {
        const XrNativeParameter *parameter=m->parameters+p;
        if(!parameter->name||strcmp(parameter->name,p?"writeToken":"readToken")||
            !parameter->type_text||strcmp(parameter->type_text,"i64")||parameter->type!=XR_NATIVE_TERM_I64||
            parameter->optional||parameter->variadic||parameters[p]!=XR_XIR_I64)return false;
    }
    return true;
}
static inline bool xir_os_exec_shell_signature(uint32_t id,const XrXirType *parameters,
    uint32_t count,XrXirType result,XrXirType error) {
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if(!xir_os_exec_shell_id(id)||!native||native->owner||native->error_e_type||native->ctfe||
        !native->namespace_id||strcmp(native->namespace_id,"os")||!native->source_path||
        strcmp(native->source_path,"stdlib/defs/core.def")||!native->member)return false;
    const XrNativeMemberDeclaration *m=native->member;
    if(!m->name||strcmp(m->name,"__execShellSpawn")||!m->signature||
        strcmp(m->signature,"(command: string, stdoutToken: i64, stderrToken: i64): i64")||
        !m->result_text||strcmp(m->result_text,"i64")||m->is_public||m->is_method||m->is_static||m->lowered||
        m->receiver!=XR_NATIVE_RECEIVER_READ||m->operation!=XR_NATIVE_OPERATION_NONE||
        m->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN||!m->failures||*m->failures||
        m->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED||m->result!=XR_NATIVE_TERM_I64||
        m->parameter_count!=3||!m->parameters||count!=3||!parameters||result!=XR_XIR_I64||error!=XR_XIR_NEVER)return false;
    for(uint32_t p=0;p<3;++p) {
        const XrNativeParameter *parameter=m->parameters+p;
        const char *name=p==0?"command":p==1?"stdoutToken":"stderrToken";
        if(!parameter->name||strcmp(parameter->name,name)||!parameter->type_text||
            strcmp(parameter->type_text,p?"i64":"string")||parameter->type!=(p?XR_NATIVE_TERM_I64:XR_NATIVE_TERM_STRING)||
            parameter->optional||parameter->variadic||parameters[p]!=(p?XR_XIR_I64:XR_XIR_STRING))return false;
    }
    return true;
}
static inline bool xir_os_exec_native_signature(uint32_t id,const XrXirType *parameters,
    uint32_t count,XrXirType result,XrXirType error) {
    return xir_os_exec_close_signature(id,parameters,count,result,error)||
        xir_os_exec_shell_signature(id,parameters,count,result,error);
}
#endif // XXIR_OS_EXEC_NATIVE_H
