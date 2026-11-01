/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_mem_native.h - Exact governed byte-memory callable signatures
 *
 * KEY CONCEPT:
 *   A private callable receives concrete carrier types from the actual pool.
 *   Module authority and runtime origin checks remain independent obligations.
 */
#ifndef XXIR_MEM_NATIVE_H
#define XXIR_MEM_NATIVE_H
#include "xxir_types.h"
#include "xxir_nominal.h"
#include "../shared/xnative_declaration.h"
#include <string.h>
static inline bool xir_mem_native_id(uint32_t id) {
    return (id>=XR_NATIVE_CALLABLE_BUFFER_ALLOCATE && id<=XR_NATIVE_CALLABLE_BUFFER_LENGTH) ||
        (id>=XR_NATIVE_CALLABLE_BUFFER_AS_BYTES && id<=XR_NATIVE_CALLABLE_BUFFER_SLICE_MUT_BEGIN);
}
static inline bool xir_mem_buffer_ref(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *cell=xr_xir_type_node(types,type);
    const XrXirTypeNode *node=cell && cell->kind==XR_XIR_TYPE_CELL ? xr_xir_type_node(types,cell->element):NULL;
    const XrXirNominalTable *table=types?types->nominals:NULL;
    if (!node || node->kind!=XR_XIR_TYPE_NOMINAL || node->parameter_span ||
        node->nominal.argument_count || node->nominal.field_count!=1 || !node->nominal.fields ||
        node->nominal.fields[0]!=XR_XIR_BUFFER_STORAGE || !table || node->nominal.declaration>=table->count ||
        (!table->declarations && !table->identities)) return false;
    uint32_t ordinal=node->nominal.declaration;
    const XrXirNominalDeclaration *d=table->declarations?&table->declarations[ordinal]:NULL;
    const XrXirNominalIdentity *id=d?NULL:&table->identities[ordinal];
    XrXirLiteral name=d?d->name:id->name;
    return (d?d->kind:id->kind)==XR_XIR_NOMINAL_CLASS && (d?d->flags:id->flags)==XR_XIR_NOMINAL_FINAL &&
        (d?d->exported:id->exported) && !(d?d->parameter_count:id->arity) &&
        !(d?d->native.native_id:id->native.native_id) && (d?d->field_count:id->field_count)==1 &&
        (d?d->fields!=NULL:id->fields!=NULL) && (!d || d->fields[0].type==XR_XIR_BUFFER_STORAGE) &&
        ((d?d->fields[0].flags:id->fields[0].flags)&XR_XIR_FIELD_PRIVATE) &&
        name.length==6 && name.bytes && !memcmp(name.bytes,"Buffer",6);
}
static inline bool xir_mem_buffer_ref_scope(const XrXirModule *module,uint32_t function,XrXirType type) {
    if (!xir_mem_buffer_ref(module->types,type) || !module->declarations || !module->declarations->functions ||
        !module->declarations->modules || function>=module->function_count) return false;
    uint32_t owner=module->declarations->functions[function].module;
    if (owner>=module->declarations->module_count) return false;
    const XrXirTypeNode *cell=xr_xir_type_node(module->types,type);
    const XrXirTypeNode *node=xr_xir_type_node(module->types,cell->element);
    const XrXirNominalTable *table=module->types->nominals;
    XrXirLiteral nominal=table->declarations?table->declarations[node->nominal.declaration].module:
        table->identities[node->nominal.declaration].module;
    const XrXirSourceModule *scope=&module->declarations->modules[owner];
    return nominal.bytes && scope->name && nominal.length==scope->name_length &&
        !memcmp(nominal.bytes,scope->name,nominal.length);
}
static inline bool xir_mem_native_term(const XrXirTypes *types,XrNativeTypeTerm term,XrXirType type) {
    switch(term) {
    case XR_NATIVE_TERM_UNIT:return type==XR_XIR_UNIT;
    case XR_NATIVE_TERM_I64:return type==XR_XIR_I64;
    case XR_NATIVE_TERM_BOOL:return type==XR_XIR_BOOL;
    case XR_NATIVE_TERM_BUFFER_STORAGE:return type==XR_XIR_BUFFER_STORAGE;
    case XR_NATIVE_TERM_BUFFER_REF:return xir_mem_buffer_ref(types,type);
    case XR_NATIVE_TERM_MUT_SLICE_U8:case XR_NATIVE_TERM_SLICE_U8:case XR_NATIVE_TERM_PTR_U8:case XR_NATIVE_TERM_MUT_PTR_U8: {
        const XrXirTypeNode *node=xr_xir_type_node(types,type);
        uint32_t kind=term==XR_NATIVE_TERM_SLICE_U8?XR_XIR_TYPE_SLICE:
            term==XR_NATIVE_TERM_MUT_SLICE_U8?XR_XIR_TYPE_MUT_SLICE:
            term==XR_NATIVE_TERM_PTR_U8?XR_XIR_TYPE_PTR:XR_XIR_TYPE_MUT_PTR;
        return node && node->kind==kind && node->element==XR_XIR_U8 && !node->parameter_span;
    }
    default:return false;
    }
}
static inline bool xir_mem_native_signature(uint32_t id,const XrXirTypes *types,
    const XrXirType *parameters,uint32_t count,XrXirType result,XrXirType error) {
    static const char *const names[]={"__alloc","__allocZeroed","__allocAligned","__bufferLength",
        "__bufferAsBytes","__bufferBorrowPtr","__bufferResize","__fence","__prefetch","__cacheFlush",
        "__nontemporalStore","__cacheLineSize","__pageAlloc","__pageProtect","__pageFree","__move",
        "__volatileLoad","__volatileStore","__bufferSliceMutBegin"};
    const XrNativeCallableDeclaration *canonical=xr_native_callable_by_id(id);
    if (!xir_mem_native_id(id) || !canonical || canonical->owner || canonical->error_e_type || canonical->ctfe ||
        !canonical->namespace_id || strcmp(canonical->namespace_id,"mem") || !canonical->source_path ||
        strcmp(canonical->source_path,"stdlib/defs/core.def") || !canonical->member || error!=XR_XIR_NEVER)
        return false;
    const XrNativeMemberDeclaration *member=canonical->member;
    bool borrowed=id==XR_NATIVE_CALLABLE_BUFFER_AS_BYTES || id==XR_NATIVE_CALLABLE_BUFFER_BORROW_PTR;
    bool allocating=id==XR_NATIVE_CALLABLE_BUFFER_ALLOCATE || id==XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ZEROED ||
        id==XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ALIGNED || borrowed || id==XR_NATIVE_CALLABLE_BUFFER_RESIZE ||
        id==XR_NATIVE_CALLABLE_MEM_PAGE_ALLOCATE || id==XR_NATIVE_CALLABLE_BUFFER_SLICE_MUT_BEGIN;
    XrNativeResultOwnership ownership=id==XR_NATIVE_CALLABLE_BUFFER_SLICE_MUT_BEGIN?XR_NATIVE_RESULT_OWNERSHIP_BORROWED_PARAM_2:
        borrowed?XR_NATIVE_RESULT_OWNERSHIP_BORROWED_PARAM_0:
        member->result==XR_NATIVE_TERM_UNIT?XR_NATIVE_RESULT_OWNERSHIP_UNIT:XR_NATIVE_RESULT_OWNERSHIP_OWNED;
    if (!member->name || strcmp(member->name,names[id<=XR_NATIVE_CALLABLE_BUFFER_LENGTH?id-XR_NATIVE_CALLABLE_BUFFER_ALLOCATE:
            id-XR_NATIVE_CALLABLE_BUFFER_AS_BYTES+4]) ||
        member->is_method || member->is_static || member->is_public || member->operation!=XR_NATIVE_OPERATION_NONE ||
        member->parameter_count!=count || (count && (!parameters || !member->parameters)) ||
        member->allocation!=(allocating?XR_NATIVE_ALLOCATION_MAY_HEAP:XR_NATIVE_ALLOCATION_NO_HEAP) ||
        member->ownership!=ownership || !member->failures ||
        strcmp(member->failures,allocating?"allocation,retain,limit":"none") ||
        !xir_mem_native_term(types,member->result,result)) return false;
    for (uint32_t p=0;p<count;++p)
        if (member->parameters[p].optional || member->parameters[p].variadic ||
            !xir_mem_native_term(types,member->parameters[p].type,parameters[p])) return false;
    return true;
}
#endif // XXIR_MEM_NATIVE_H
