/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_read_scratch.h - Capacity for callback-free metadata read attempts
 *
 * A read attempt can claim and release existing buffers but cannot allocate.
 * Missing capacity records a byte request and unwinds the complete attempt.
 * The owner grows storage only after every borrowed descriptor has expired.
 */
#ifndef XXIR_READ_SCRATCH_H
#define XXIR_READ_SCRATCH_H
#include "xxir_compile_memory.h"
#include "../base/xchecks.h"
#include <stddef.h>

typedef struct XirReadScratchBlock {
    struct XirReadScratchBlock *next;
    size_t bytes;
    bool claimed;
    union { long double real;void *pointer;uint64_t word; } alignment;
} XirReadScratchBlock;
_Static_assert(_Alignof(XirReadScratchBlock)>=_Alignof(void *) &&
    _Alignof(XirReadScratchBlock)>=_Alignof(uint64_t),"Metadata scratch payload alignment");
typedef struct XirReadScratch {
    XrCompileResources *resources;
    XirReadScratchBlock *memory;
    size_t required;
} XirReadScratch;

static inline XrXirStatus xir_read_scratch_begin(const XrXirCompileContext *work,XirReadScratch *scratch) {
    if(!xir_compile_context_valid(work) || !scratch || scratch->resources!=work->resources)
        return XR_XIR_BAD_STRUCTURE;
    scratch->required=0;
    for(XirReadScratchBlock *block=scratch->memory;block;block=block->next) {

        if(block->claimed) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
static inline void *xir_read_scratch_alloc(const XrXirCompileContext *work,XirReadScratch *scratch,
    size_t bytes,bool clear,XrXirStatus *status) {
    if(!scratch) return clear?xir_compile_calloc(work,1,bytes,status):xir_compile_alloc(work,bytes,status);
    if(*status!=XR_XIR_OK || !bytes) return NULL;
    if(!xir_compile_context_valid(work) || scratch->resources!=work->resources) {
        *status=XR_XIR_BAD_STRUCTURE;return NULL;
    }
    for(XirReadScratchBlock *block=scratch->memory;block;block=block->next) {

        if(block->claimed || block->bytes<bytes) continue;
        void *memory=block+1;

        block->claimed=true;
        if(clear) memset(memory,0,bytes);
        return memory;
    }
    if(bytes>scratch->required) scratch->required=bytes;
    *status=XR_XIR_BUDGET;return NULL;
}
/* Logical release cannot invoke client code or fail during error cleanup.
 * The enclosing owner retains the actual allocations until no read is live. */
static inline void xir_read_scratch_release(XirReadScratch *scratch,void *memory) {
    if(!memory) return;
    if(!scratch) { xr_compile_resources_free(memory);return; }
    for(XirReadScratchBlock *block=scratch->memory;block;block=block->next) if(memory==block+1) {
        XR_CHECK(block->claimed,"Read scratch buffer released twice");block->claimed=false;return;
    }
    XR_CHECK(false,"Foreign read scratch buffer released");
}
/* No Input, closure, matcher or proof borrow may be live at this boundary. */
static inline XrXirStatus xir_read_scratch_grow(const XrXirCompileContext *work,XirReadScratch *scratch) {
    if(!xir_compile_context_valid(work) || !scratch || scratch->resources!=work->resources || !scratch->required)
        return XR_XIR_BAD_STRUCTURE;
    size_t bytes=scratch->required;
    if(bytes>SIZE_MAX-sizeof(XirReadScratchBlock)) return XR_XIR_BUDGET;
    for(XirReadScratchBlock *block=scratch->memory;block;block=block->next)
        if(block->claimed) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    XirReadScratchBlock *block=xir_compile_alloc(work,sizeof(*block)+bytes,&status);
    if(!block) return status;
    block->next=scratch->memory;block->bytes=bytes;block->claimed=false;
    scratch->memory=block;scratch->required=0;return XR_XIR_OK;
}
static inline void xir_read_scratch_free(XirReadScratch *scratch) {
    if(!scratch) return;
    XirReadScratchBlock *memory=scratch->memory;*scratch=(XirReadScratch){0};
    while(memory) { XirReadScratchBlock *next=memory->next;xr_compile_resources_free(memory);memory=next; }
}
#endif // XXIR_READ_SCRATCH_H
