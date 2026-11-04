/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_sys_native.h - Governed OS-thread signatures and module authority
 *
 * KEY CONCEPT:
 *   The private syscall belongs to the actual standard sys module, not to a
 *   same-named function or to the task scheduler.
 */
#ifndef XXIR_SYS_NATIVE_H
#define XXIR_SYS_NATIVE_H
#include "xxir_declarations.h"
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"
#include "xxir_sys_pipe_native.h"
#include "xxir_sys_process_native.h"
#include "xxir_sys_dylib_native.h"
#include <string.h>

static inline bool xir_sys_thread_token_id(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_SYS_THREAD_LOCAL_ID || id==XR_NATIVE_CALLABLE_SYS_THREAD_LOCAL_ALIVE;
}
static inline bool xir_sys_direct_native_id(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_SYS_ON_SIGNAL || xir_sys_thread_token_id(id) || xir_sys_pipe_id(id) || xir_sys_process_id(id) || xir_sys_dylib_id(id);
}
static inline bool xir_sys_native_id(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_SYS_THREAD_YIELD || id==XR_NATIVE_CALLABLE_SYS_PIN_TO_CPU ||
        xir_sys_direct_native_id(id);
}
static const uint32_t xir_sys_native_ids[]={XR_NATIVE_CALLABLE_SYS_THREAD_YIELD,
    XR_NATIVE_CALLABLE_SYS_PIN_TO_CPU,XR_NATIVE_CALLABLE_SYS_ON_SIGNAL,
    XR_NATIVE_CALLABLE_SYS_THREAD_LOCAL_ID,XR_NATIVE_CALLABLE_SYS_THREAD_LOCAL_ALIVE,
    XR_NATIVE_CALLABLE_SYS_PIPE_OPEN,XR_NATIVE_CALLABLE_SYS_PIPE_READ,
    XR_NATIVE_CALLABLE_SYS_PIPE_WRITE,XR_NATIVE_CALLABLE_SYS_PIPE_CLOSE,
    XR_NATIVE_CALLABLE_SYS_PROCESS_SPAWN,XR_NATIVE_CALLABLE_SYS_PROCESS_TRY_WAIT,XR_NATIVE_CALLABLE_SYS_PROCESS_KILL,
    XR_NATIVE_CALLABLE_SYS_DYLIB_OPEN,XR_NATIVE_CALLABLE_SYS_DYLIB_SYMBOL,XR_NATIVE_CALLABLE_SYS_DYLIB_CLOSE,
    XR_NATIVE_CALLABLE_SYS_DYLIB_LAST_ERROR};
static inline bool xir_sys_native_signature(uint32_t id,const XrXirTypes *types,const XrXirType *parameters,
    uint32_t count,XrXirType result,XrXirType error) {
    if(xir_sys_dylib_id(id))return xir_sys_dylib_signature(id,types,parameters,count,result,error);
    if(xir_sys_process_id(id))return xir_sys_process_signature(id,types,parameters,count,result,error);
    if(xir_sys_pipe_id(id))return xir_sys_pipe_signature(id,types,parameters,count,result,error);
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    bool pin=id==XR_NATIVE_CALLABLE_SYS_PIN_TO_CPU,signal=id==XR_NATIVE_CALLABLE_SYS_ON_SIGNAL;
    bool token=id==XR_NATIVE_CALLABLE_SYS_THREAD_LOCAL_ID,alive=id==XR_NATIVE_CALLABLE_SYS_THREAD_LOCAL_ALIVE;
    const char *name=signal?"__onSignal":pin?"__pinToCpu":token?"__threadLocalId":alive?"__threadLocalAlive":"__threadYield";
    const char *signature=signal?"(signal: i64, handler: fn(): ()): bool":
        pin?"(cpu: i64): bool":token?"(): i64":alive?"(id: i64): bool":"(): ()";
    if(!xir_sys_native_id(id) || !native || native->owner || native->error_e_type || native->ctfe || !native->namespace_id ||
        strcmp(native->namespace_id,"sys") || !native->source_path ||
        strcmp(native->source_path,"stdlib/defs/core.def") || !native->member) return false;
    const XrNativeMemberDeclaration *member=native->member;
    if(!member->name || strcmp(member->name,name) || !member->signature ||
        strcmp(member->signature,signature) || !member->result_text ||
        strcmp(member->result_text,token?"i64":pin||signal||alive?"bool":"()") || member->is_public || member->is_method ||
        member->is_static || member->lowered ||
        member->receiver!=XR_NATIVE_RECEIVER_READ || member->operation!=XR_NATIVE_OPERATION_NONE ||
        member->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN || !member->failures || *member->failures ||
        member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED || error!=XR_XIR_NEVER) return false;
    if(signal) {
        if(!member->parameters || member->parameter_count!=2 || count!=2 || !parameters ||
            parameters[0]!=XR_XIR_I64 || result!=XR_XIR_BOOL || member->result!=XR_NATIVE_TERM_BOOL) return false;
        const XrNativeParameter *a=member->parameters,*b=a+1;
        if(!a->name || strcmp(a->name,"signal") || !a->type_text || strcmp(a->type_text,"i64") ||
            a->type!=XR_NATIVE_TERM_I64 || a->optional || a->variadic ||
            !b->name || strcmp(b->name,"handler") || !b->type_text || strcmp(b->type_text,"fn(): ()") ||
            b->type!=XR_NATIVE_TERM_UNIT_CALLBACK || b->optional || b->variadic) return false;
        const XrXirTypeNode *handler=xr_xir_type_node(types,parameters[1]);
        return handler && handler->kind==XR_XIR_TYPE_CALLABLE && !handler->parameter_span && !handler->parameter_count && !handler->parameters &&
            handler->result==XR_XIR_UNIT && handler->element==XR_XIR_NEVER &&
            handler->flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    }
    if(!pin && !alive)return
        !member->parameters && !member->parameter_count &&
        member->result==(token?XR_NATIVE_TERM_I64:XR_NATIVE_TERM_UNIT) && !parameters && !count &&
        result==(token?XR_XIR_I64:XR_XIR_UNIT);
    const XrNativeParameter *parameter=member->parameters;
    return parameter && member->parameter_count==1 && count==1 && parameters && parameters[0]==XR_XIR_I64 &&
        parameter->name && !strcmp(parameter->name,alive?"id":"cpu") && parameter->type_text && !strcmp(parameter->type_text,"i64") &&
        parameter->type==XR_NATIVE_TERM_I64 && !parameter->optional && !parameter->variadic &&
        member->result==XR_NATIVE_TERM_BOOL && result==XR_XIR_BOOL;
}
static inline bool xir_sys_native_scope(const XrXirDeclarations *declarations,
    uint32_t function,uint32_t count) {
    static const char identity[]="stdlib-module-v1:module=3:sys:path=10:sys/sys.xr";
    if(!declarations || !declarations->functions || !declarations->modules || function>=count) return false;
    uint32_t owner=declarations->functions[function].module;
    if(owner>=declarations->module_count) return false;
    const XrXirSourceModule *module=declarations->modules+owner;
    return module->name && module->name_length==sizeof(identity)-1 && !module->lexical_scope &&
        !memcmp(module->name,identity,sizeof(identity)-1);
}
#endif // XXIR_SYS_NATIVE_H
