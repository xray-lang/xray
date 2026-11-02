/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_os_native.h - Governed host-query signatures and module authority
 *
 * KEY CONCEPT:
 *   Each host fact belongs to the actual standard os module and returns
 *   an owned runtime value, independent of same-named lexical declarations.
 */
#ifndef XXIR_OS_NATIVE_H
#define XXIR_OS_NATIVE_H
#include "xxir_declarations.h"
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"
#include <string.h>

static inline bool xir_os_native_id(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_OS_PLATFORM || id==XR_NATIVE_CALLABLE_OS_ARCH || id==XR_NATIVE_CALLABLE_OS_GETENV || id==XR_NATIVE_CALLABLE_OS_SETENV || id==XR_NATIVE_CALLABLE_OS_UNSETENV || id==XR_NATIVE_CALLABLE_OS_ENVIRON_BLOCK || id==XR_NATIVE_CALLABLE_OS_EXIT || id==XR_NATIVE_CALLABLE_OS_GETPID || id==XR_NATIVE_CALLABLE_OS_GETCWD;
}
static inline bool xir_os_native_signature(uint32_t id,const XrXirTypes *types,const XrXirType *parameters,
    uint32_t count,XrXirType result,XrXirType error) {
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if(!xir_os_native_id(id) || !native || native->owner || native->error_e_type || native->ctfe || !native->namespace_id ||
        strcmp(native->namespace_id,"os") || !native->source_path ||
        strcmp(native->source_path,"stdlib/defs/core.def") || !native->member) return false;
    const XrNativeMemberDeclaration *member=native->member;
    bool pid=id==XR_NATIVE_CALLABLE_OS_GETPID;
    bool environment=id==XR_NATIVE_CALLABLE_OS_GETENV,remove=id==XR_NATIVE_CALLABLE_OS_UNSETENV;
    bool snapshot=id==XR_NATIVE_CALLABLE_OS_ENVIRON_BLOCK,terminate=id==XR_NATIVE_CALLABLE_OS_EXIT;
    bool setter=id==XR_NATIVE_CALLABLE_OS_SETENV || remove;
    const char *name=id==XR_NATIVE_CALLABLE_OS_GETCWD?"__getcwd":pid?"__getpid":terminate?"__exit":snapshot?"__environBlock":remove?"__unsetenv":setter?"__setenv":environment?"__getenv":id==XR_NATIVE_CALLABLE_OS_PLATFORM?"__platform":"__arch";
    if(!member->name || strcmp(member->name,name) || !member->signature ||
        strcmp(member->signature,pid?"(): i64":terminate?"(code: i64): ()":snapshot?"(): Array<string>":remove?"(name: string): bool":setter?"(name: string, value: string): bool":environment?"(name: string): string?":"(): string") ||
        !member->result_text || strcmp(member->result_text,pid?"i64":terminate?"()":snapshot?"Array<string>":setter?"bool":environment?"string?":"string") ||
        member->is_public || member->is_method || member->is_static ||
        member->receiver!=XR_NATIVE_RECEIVER_READ || member->operation!=XR_NATIVE_OPERATION_NONE ||
        member->allocation!=(pid?XR_NATIVE_ALLOCATION_NO_HEAP:XR_NATIVE_ALLOCATION_UNKNOWN) ||
        !member->failures || (pid?strcmp(member->failures,"none")!=0:*member->failures!=0) ||
        member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED || error!=XR_XIR_NEVER) return false;
    if(pid)return !member->parameters && !member->parameter_count && !parameters && !count &&
        member->result==XR_NATIVE_TERM_I64 && result==XR_XIR_I64;
    if(terminate) {
        const XrNativeParameter *parameter=member->parameters;
        return count==1 && parameters && parameters[0]==XR_XIR_I64 && result==XR_XIR_UNIT &&
            member->result==XR_NATIVE_TERM_UNIT && member->parameter_count==1 && parameter &&
            parameter->name && !strcmp(parameter->name,"code") && parameter->type_text &&
            !strcmp(parameter->type_text,"i64") && parameter->type==XR_NATIVE_TERM_I64 &&
            !parameter->optional && !parameter->variadic;
    }
    if(snapshot) {
        const XrXirTypeNode *node=xr_xir_type_node(types,result);
        return !member->parameters && !member->parameter_count && !parameters && !count &&
            member->result==XR_NATIVE_TERM_ARRAY_STRING && node && node->kind==XR_XIR_TYPE_ARRAY &&
            !node->parameter_span && node->element==XR_XIR_STRING;
    }
    if(!environment && !setter)return !member->parameters && !member->parameter_count &&
        member->result==XR_NATIVE_TERM_STRING && !parameters && !count && result==XR_XIR_STRING;
    uint32_t expected=setter&&!remove?2u:1u;
    if(count!=expected || !parameters || member->parameter_count!=expected || !member->parameters)return false;
    for(uint32_t p=0;p<expected;++p) {
        const XrNativeParameter *parameter=member->parameters+p;
        if(!parameter->name || strcmp(parameter->name,p?"value":"name") || !parameter->type_text ||
            strcmp(parameter->type_text,"string") || parameter->type!=XR_NATIVE_TERM_STRING ||
            parameter->optional || parameter->variadic || parameters[p]!=XR_XIR_STRING)return false;
    }
    if(setter)return member->result==XR_NATIVE_TERM_BOOL && result==XR_XIR_BOOL;
    const XrXirTypeNode *node=xr_xir_type_node(types,result);
    return member->result==XR_NATIVE_TERM_NULLABLE_STRING && node && node->kind==XR_XIR_TYPE_NULLABLE &&
        !node->parameter_span && node->element==XR_XIR_STRING;
}
static inline bool xir_os_native_scope(const XrXirDeclarations *declarations,
    uint32_t function,uint32_t count) {
    static const char identity[]="stdlib-module-v1:module=2:os:path=8:os/os.xr";
    if(!declarations || !declarations->functions || !declarations->modules || function>=count) return false;
    uint32_t owner=declarations->functions[function].module;
    if(owner>=declarations->module_count) return false;
    const XrXirSourceModule *module=declarations->modules+owner;
    return module->name && module->name_length==sizeof(identity)-1 && !module->lexical_scope &&
        !memcmp(module->name,identity,sizeof(identity)-1);
}
#endif // XXIR_OS_NATIVE_H
