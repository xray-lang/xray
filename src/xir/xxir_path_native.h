/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_path_native.h - Exact private current-directory authority for Path
 */
#ifndef XXIR_PATH_NATIVE_H
#define XXIR_PATH_NATIVE_H
#include "xxir_path_prelude.h"
#include "../shared/xnative_declaration.h"
static inline bool xir_path_native_id(uint32_t id) {return id==XR_NATIVE_CALLABLE_PATH_GETCWD;}
static inline bool xir_path_native_scope(const XrXirDeclarations *scope,uint32_t function,uint32_t count) {
    if(!scope||!scope->functions||!scope->modules||function>=count)return false;
    uint32_t owner=scope->functions[function].module;if(owner>=scope->module_count)return false;
    const XrXirSourceModule *module=scope->modules+owner;char logical[64],canonical[160];
    return xir_path_prelude_coordinates(logical,canonical)&&module->name&&!module->lexical_scope&&
        module->name_length==strlen(canonical)&&!memcmp(module->name,canonical,module->name_length);
}
static inline bool xir_path_native_signature(uint32_t id,const XrXirType *parameters,
    uint32_t count,XrXirType result,XrXirType error) {
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if(!xir_path_native_id(id)||!native||native->owner||native->error_e_type||native->ctfe||
        !native->namespace_id||strcmp(native->namespace_id,"path")||!native->source_path||
        strcmp(native->source_path,"stdlib/defs/core.def")||!native->member)return false;
    const XrNativeMemberDeclaration *m=native->member;
    return m->name&&!strcmp(m->name,"__getcwd")&&m->signature&&!strcmp(m->signature,"(): string")&&
        m->result_text&&!strcmp(m->result_text,"string")&&!m->is_public&&!m->is_method&&!m->is_static&&!m->lowered&&
        m->receiver==XR_NATIVE_RECEIVER_READ&&m->operation==XR_NATIVE_OPERATION_NONE&&
        m->allocation==XR_NATIVE_ALLOCATION_UNKNOWN&&m->failures&&!*m->failures&&
        m->ownership==XR_NATIVE_RESULT_OWNERSHIP_OWNED&&!m->parameters&&!m->parameter_count&&
        m->result==XR_NATIVE_TERM_STRING&&!parameters&&!count&&result==XR_XIR_STRING&&error==XR_XIR_NEVER;
}
#endif // XXIR_PATH_NATIVE_H
