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
#include "../shared/xnative_declaration.h"
#include <string.h>
static inline bool xir_mem_native_id(uint32_t id) {
    return (id>=XR_NATIVE_CALLABLE_BUFFER_ALLOCATE && id<=XR_NATIVE_CALLABLE_BUFFER_LENGTH) ||
        (id>=XR_NATIVE_CALLABLE_BUFFER_AS_BYTES && id<=XR_NATIVE_CALLABLE_MEM_VOLATILE_STORE);
}
static inline bool xir_mem_native_term(const XrXirTypes *types,XrNativeTypeTerm term,XrXirType type) {
    switch(term) {
    case XR_NATIVE_TERM_UNIT:return type==XR_XIR_UNIT;
    case XR_NATIVE_TERM_I64:return type==XR_XIR_I64;
    case XR_NATIVE_TERM_BOOL:return type==XR_XIR_BOOL;
    case XR_NATIVE_TERM_BUFFER_STORAGE:return type==XR_XIR_BUFFER_STORAGE;
    case XR_NATIVE_TERM_SLICE_U8:case XR_NATIVE_TERM_PTR_U8:case XR_NATIVE_TERM_MUT_PTR_U8: {
        const XrXirTypeNode *node=xr_xir_type_node(types,type);
        uint32_t kind=term==XR_NATIVE_TERM_SLICE_U8?XR_XIR_TYPE_SLICE:
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
        "__volatileLoad","__volatileStore"};
    const XrNativeCallableDeclaration *canonical=xr_native_callable_by_id(id);
    if (!xir_mem_native_id(id) || !canonical || canonical->owner || canonical->error_e_type || canonical->ctfe ||
        !canonical->namespace_id || strcmp(canonical->namespace_id,"mem") || !canonical->source_path ||
        strcmp(canonical->source_path,"stdlib/defs/core.def") || !canonical->member || error!=XR_XIR_NEVER)
        return false;
    const XrNativeMemberDeclaration *member=canonical->member;
    bool borrowed=id==XR_NATIVE_CALLABLE_BUFFER_AS_BYTES || id==XR_NATIVE_CALLABLE_BUFFER_BORROW_PTR;
    bool allocating=id==XR_NATIVE_CALLABLE_BUFFER_ALLOCATE || id==XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ZEROED ||
        id==XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ALIGNED || borrowed || id==XR_NATIVE_CALLABLE_BUFFER_RESIZE ||
        id==XR_NATIVE_CALLABLE_MEM_PAGE_ALLOCATE;
    XrNativeResultOwnership ownership=borrowed?XR_NATIVE_RESULT_OWNERSHIP_BORROWED_PARAM_0:
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
