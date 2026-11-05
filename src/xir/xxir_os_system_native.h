/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_os_system_native.h - Exact governed system fact contracts
 *
 * KEY CONCEPT:
 *   Native identity never grants a different signature or module authority.
 */
#ifndef XXIR_OS_SYSTEM_NATIVE_H
#define XXIR_OS_SYSTEM_NATIVE_H
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"
#include <string.h>
typedef struct XirOsSystemContract {
    uint32_t id;
    const char *name,*signature,*result_text;
    XrNativeTypeTerm term;
    XrXirType result,element;
    uint32_t parameters;
} XirOsSystemContract;
static const XirOsSystemContract xir_os_system_contracts[]={
    {XR_NATIVE_CALLABLE_OS_UID,"__uid","(): i64?","i64?",XR_NATIVE_TERM_NULLABLE_I64,XR_XIR_UNIT,XR_XIR_I64,0},
    {XR_NATIVE_CALLABLE_OS_GID,"__gid","(): i64?","i64?",XR_NATIVE_TERM_NULLABLE_I64,XR_XIR_UNIT,XR_XIR_I64,0},
    {XR_NATIVE_CALLABLE_OS_PPID,"__ppid","(): i64","i64",XR_NATIVE_TERM_I64,XR_XIR_I64,XR_XIR_UNIT,0},
    {XR_NATIVE_CALLABLE_OS_KILL,"__kill","(pid: i64, signal: i64): bool","bool",XR_NATIVE_TERM_BOOL,XR_XIR_BOOL,XR_XIR_UNIT,2},
    {XR_NATIVE_CALLABLE_OS_TOTAL_MEMORY,"__totalMemory","(): i64","i64",XR_NATIVE_TERM_I64,XR_XIR_I64,XR_XIR_UNIT,0},
    {XR_NATIVE_CALLABLE_OS_FREE_MEMORY,"__freeMemory","(): i64","i64",XR_NATIVE_TERM_I64,XR_XIR_I64,XR_XIR_UNIT,0},
    {XR_NATIVE_CALLABLE_OS_UPTIME,"__uptime","(): f64","f64",XR_NATIVE_TERM_F64,XR_XIR_F64,XR_XIR_UNIT,0},
    {XR_NATIVE_CALLABLE_OS_LOADAVG,"__loadavg","(): Array<f64>?","Array<f64>?",XR_NATIVE_TERM_NULLABLE_ARRAY_F64,XR_XIR_UNIT,XR_XIR_F64,0}
};
static inline const XirOsSystemContract *xir_os_system_contract(uint32_t id) {
    for(size_t i=0;i<sizeof(xir_os_system_contracts)/sizeof(*xir_os_system_contracts);++i)
        if(xir_os_system_contracts[i].id==id)return xir_os_system_contracts+i;
    return NULL;
}
static inline bool xir_os_system_signature(const XirOsSystemContract *contract,
    const XrNativeMemberDeclaration *member,const XrXirTypes *types,
    const XrXirType *parameters,uint32_t count,XrXirType result,XrXirType error) {
    if(!contract || !member || !member->name || strcmp(member->name,contract->name) ||
        !member->signature || strcmp(member->signature,contract->signature) ||
        !member->result_text || strcmp(member->result_text,contract->result_text) ||
        member->is_public || member->is_method || member->is_static || member->lowered ||
        member->receiver!=XR_NATIVE_RECEIVER_READ || member->operation!=XR_NATIVE_OPERATION_NONE ||
        member->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN || !member->failures || *member->failures ||
        member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED || member->result!=contract->term ||
        member->parameter_count!=contract->parameters || count!=contract->parameters ||
        (!!parameters!=!!count) || (!!member->parameters!=!!count) || error!=XR_XIR_NEVER)return false;
    for(uint32_t i=0;i<count;++i) {
        const XrNativeParameter *parameter=member->parameters+i;
        if(parameters[i]!=XR_XIR_I64 || !parameter->name || strcmp(parameter->name,i?"signal":"pid") ||
            !parameter->type_text || strcmp(parameter->type_text,"i64") ||
            parameter->type!=XR_NATIVE_TERM_I64 || parameter->optional || parameter->variadic)return false;
    }
    if(contract->element==XR_XIR_UNIT)return result==contract->result;
    const XrXirTypeNode *node=xr_xir_type_node(types,result);
    if(!node || node->kind!=XR_XIR_TYPE_NULLABLE || node->parameter_span)return false;
    if(contract->term!=XR_NATIVE_TERM_NULLABLE_ARRAY_F64)return node->element==contract->element;
    const XrXirTypeNode *array=xr_xir_type_node(types,node->element);
    return array && array->kind==XR_XIR_TYPE_ARRAY &&
        array->element==XR_XIR_F64 && !array->parameter_span;
}
#endif
