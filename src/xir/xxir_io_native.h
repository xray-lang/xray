/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_io_native.h - Source-owned Path signatures and filesystem authority
 *
 * KEY CONCEPT:
 *   A canonical declaration selects its actual public String member. Ordinary
 *   import and READ permission remain independent of type-only visibility.
 */
#ifndef XXIR_IO_NATIVE_H
#define XXIR_IO_NATIVE_H
#include "xxir_path_prelude.h"
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"

static inline bool xir_io_native_id(uint32_t id) {
    return id==XR_NATIVE_CALLABLE_IO_CHMOD || id==XR_NATIVE_CALLABLE_IO_CHDIR;
}
/* This observes a declaration, not a field-read capability. */
static inline bool xir_io_path_field(const XrXirTypes *types,XrXirType type,uint32_t *output) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!output || !node || node->kind!=XR_XIR_TYPE_NOMINAL || node->parameter_span ||
        node->nominal.argument_count || !node->nominal.fields ||
        !xir_path_prelude_declaration(types->nominals,node->nominal.declaration))return false;
    const XrXirNominalTable *table=types->nominals;uint32_t declaration=node->nominal.declaration;
    uint32_t count=table->declarations?table->declarations[declaration].field_count:table->identities[declaration].field_count;
    if(count!=node->nominal.field_count)return false;
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<count;++f) {
        XrXirLiteral name=table->declarations?table->declarations[declaration].fields[f].name:
            table->identities[declaration].fields[f].name;
        if(!xir_path_prelude_literal(name,"raw"))continue;
        uint32_t flags=table->declarations?table->declarations[declaration].fields[f].flags:
            table->identities[declaration].fields[f].flags;
        if(found!=UINT32_MAX || node->nominal.fields[f]!=XR_XIR_STRING ||
            (table->declarations && table->declarations[declaration].fields[f].type!=XR_XIR_STRING) ||
            (flags&(XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED)))return false;
        found=f;
    }
    if(found==UINT32_MAX)return false;
    *output=found;return true;
}
static inline bool xir_io_native_signature(uint32_t id,const XrXirTypes *types,
    const XrXirType *parameters,uint32_t count,XrXirType result,XrXirType error) {
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    bool chmod=id==XR_NATIVE_CALLABLE_IO_CHMOD;
    if(!xir_io_native_id(id) || !native || native->owner || native->error_e_type || native->ctfe ||
        !native->namespace_id || strcmp(native->namespace_id,"io") || !native->source_path ||
        strcmp(native->source_path,"stdlib/defs/core.def") || !native->member)return false;
    const XrNativeMemberDeclaration *m=native->member;
    if(!m->name || strcmp(m->name,chmod?"__chmod":"__chdir") || !m->signature ||
        strcmp(m->signature,chmod?"(path: Path, mode: i64): bool":"(path: Path): bool") ||
        !m->result_text || strcmp(m->result_text,"bool") || m->is_public || m->is_static ||
        m->is_method || m->lowered || m->receiver!=XR_NATIVE_RECEIVER_READ ||
        m->operation!=XR_NATIVE_OPERATION_NONE || m->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN ||
        !m->failures || *m->failures || m->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        !m->parameters || m->parameter_count!=(chmod?2u:1u) || count!=m->parameter_count ||
        !parameters || result!=XR_XIR_BOOL || error!=XR_XIR_NEVER || m->result!=XR_NATIVE_TERM_BOOL)return false;
    for(uint32_t p=0;p<count;++p) {
        const XrNativeParameter *parameter=m->parameters+p;
        if(!parameter->name || strcmp(parameter->name,p?"mode":"path") || !parameter->type_text ||
            strcmp(parameter->type_text,p?"i64":"Path") || parameter->type!=(p?XR_NATIVE_TERM_I64:XR_NATIVE_TERM_PATH) ||
            parameter->optional || parameter->variadic)return false;
    }
    uint32_t field=0;
    return (!chmod || parameters[1]==XR_XIR_I64) && xir_io_path_field(types,parameters[0],&field);
}
static inline bool xir_io_native_scope(const XrXirDeclarations *scope,const XrXirTypes *types,
    uint32_t function,uint32_t count,XrXirType path,uint32_t *field) {
    static const char identity[]="stdlib-module-v1:module=2:io:path=8:io/io.xr";
    uint32_t selected=0;
    if(!field || !scope || !scope->functions || !scope->modules || function>=count ||
        !xir_io_path_field(types,path,&selected))return false;
    uint32_t caller=scope->functions[function].module;
    if(caller>=scope->module_count)return false;
    const XrXirSourceModule *module=scope->modules+caller;
    const XrXirTypeNode *node=xr_xir_type_node(types,path);
    if(!module->name || module->name_length!=sizeof(identity)-1 || module->lexical_scope ||
        memcmp(module->name,identity,sizeof(identity)-1) ||
        !xir_path_prelude_type(scope,types->nominals,node->nominal.declaration))return false;
    char logical[64],canonical[160];if(!xir_path_prelude_coordinates(logical,canonical))return false;
    for(uint32_t m=0;m<scope->module_count;++m) {
        const XrXirSourceModule *owner=scope->modules+m;
        if(owner->name && owner->name_length==strlen(canonical) &&
            !memcmp(owner->name,canonical,owner->name_length)) {
            if(!xr_xir_module_imports(scope,caller,m))return false;
            *field=selected;return true;
        }
    }
    return false;
}
#endif // XXIR_IO_NATIVE_H
