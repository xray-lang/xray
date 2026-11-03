/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_sys_native.h - Governed OS-thread yield signature and module authority
 *
 * KEY CONCEPT:
 *   The private syscall belongs to the actual standard sys module, not to a
 *   same-named function or to the task scheduler.
 */
#ifndef XXIR_SYS_NATIVE_H
#define XXIR_SYS_NATIVE_H
#include "xxir_declarations.h"
#include "../shared/xnative_declaration.h"
#include <string.h>

static inline bool xir_sys_native_id(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_SYS_THREAD_YIELD || id==XR_NATIVE_CALLABLE_SYS_PIN_TO_CPU;
}
static inline bool xir_sys_native_signature(uint32_t id,const XrXirType *parameters,
    uint32_t count,XrXirType result,XrXirType error) {
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    bool pin=id==XR_NATIVE_CALLABLE_SYS_PIN_TO_CPU;
    if(!xir_sys_native_id(id) || !native || native->owner || native->error_e_type || native->ctfe || !native->namespace_id ||
        strcmp(native->namespace_id,"sys") || !native->source_path ||
        strcmp(native->source_path,"stdlib/defs/core.def") || !native->member) return false;
    const XrNativeMemberDeclaration *member=native->member;
    if(!member->name || strcmp(member->name,pin?"__pinToCpu":"__threadYield") || !member->signature ||
        strcmp(member->signature,pin?"(cpu: i64): bool":"(): ()") || !member->result_text ||
        strcmp(member->result_text,pin?"bool":"()") || member->is_public || member->is_method ||
        member->is_static || member->lowered ||
        member->receiver!=XR_NATIVE_RECEIVER_READ || member->operation!=XR_NATIVE_OPERATION_NONE ||
        member->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN || !member->failures || *member->failures ||
        member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED || error!=XR_XIR_NEVER) return false;
    if(!pin)return
        !member->parameters && !member->parameter_count &&
        member->result==XR_NATIVE_TERM_UNIT && !parameters && !count && result==XR_XIR_UNIT;
    const XrNativeParameter *parameter=member->parameters;
    return parameter && member->parameter_count==1 && count==1 && parameters && parameters[0]==XR_XIR_I64 &&
        parameter->name && !strcmp(parameter->name,"cpu") && parameter->type_text && !strcmp(parameter->type_text,"i64") &&
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
