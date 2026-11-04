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
#include "xxir_file_stat_native.h"
#include "../shared/xnative_declaration.h"

typedef struct XirIoNativeContract {
    uint32_t id,count,paths;
    const char *name,*signature;
} XirIoNativeContract;
static const XirIoNativeContract xir_io_native_contracts[]={
    {XR_NATIVE_CALLABLE_IO_CHMOD,2,1,"__chmod","(path: Path, mode: i64): bool"},
    {XR_NATIVE_CALLABLE_IO_CHDIR,1,1,"__chdir","(path: Path): bool"},
    {XR_NATIVE_CALLABLE_IO_STAT,1,1,"__stat","(path: Path): __FileStat?"},
    {XR_NATIVE_CALLABLE_IO_MKDIR,1,1,"__mkdir","(path: Path): bool"},
    {XR_NATIVE_CALLABLE_IO_RMDIR,1,1,"__rmdir","(path: Path): bool"},
    {XR_NATIVE_CALLABLE_IO_REMOVE,1,1,"__remove","(path: Path): bool"},
    {XR_NATIVE_CALLABLE_IO_RENAME,2,2,"__rename","(old: Path, new: Path): bool"},
    {XR_NATIVE_CALLABLE_IO_UTIME_NOW,1,1,"__utimeNow","(path: Path): bool"},
    {XR_NATIVE_CALLABLE_IO_SYMLINK,2,2,"__symlink","(source: Path, link: Path): bool"},
    {XR_NATIVE_CALLABLE_IO_READLINK,1,1,"__readlink","(path: Path): string?"},
    {XR_NATIVE_CALLABLE_IO_REALPATH,1,1,"__realpath","(path: Path): string?"},
    {XR_NATIVE_CALLABLE_IO_MAKE_TEMP_DIR,1,0,"__makeTempDir","(root: string): string?"},
    {XR_NATIVE_CALLABLE_IO_MAKE_TEMP_FILE,1,0,"__makeTempFile","(root: string): string?"},
    {XR_NATIVE_CALLABLE_IO_DIR_OPEN,1,1,"__dirOpen","(path: Path): i64"},
    {XR_NATIVE_CALLABLE_IO_DIR_NEXT,1,0,"__dirNext","(handle: i64): string?"},
    {XR_NATIVE_CALLABLE_IO_DIR_CLOSE,1,0,"__dirClose","(handle: i64): bool"},
    {XR_NATIVE_CALLABLE_IO_FILE_CLOSE,1,0,"__fileClose","(handle: i64): bool"},
    {XR_NATIVE_CALLABLE_IO_FILE_FLUSH,1,0,"__fileFlush","(handle: i64): bool"},
    {XR_NATIVE_CALLABLE_IO_FILE_OPEN,1,1,"__fileOpen","(path: Path): i64"},
    {XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE,2,1,"__fileOpenWrite","(path: Path, append: bool): i64"},
    {XR_NATIVE_CALLABLE_IO_FILE_READ,2,0,"__fileRead","(handle: i64, maxBytes: i64): Array<u8>?"},
    {XR_NATIVE_CALLABLE_IO_FILE_WRITE,3,0,"__fileWrite","(handle: i64, data: Array<u8>, offset: i64): i64"},
    {XR_NATIVE_CALLABLE_IO_FILE_WRITE_STR,3,0,"__fileWriteStr","(handle: i64, data: string, offset: i64): i64"}
};
static inline const XirIoNativeContract *xir_io_native_contract(uint32_t id) {
    for(uint32_t n=0;n<sizeof(xir_io_native_contracts)/sizeof(*xir_io_native_contracts);++n)
        if(xir_io_native_contracts[n].id==id)return xir_io_native_contracts+n;
    return NULL;
}
static inline bool xir_io_native_id(uint32_t id) {return xir_io_native_contract(id)!=NULL;}
static inline bool xir_io_file_id(uint32_t id) {
    return id>=XR_NATIVE_CALLABLE_IO_FILE_CLOSE && id<=XR_NATIVE_CALLABLE_IO_FILE_WRITE_STR;
}
static inline bool xir_io_native_can_suspend(uint32_t id) {
    return xir_io_file_id(id) && id!=XR_NATIVE_CALLABLE_IO_FILE_CLOSE;
}
static inline bool xir_io_file_array(const XrXirTypes *types,XrXirType type) {
    return xr_xir_type_is_array(types,type) && xr_xir_array_element(types,type)==XR_XIR_U8;
}
static inline bool xir_io_nullable_string(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && node->kind==XR_XIR_TYPE_NULLABLE && node->element==XR_XIR_STRING;
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
static inline bool xir_io_file_signature(uint32_t id,const XrXirTypes *types,
    const XrXirType *parameters,uint32_t count,XrXirType result,XrXirType error) {
    const XirIoNativeContract *contract=xir_io_native_contract(id);
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    bool read=id==XR_NATIVE_CALLABLE_IO_FILE_READ;
    bool integer=id==XR_NATIVE_CALLABLE_IO_FILE_OPEN || id==XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE ||
        id==XR_NATIVE_CALLABLE_IO_FILE_WRITE || id==XR_NATIVE_CALLABLE_IO_FILE_WRITE_STR;
    const char *text=read?"Array<u8>?":integer?"i64":"bool";
    XrNativeTypeTerm term=read?XR_NATIVE_TERM_NULLABLE_ARRAY_U8:integer?XR_NATIVE_TERM_I64:XR_NATIVE_TERM_BOOL;
    if(!xir_io_file_id(id) || !contract || !native || native->owner || native->error_e_type || native->ctfe ||
        !native->namespace_id || strcmp(native->namespace_id,"io") || !native->source_path ||
        strcmp(native->source_path,"stdlib/defs/core.def") || !native->member || error!=XR_XIR_NEVER)return false;
    const XrNativeMemberDeclaration *m=native->member;
    if(!m->name || strcmp(m->name,contract->name) || !m->signature || strcmp(m->signature,contract->signature) ||
        !m->result_text || strcmp(m->result_text,text) || m->result!=term || m->is_public || m->is_static ||
        m->is_method || m->lowered || m->receiver!=XR_NATIVE_RECEIVER_READ ||
        m->operation!=XR_NATIVE_OPERATION_NONE || m->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN ||
        !m->failures || *m->failures || m->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        count!=contract->count || m->parameter_count!=count || !parameters || !m->parameters)return false;
    for(uint32_t p=0;p<count;++p) {
        bool path=p<contract->paths,append=id==XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE && p==1;
        bool data=p==1 && (id==XR_NATIVE_CALLABLE_IO_FILE_WRITE || id==XR_NATIVE_CALLABLE_IO_FILE_WRITE_STR);
        bool array=data && id==XR_NATIVE_CALLABLE_IO_FILE_WRITE;
        const char *name=path?"path":append?"append":data?"data":p==2?"offset":p?"maxBytes":"handle";
        const char *type=path?"Path":append?"bool":array?"Array<u8>":data?"string":"i64";
        XrNativeTypeTerm expected=path?XR_NATIVE_TERM_PATH:append?XR_NATIVE_TERM_BOOL:
            array?XR_NATIVE_TERM_ARRAY_U8:data?XR_NATIVE_TERM_STRING:XR_NATIVE_TERM_I64;
        const XrNativeParameter *parameter=m->parameters+p;uint32_t field=0;
        if(!parameter->name || strcmp(parameter->name,name) || !parameter->type_text || strcmp(parameter->type_text,type) ||
            parameter->type!=expected || parameter->optional || parameter->variadic)return false;
        if(path?!xir_io_path_field(types,parameters[p],&field):array?!xir_io_file_array(types,parameters[p]):
            parameters[p]!=(append?XR_XIR_BOOL:data?XR_XIR_STRING:XR_XIR_I64))return false;
    }
    if(!read)return result==(integer?XR_XIR_I64:XR_XIR_BOOL);
    const XrXirTypeNode *nullable=xr_xir_type_node(types,result);
    return nullable && nullable->kind==XR_XIR_TYPE_NULLABLE && !nullable->parameter_span &&
        xir_io_file_array(types,nullable->element);
}
static inline bool xir_io_native_signature(uint32_t id,const XrXirTypes *types,
    const XrXirType *parameters,uint32_t count,XrXirType result,XrXirType error) {
    if(xir_io_file_id(id))return xir_io_file_signature(id,types,parameters,count,result,error);
    const XrNativeCallableDeclaration *native=xr_native_callable_by_id(id);
    const XirIoNativeContract *contract=xir_io_native_contract(id);
    bool stat=id==XR_NATIVE_CALLABLE_IO_STAT;
    bool directory=id==XR_NATIVE_CALLABLE_IO_DIR_OPEN;
    bool token=id==XR_NATIVE_CALLABLE_IO_DIR_NEXT || id==XR_NATIVE_CALLABLE_IO_DIR_CLOSE;
    bool temporary=id==XR_NATIVE_CALLABLE_IO_MAKE_TEMP_DIR || id==XR_NATIVE_CALLABLE_IO_MAKE_TEMP_FILE;
    bool text=temporary || id==XR_NATIVE_CALLABLE_IO_READLINK || id==XR_NATIVE_CALLABLE_IO_REALPATH || id==XR_NATIVE_CALLABLE_IO_DIR_NEXT;
    if(!contract || !native || native->owner || native->error_e_type || native->ctfe ||
        !native->namespace_id || strcmp(native->namespace_id,"io") || !native->source_path ||
        strcmp(native->source_path,"stdlib/defs/core.def") || !native->member)return false;
    const XrNativeMemberDeclaration *m=native->member;
    if(!m->name || strcmp(m->name,contract->name) || !m->signature ||
        strcmp(m->signature,contract->signature) ||
        !m->result_text || strcmp(m->result_text,stat?"__FileStat?":text?"string?":directory?"i64":"bool") || m->is_public || m->is_static ||
        m->is_method || m->lowered || m->receiver!=XR_NATIVE_RECEIVER_READ ||
        m->operation!=XR_NATIVE_OPERATION_NONE || m->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN ||
        !m->failures || *m->failures || m->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        !m->parameters || m->parameter_count!=contract->count || count!=m->parameter_count ||
        !parameters || (stat?!xir_file_stat_nullable(types,result):text?!xir_io_nullable_string(types,result):result!=(directory?XR_XIR_I64:XR_XIR_BOOL)) ||
        error!=XR_XIR_NEVER || m->result!=(stat?XR_NATIVE_TERM_NULLABLE_FILE_STAT:text?XR_NATIVE_TERM_NULLABLE_STRING:directory?XR_NATIVE_TERM_I64:XR_NATIVE_TERM_BOOL))return false;
    for(uint32_t p=0;p<count;++p) {
        bool path=p<contract->paths;const XrNativeParameter *parameter=m->parameters+p;
        const char *name=temporary?"root":token?"handle":id==XR_NATIVE_CALLABLE_IO_SYMLINK?(p?"link":"source"):
            contract->paths==2?(p?"new":"old"):p?"mode":"path";
        if(!parameter->name || strcmp(parameter->name,name) || !parameter->type_text ||
            strcmp(parameter->type_text,path?"Path":temporary?"string":"i64") ||
            parameter->type!=(path?XR_NATIVE_TERM_PATH:temporary?XR_NATIVE_TERM_STRING:XR_NATIVE_TERM_I64) ||
            parameter->optional || parameter->variadic)return false;
        uint32_t field=0;
        if(path?!xir_io_path_field(types,parameters[p],&field):parameters[p]!=(temporary?XR_XIR_STRING:XR_XIR_I64))return false;
    }
    return true;
}
static inline bool xir_io_native_module_scope(const XrXirDeclarations *scope,uint32_t function,uint32_t count) {
    static const char identity[]="stdlib-module-v1:module=2:io:path=8:io/io.xr";
    if(!scope || !scope->functions || !scope->modules || function>=count)return false;
    uint32_t caller=scope->functions[function].module;
    if(caller>=scope->module_count)return false;
    const XrXirSourceModule *module=scope->modules+caller;
    return module->name && module->name_length==sizeof(identity)-1 && !module->lexical_scope &&
        !memcmp(module->name,identity,sizeof(identity)-1);
}
static inline bool xir_io_native_scope(const XrXirDeclarations *scope,const XrXirTypes *types,
    uint32_t function,uint32_t count,XrXirType path,uint32_t *field) {
    uint32_t selected=0;
    if(!field || !xir_io_native_module_scope(scope,function,count) ||
        !xir_io_path_field(types,path,&selected))return false;
    uint32_t caller=scope->functions[function].module;
    const XrXirTypeNode *node=xr_xir_type_node(types,path);
    if(!xir_path_prelude_type(scope,types->nominals,node->nominal.declaration))return false;
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
