/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_path_native.h - Exact private host authority for Path
 *
 * KEY CONCEPT:
 *   Only the real standard Path module receives the host facts its exported
 *   constants and resolve() are built from. The identity is the canonical
 *   module name, so a same-named declaration elsewhere is not a Path query.
 */
#ifndef XXIR_PATH_NATIVE_H
#define XXIR_PATH_NATIVE_H
#include "xxir_path_prelude.h"
#include "../shared/xnative_declaration.h"
static const uint32_t xir_path_native_ids[]={
    XR_NATIVE_CALLABLE_PATH_GETCWD,XR_NATIVE_CALLABLE_PATH_IS_WINDOWS,XR_NATIVE_CALLABLE_PATH_FULLPATH
};
static inline bool xir_path_native_id(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_PATH_GETCWD || id==XR_NATIVE_CALLABLE_PATH_IS_WINDOWS ||
        id==XR_NATIVE_CALLABLE_PATH_FULLPATH;
}
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
    if(!m->name||!m->signature||!m->result_text||m->is_public||m->is_method||m->is_static||m->lowered||
        m->receiver!=XR_NATIVE_RECEIVER_READ||m->operation!=XR_NATIVE_OPERATION_NONE||
        !m->failures||*m->failures||m->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED||error!=XR_XIR_NEVER)
        return false;
    if(id==XR_NATIVE_CALLABLE_PATH_GETCWD) {
        return !strcmp(m->name,"__getcwd")&&!strcmp(m->signature,"(): string")&&!strcmp(m->result_text,"string")&&
            m->allocation==XR_NATIVE_ALLOCATION_UNKNOWN&&!m->parameters&&!m->parameter_count&&
            m->result==XR_NATIVE_TERM_STRING&&!parameters&&!count&&result==XR_XIR_STRING;
    }
    if(id==XR_NATIVE_CALLABLE_PATH_IS_WINDOWS) {
        return !strcmp(m->name,"__isWindows")&&!strcmp(m->signature,"(): bool")&&!strcmp(m->result_text,"bool")&&
            m->allocation==XR_NATIVE_ALLOCATION_NO_HEAP&&!m->parameters&&!m->parameter_count&&
            m->result==XR_NATIVE_TERM_BOOL&&!parameters&&!count&&result==XR_XIR_BOOL;
    }
    const XrNativeParameter *parameter=m->parameters;
    return !strcmp(m->name,"__fullpath")&&!strcmp(m->signature,"(raw: string): string")&&
        !strcmp(m->result_text,"string")&&m->allocation==XR_NATIVE_ALLOCATION_UNKNOWN&&
        m->parameter_count==1&&parameter&&parameter->name&&!strcmp(parameter->name,"raw")&&
        parameter->type_text&&!strcmp(parameter->type_text,"string")&&parameter->type==XR_NATIVE_TERM_STRING&&
        !parameter->optional&&!parameter->variadic&&m->result==XR_NATIVE_TERM_STRING&&
        count==1&&parameters&&parameters[0]==XR_XIR_STRING&&result==XR_XIR_STRING;
}
#endif // XXIR_PATH_NATIVE_H
