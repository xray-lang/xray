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
    /* File-handle leaves carry no Path operand except the two openers. */
    {XR_NATIVE_CALLABLE_IO_FILE_OPEN,1,1,"__fileOpen","(path: Path): i64"},
    {XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE,2,1,"__fileOpenWrite","(path: Path, append: bool): i64"},
    {XR_NATIVE_CALLABLE_IO_FILE_READ,2,0,"__fileRead","(handle: i64, maxBytes: i64): Array<u8>?"},
    {XR_NATIVE_CALLABLE_IO_FILE_WRITE,3,0,"__fileWrite","(handle: i64, data: Array<u8>, offset: i64): i64"},
    {XR_NATIVE_CALLABLE_IO_FILE_WRITE_STR,3,0,"__fileWriteStr","(handle: i64, data: string, offset: i64): i64"},
    {XR_NATIVE_CALLABLE_IO_FILE_CLOSE,1,0,"__fileClose","(handle: i64): bool"},
    {XR_NATIVE_CALLABLE_IO_FILE_FLUSH,1,0,"__fileFlush","(handle: i64): bool"}
};
static inline const XirIoNativeContract *xir_io_native_contract(uint32_t id) {
    for(uint32_t n=0;n<sizeof(xir_io_native_contracts)/sizeof(*xir_io_native_contracts);++n)
        if(xir_io_native_contracts[n].id==id)return xir_io_native_contracts+n;
    return NULL;
}
static inline bool xir_io_native_id(uint32_t id) {return xir_io_native_contract(id)!=NULL;}
static inline bool xir_io_nullable_string(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && node->kind==XR_XIR_TYPE_NULLABLE && node->element==XR_XIR_STRING;
}
static inline bool xir_io_nullable_u8_array(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    if(!node || node->kind!=XR_XIR_TYPE_NULLABLE)return false;
    const XrXirTypeNode *inner=xr_xir_type_node(types,node->element);
    return inner && inner->kind==XR_XIR_TYPE_ARRAY && inner->element==XR_XIR_U8;
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
    /* File-handle leaves form their own family: they return i64, Array<u8>?,
     * string or bool depending on the id, and only the two openers carry a
     * Path operand. Keep them separate so the filesystem shapes above stay
     * exactly as they were. */
    bool file_handle=id>=XR_NATIVE_CALLABLE_IO_FILE_OPEN && id<=XR_NATIVE_CALLABLE_IO_FILE_FLUSH;
    bool file_read=id==XR_NATIVE_CALLABLE_IO_FILE_READ;
    bool file_text=id==XR_NATIVE_CALLABLE_IO_FILE_WRITE_STR;
    bool file_returns_i64=id==XR_NATIVE_CALLABLE_IO_FILE_OPEN ||
        id==XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE || id==XR_NATIVE_CALLABLE_IO_FILE_WRITE ||
        file_text;
    const char *expected_result_text=file_handle?(file_read?"Array<u8>?":file_returns_i64?"i64":"bool"):
        stat?"__FileStat?":text?"string?":directory?"i64":"bool";
    XrNativeTypeTerm expected_term=file_handle?(file_read?XR_NATIVE_TERM_NULLABLE_ARRAY_U8:
        file_returns_i64?XR_NATIVE_TERM_I64:XR_NATIVE_TERM_BOOL):
        stat?XR_NATIVE_TERM_NULLABLE_FILE_STAT:text?XR_NATIVE_TERM_NULLABLE_STRING:
        directory?XR_NATIVE_TERM_I64:XR_NATIVE_TERM_BOOL;
    if(!m->name || strcmp(m->name,contract->name) || !m->signature ||
        strcmp(m->signature,contract->signature) ||
        !m->result_text || strcmp(m->result_text,expected_result_text) || m->is_public || m->is_static ||
        m->is_method || m->lowered || m->receiver!=XR_NATIVE_RECEIVER_READ ||
        m->operation!=XR_NATIVE_OPERATION_NONE || m->allocation!=XR_NATIVE_ALLOCATION_UNKNOWN ||
        !m->failures || *m->failures || m->ownership!=XR_NATIVE_RESULT_OWNERSHIP_OWNED ||
        !m->parameters || m->parameter_count!=contract->count || count!=m->parameter_count ||
        !parameters || m->result!=expected_term ||
        error!=XR_XIR_NEVER) return false;
    if(file_handle) {
        if(file_read?!xir_io_nullable_u8_array(types,result):result!=(file_returns_i64?XR_XIR_I64:XR_XIR_BOOL))return false;
    } else if(stat?!xir_file_stat_nullable(types,result):
        text?!xir_io_nullable_string(types,result):result!=(directory?XR_XIR_I64:XR_XIR_BOOL))return false;
    for(uint32_t p=0;p<count;++p) {
        bool path=p<contract->paths;const XrNativeParameter *parameter=m->parameters+p;
        if(file_handle) {
            /* File-handle parameter names and types are fully determined by
             * the leaf identity; the generic filesystem mapping above does not
             * describe them. */
            const char *want_name;
            const char *want_text;
            XrNativeTypeTerm want_term;
            XrXirType want_type;
            if(id==XR_NATIVE_CALLABLE_IO_FILE_OPEN) {
                want_name="path";want_text="Path";want_term=XR_NATIVE_TERM_PATH;want_type=parameters[p];
            } else if(id==XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE) {
                if(p==0) {want_name="path";want_text="Path";want_term=XR_NATIVE_TERM_PATH;}
                else {want_name="append";want_text="bool";want_term=XR_NATIVE_TERM_BOOL;}
                want_type=parameters[p];
            } else if(id==XR_NATIVE_CALLABLE_IO_FILE_READ) {
                want_name=p==0?"handle":"maxBytes";want_text="i64";want_term=XR_NATIVE_TERM_I64;want_type=XR_XIR_I64;
            } else if(id==XR_NATIVE_CALLABLE_IO_FILE_WRITE) {
                want_name=p==0?"handle":p==1?"data":"offset";
                want_text=p==1?"Array<u8>":"i64";
                want_term=p==1?XR_NATIVE_TERM_ARRAY_U8:XR_NATIVE_TERM_I64;
                want_type=parameters[p];
            } else if(id==XR_NATIVE_CALLABLE_IO_FILE_WRITE_STR) {
                want_name=p==0?"handle":p==1?"data":"offset";
                want_text=p==1?"string":"i64";
                want_term=p==1?XR_NATIVE_TERM_STRING:XR_NATIVE_TERM_I64;
                want_type=parameters[p];
            } else {
                want_name="handle";want_text="i64";want_term=XR_NATIVE_TERM_I64;want_type=XR_XIR_I64;
            }
            if(!parameter->name || strcmp(parameter->name,want_name) || !parameter->type_text ||
                strcmp(parameter->type_text,want_text) || parameter->type!=want_term ||
                parameter->optional || parameter->variadic || parameters[p]!=want_type)return false;
            if(path) {
                uint32_t field=0;
                if(!xir_io_path_field(types,parameters[p],&field))return false;
            }
            continue;
        }
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
