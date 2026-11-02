/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_os_native.h - Governed host-name signature and module authority
 *
 * KEY CONCEPT:
 *   Each host fact belongs to the actual standard os module and returns
 *   an owned runtime String, independent of same-named lexical declarations.
 */
#ifndef XXIR_OS_NATIVE_H
#define XXIR_OS_NATIVE_H
#include "xxir_declarations.h"
#include "../shared/xnative_declaration.h"
#include <string.h>

static inline bool xir_os_native_id(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_OS_PLATFORM || id==XR_NATIVE_CALLABLE_OS_ARCH;
}
static inline bool xir_os_native_signature(uint32_t id,const XrXirType *parameters,
    uint32_t count,XrXirType result,XrXirType error) {
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if(!xir_os_native_id(id) || !native || native->owner || native->error_e_type || native->ctfe || !native->namespace_id ||
        strcmp(native->namespace_id,"os") || !native->source_path ||
        strcmp(native->source_path,"stdlib/defs/core.def") || !native->member) return false;
    const XrNativeMemberDeclaration *member=native->member;
    return member->name && !strcmp(member->name,id==XR_NATIVE_CALLABLE_OS_PLATFORM?"__platform":"__arch") && member->signature &&
        !strcmp(member->signature,"(): string") && member->result_text && !strcmp(member->result_text,"string") &&
        !member->is_public && !member->is_method && !member->is_static &&
        member->receiver==XR_NATIVE_RECEIVER_READ && member->operation==XR_NATIVE_OPERATION_NONE &&
        member->allocation==XR_NATIVE_ALLOCATION_UNKNOWN && member->failures && !*member->failures &&
        member->ownership==XR_NATIVE_RESULT_OWNERSHIP_OWNED && !member->parameters && !member->parameter_count &&
        member->result==XR_NATIVE_TERM_STRING && !parameters && !count && result==XR_XIR_STRING && error==XR_XIR_NEVER;
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
