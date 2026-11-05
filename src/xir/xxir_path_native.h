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
static inline bool xir_path_native_id(uint32_t id) {return id==XR_NATIVE_CALLABLE_PATH_GETCWD ||
    id==XR_NATIVE_CALLABLE_PATH_IS_WINDOWS || id==XR_NATIVE_CALLABLE_PATH_FULLPATH;}
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
    bool full=id==XR_NATIVE_CALLABLE_PATH_FULLPATH,flavour=id==XR_NATIVE_CALLABLE_PATH_IS_WINDOWS;
    const char *name=full?"__fullpath":flavour?"__isWindows":"__getcwd";
    const char *signature=full?"(raw: string): string":flavour?"(): bool":"(): string";
    if(!m->name || strcmp(m->name,name) || !m->signature || strcmp(m->signature,signature) ||
        !m->result_text || strcmp(m->result_text,flavour?"bool":"string") ||
        m->is_public || m->is_method || m->is_static || m->lowered ||
        m->receiver!=XR_NATIVE_RECEIVER_READ || m->operation!=XR_NATIVE_OPERATION_NONE ||
        m->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN || !m->failures || *m->failures ||
        m->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED || m->parameter_count!=(full?1u:0u) ||
        m->result!=(flavour?XR_NATIVE_TERM_BOOL:XR_NATIVE_TERM_STRING) ||
        count!=(full?1u:0u) || result!=(flavour?XR_XIR_BOOL:XR_XIR_STRING) || error!=XR_XIR_NEVER)return false;
    if(!full)return !m->parameters && !parameters;
    const XrNativeParameter *p=m->parameters;
    return p && p->name && !strcmp(p->name,"raw") && p->type_text && !strcmp(p->type_text,"string") &&
        p->type==XR_NATIVE_TERM_STRING && !p->optional && !p->variadic && parameters && parameters[0]==XR_XIR_STRING;
}
#endif // XXIR_PATH_NATIVE_H
