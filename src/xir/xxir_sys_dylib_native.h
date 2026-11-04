/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_sys_dylib_native.h - Governed loader signatures and canonical Path reads
 */
#ifndef XXIR_SYS_DYLIB_NATIVE_H
#define XXIR_SYS_DYLIB_NATIVE_H
#include "xxir_io_native.h"
#include "xxir_pointer.h"
static inline bool xir_sys_dylib_id(uint32_t id) {
    return id>=XR_NATIVE_CALLABLE_SYS_DYLIB_OPEN&&id<=XR_NATIVE_CALLABLE_SYS_DYLIB_LAST_ERROR;
}
static inline bool xir_sys_dylib_pointer(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!node||node->kind!=XR_XIR_TYPE_NULLABLE||node->parameter_span)return false;
    const XrXirTypeNode *pointer=xr_xir_type_node(types,node->element);
    return pointer&&pointer->kind==XR_XIR_TYPE_PTR&&pointer->element==XR_XIR_U8&&!pointer->parameter_span;
}
static inline bool xir_sys_dylib_signature(uint32_t id,const XrXirTypes *types,
    const XrXirType *parameters,uint32_t count,XrXirType result,XrXirType error) {
    if(!xir_sys_dylib_id(id)||!types||error!=XR_XIR_NEVER)return false;
    static const char *names[]={"__dylibOpen","__dylibSymbol","__dylibClose","__dylibLastError"};
    static const char *signatures[]={"(path: Path): i64","(handle: i64, name: string): Ptr<u8>?","(handle: i64): bool","(): string"};
    static const char *results[]={"i64","Ptr<u8>?","bool","string"};
    static const XrNativeTypeTerm terms[]={XR_NATIVE_TERM_I64,XR_NATIVE_TERM_NULLABLE_PTR_U8,XR_NATIVE_TERM_BOOL,XR_NATIVE_TERM_STRING};
    uint32_t at=id-XR_NATIVE_CALLABLE_SYS_DYLIB_OPEN,wanted=at==3?0:at==1?2:1;
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    if(!native||native->owner||native->error_e_type||native->ctfe||!native->namespace_id||strcmp(native->namespace_id,"sys")||
        !native->source_path||strcmp(native->source_path,"stdlib/defs/core.def")||!native->member)return false;
    const XrNativeMemberDeclaration *member=native->member;
    if(!member->name||strcmp(member->name,names[at])||!member->signature||strcmp(member->signature,signatures[at])||
        !member->result_text||strcmp(member->result_text,results[at])||member->result!=terms[at]||
        member->is_public||member->is_method||member->is_static||member->lowered||member->receiver!=XR_NATIVE_RECEIVER_READ||
        member->operation!=XR_NATIVE_OPERATION_NONE||member->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN||
        !member->failures||*member->failures||member->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED||
        member->parameter_count!=wanted||count!=wanted||(wanted?(!parameters||!member->parameters):(parameters||member->parameters)))return false;
    for(uint32_t p=0;p<wanted;++p) {
        const XrNativeParameter *parameter=member->parameters+p;
        const char *name=at==0?"path":p?"name":"handle";
        const char *text=at==0?"Path":p?"string":"i64";
        XrNativeTypeTerm term=at==0?XR_NATIVE_TERM_PATH:p?XR_NATIVE_TERM_STRING:XR_NATIVE_TERM_I64;
        uint32_t field=0;
        if(!parameter->name||strcmp(parameter->name,name)||!parameter->type_text||strcmp(parameter->type_text,text)||
            parameter->type!=term||parameter->optional||parameter->variadic||
            (at==0?!xir_io_path_field(types,parameters[p],&field):parameters[p]!=(p?XR_XIR_STRING:XR_XIR_I64)))return false;
    }
    return at==1?xir_sys_dylib_pointer(types,result):result==(at==0?XR_XIR_I64:at==2?XR_XIR_BOOL:XR_XIR_STRING);
}
static inline bool xir_sys_dylib_path_scope(const XrXirDeclarations *scope,const XrXirTypes *types,
    uint32_t function,uint32_t count,XrXirType path,uint32_t *field) {
    static const char identity[]="stdlib-module-v1:module=3:sys:path=10:sys/sys.xr";
    uint32_t selected=0;
    if(!scope||!scope->functions||!scope->modules||!field||function>=count||!xir_io_path_field(types,path,&selected))return false;
    uint32_t caller=scope->functions[function].module;
    if(caller>=scope->module_count)return false;
    const XrXirSourceModule *module=scope->modules+caller;
    const XrXirTypeNode *node=xr_xir_type_node(types,path);
    if(!module->name||module->name_length!=sizeof(identity)-1||module->lexical_scope||memcmp(module->name,identity,sizeof(identity)-1)||
        !xir_path_prelude_type(scope,types->nominals,node->nominal.declaration))return false;
    char logical[64],canonical[160];if(!xir_path_prelude_coordinates(logical,canonical))return false;
    for(uint32_t m=0;m<scope->module_count;++m) {
        const XrXirSourceModule *owner=scope->modules+m;
        if(owner->name&&owner->name_length==strlen(canonical)&&!memcmp(owner->name,canonical,owner->name_length)) {
            if(!xr_xir_module_imports(scope,caller,m))return false;
            *field=selected;return true;
        }
    }
    return false;
}
#endif // XXIR_SYS_DYLIB_NATIVE_H
