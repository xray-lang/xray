/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_aggregate_auth.h - Canonical aggregate signatures over real Tasks
 *
 * KEY CONCEPT:
 *   A complete declaration identifies the leaf. Its actual Array<Task<T,E>>
 *   supplies both result components; a requested result type grants nothing.
 */
#ifndef XXIR_TASK_AGGREGATE_AUTH_H
#define XXIR_TASK_AGGREGATE_AUTH_H
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"
#include <string.h>
static inline bool xir_task_aggregate_native(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_TASK_ALL_BEGIN||id==XR_NATIVE_CALLABLE_TASK_FIRST_BEGIN;
}
/* The complete Module or arena has already validated the descriptor graph.
 * Native generic binders remain closed; no contextual parameter is invented. */
static inline bool xir_task_aggregate_signature(uint32_t id,const XrXirTypes *types,
    const XrXirType *parameters,uint32_t count,XrXirType result,XrXirType error) {
    bool all=id==XR_NATIVE_CALLABLE_TASK_ALL_BEGIN;
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if(!xir_task_aggregate_native(id)||!native||!native->owner||!native->member||
        native->error_e_type||native->ctfe||native->namespace_id||native->source_path||
        native->owner->id!=XR_NATIVE_DECLARATION_TASK||!xr_native_declaration_validate(native->owner)||
        !native->owner->source_path||strcmp(native->owner->source_path,"stdlib/types/coroutine.xr")||
        !types||count!=1||!parameters||error!=XR_XIR_NEVER||
        xr_xir_type_span(types,parameters[0])||xr_xir_type_span(types,result))return false;
    const XrNativeMemberDeclaration *member=native->member;
    if(native->owner->member_count<(all?4u:5u)||member!=native->owner->members+(all?3:4)||
        member->id!=(all?4u:5u)||!member->name||strcmp(member->name,all?"__allBegin":"__firstBegin")||
        !member->signature||strcmp(member->signature,all?
            "(tasks: Array<Task<T, E>>) -> Task<Array<T>, E>":"(tasks: Array<Task<T, E>>) -> Task<T, E>")||
        !member->is_method||!member->is_static||member->is_public||member->lowered||
        member->receiver!=XR_NATIVE_RECEIVER_READ||member->operation!=XR_NATIVE_OPERATION_NONE||
        member->allocation!=XR_NATIVE_ALLOCATION_MAY_HEAP||member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED||
        !member->failures||strcmp(member->failures,"allocation,retain,limit,unsupported")||
        member->parameter_count!=1||!member->parameters||
        member->parameters[0].type!=XR_NATIVE_TERM_ARRAY_TASK||member->parameters[0].optional||
        member->parameters[0].variadic||member->result!=(all?XR_NATIVE_TERM_TASK_ARRAY:XR_NATIVE_TERM_TASK_ELEMENT))return false;
    const XrXirTypeNode *array=xr_xir_type_node(types,parameters[0]);
    const XrXirTypeNode *input=array&&array->kind==XR_XIR_TYPE_ARRAY?xr_xir_type_node(types,array->element):NULL;
    const XrXirTypeNode *output=xr_xir_type_node(types,result);
    if(!input||input->kind!=XR_XIR_TYPE_TASK||!output||output->kind!=XR_XIR_TYPE_TASK||
        input->result!=output->result)return false;
    if(!all)return output->element==input->element;
    const XrXirTypeNode *values=xr_xir_type_node(types,output->element);
    return values&&values->kind==XR_XIR_TYPE_ARRAY&&values->element==input->element;
}
#endif // XXIR_TASK_AGGREGATE_AUTH_H
