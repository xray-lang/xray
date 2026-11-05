/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_read_scratch.h - Owned capacity for callback-free read attempts
 *
 * KEY CONCEPT:
 *   A failed capacity request unwinds every enclosing read. Preparation owns
 *   the physical callbacks; an attempt only consumes prepared storage.
 */
#ifndef XXIR_READ_SCRATCH_H
#define XXIR_READ_SCRATCH_H
#include "xxir_compile_memory.h"
#include "../base/xchecks.h"
#include "../base/xcompile_request.h"
#include <stddef.h>

typedef struct XirReadScratchBlock {
    struct XirReadScratchBlock *next;
    size_t bytes;
    bool claimed,clear,ready;
    union { long double real;void *pointer;uint64_t word; } alignment;
} XirReadScratchBlock;
_Static_assert(_Alignof(XirReadScratchBlock)>=_Alignof(void *) &&
    _Alignof(XirReadScratchBlock)>=_Alignof(uint64_t),"Metadata scratch payload alignment");
typedef struct XirReadScratch {
    XrCompileResources *resources;
    XirReadScratchBlock *memory;
    size_t required;
    XirReadScratchBlock *cursor;
    size_t ordinal,missing,active;
    bool clear,preparation_failed;
    size_t blocks,capacity,claims,clear_bytes,preparations;
} XirReadScratch;

/* The caller receives this token from allocation. Release never guesses a
 * header before an arbitrary pointer or scans an unbounded borrowed list. */
typedef struct XirReadClaim {
    XirReadScratch *owner;
    XirReadScratchBlock *block;
    void *memory;
} XirReadClaim;

static inline XrXirStatus xir_read_scratch_frame(const XrXirCompileContext *work,const XirReadScratch *scratch) {
    if(scratch->blocks>(SIZE_MAX-scratch->capacity)/sizeof(XirReadScratchBlock)) return XR_XIR_OVERFLOW;
    size_t bytes=scratch->capacity+scratch->blocks*sizeof(XirReadScratchBlock);
    return bytes>work->limits.frame_bytes?XR_XIR_BUDGET:XR_XIR_OK;
}
static inline XrXirStatus xir_read_scratch_begin(const XrXirCompileContext *work,XirReadScratch *scratch) {
    if(!xir_compile_context_valid(work) || !scratch || scratch->resources!=work->resources || scratch->active)
        return XR_XIR_BAD_STRUCTURE;
    scratch->required=0;scratch->clear=false;scratch->missing=0;scratch->preparation_failed=false;
    scratch->ordinal=0;scratch->cursor=scratch->memory;
    return xir_read_scratch_frame(work,scratch);
}
static inline void *xir_read_scratch_alloc(const XrXirCompileContext *work,XirReadScratch *scratch,
    size_t bytes,bool clear,XirReadClaim *claim,XrXirStatus *status) {
    if(*status!=XR_XIR_OK || !bytes) return NULL;
    if(!claim || claim->memory || !xir_compile_context_valid(work)) {
        *status=XR_XIR_BAD_STRUCTURE;return NULL;
    }
    if(!scratch) {
        void *memory=clear?xir_compile_calloc(work,1,bytes,status):xir_compile_alloc(work,bytes,status);
        if(memory) *claim=(XirReadClaim){.memory=memory};
        return memory;
    }
    if(scratch->resources!=work->resources || scratch->required || scratch->ordinal==SIZE_MAX) {
        *status=XR_XIR_BAD_STRUCTURE;return NULL;
    }
    /* Reusing storage on the same ledger still consumes this caller's real
     * physical frame. Failure is final, not a request to prepare more bytes. */
    *status=xir_read_scratch_frame(work,scratch);
    if(*status!=XR_XIR_OK) return NULL;
    XirReadScratchBlock *block=scratch->cursor;
    if(!block || block->bytes<bytes || (clear && (!block->clear || !block->ready))) {
        scratch->required=bytes;scratch->clear=clear;scratch->missing=scratch->ordinal;
        /* BUDGET only unwinds existing semantic kernels. The operation reads
         * its separate preparation request before interpreting this status. */
        *status=XR_XIR_BUDGET;return NULL;
    }
    if(block->claimed || scratch->active==SIZE_MAX) {
        *status=XR_XIR_OVERFLOW;return NULL;
    }
    block->claimed=true;block->clear|=clear;block->ready=false;
    scratch->cursor=block->next;++scratch->ordinal;++scratch->active;if(scratch->claims!=SIZE_MAX) ++scratch->claims;
    void *memory=block+1;*claim=(XirReadClaim){scratch,block,memory};return memory;
}
static inline void xir_read_scratch_release(XirReadScratch *scratch,void *memory,XirReadClaim *claim) {
    if(!memory) return;
    XR_CHECK(claim && claim->memory==memory && claim->owner==scratch,"Read storage needs its actual claim");
    if(!scratch) {
        XR_CHECK(!claim->block,"Independent read storage has no pool token");
        *claim=(XirReadClaim){0};xr_compile_resources_free(memory);return;
    }
    XirReadScratchBlock *block=claim->block;
    XR_CHECK(block && block->claimed && memory==block+1 && scratch->active,"Read claim belongs to live capacity");
    block->claimed=false;--scratch->active;*claim=(XirReadClaim){0};
}
static inline XrXirStatus xir_read_scratch_poll(XrCompileRequestControl *control) {
    return control?xir_compile_resource_status(xr_compile_request_control_poll(control)):XR_XIR_OK;
}
/* No current Input or descriptor borrow may be live. The exact missing
 * ordinal owns capacity, not the result of any earlier semantic query. */
static inline XrXirStatus xir_read_scratch_prepare(const XrXirCompileContext *work,
    XirReadScratch *scratch,XrCompileRequestControl *control) {
    if(!xir_compile_context_valid(work) || !scratch || scratch->resources!=work->resources ||
        !scratch->required || scratch->active) return XR_XIR_BAD_STRUCTURE;
    scratch->preparation_failed=true;
    XrXirStatus status=xir_read_scratch_poll(control);
    XirReadScratchBlock **slot=&scratch->memory;
    for(size_t i=0;status==XR_XIR_OK && i<scratch->missing;++i) {
        if(!*slot) return XR_XIR_BAD_STRUCTURE;
        slot=&(*slot)->next;
        if((i&63u)==63u) status=xir_read_scratch_poll(control);
    }
    if(status!=XR_XIR_OK) return status;
    size_t bytes=scratch->required;
    if(bytes>SIZE_MAX-sizeof(XirReadScratchBlock)) return XR_XIR_OVERFLOW;
    if(!*slot || (*slot)->bytes<bytes) {
        XirReadScratchBlock *old=*slot;
        if((!old && scratch->blocks==SIZE_MAX) || bytes>SIZE_MAX-scratch->capacity ||
            scratch->blocks>(SIZE_MAX-scratch->capacity)/sizeof(XirReadScratchBlock))
            return XR_XIR_OVERFLOW;
        size_t current=scratch->capacity+scratch->blocks*sizeof(XirReadScratchBlock);
        size_t requested=sizeof(XirReadScratchBlock)+bytes;
        /* This operation owns these temporary blocks. Replacement includes
         * the real old/new overlap, independently of any shared ledger. */
        if(requested>work->limits.frame_bytes || current>work->limits.frame_bytes-requested)
            return XR_XIR_BUDGET;
        XirReadScratchBlock *block=xir_compile_alloc(work,requested,&status);
        if(!block) return status;
        *block=(XirReadScratchBlock){.next=old?old->next:NULL,.bytes=bytes};
        *slot=block;scratch->capacity+=bytes;
        if(old) {scratch->capacity-=old->bytes;xr_compile_resources_free(old);} else ++scratch->blocks;
    }
    (*slot)->clear|=scratch->clear;
    /* Earlier clear demands are dirty after an interrupted full attempt.
     * Prepare every such slot again, or retrying would stall at its first use. */
    size_t ordinal=0;
    for(XirReadScratchBlock *block=scratch->memory;block;block=block->next,++ordinal) {
        if(block->claimed) return XR_XIR_BAD_STRUCTURE;
        if((ordinal&63u)==0) {status=xir_read_scratch_poll(control);if(status!=XR_XIR_OK) return status;}
        if(block->clear) {
            unsigned char *memory=(unsigned char *)(block+1);block->ready=false;
            for(size_t offset=0;offset<block->bytes;) {
                size_t span=block->bytes-offset;if(span>4096) span=4096;
                memset(memory+offset,0,span);offset+=span;
                scratch->clear_bytes=span>SIZE_MAX-scratch->clear_bytes?SIZE_MAX:scratch->clear_bytes+span;
                status=xir_read_scratch_poll(control);if(status!=XR_XIR_OK) return status;
            }
        }
        /* A reserve-only payload is not a zeroed payload. A later whole
         * attempt can change this ordinal's role and must prepare it first. */
        block->ready=block->clear;
    }
    if(scratch->preparations!=SIZE_MAX) ++scratch->preparations;
    scratch->preparation_failed=false;return XR_XIR_OK;
}
static inline void xir_read_scratch_free(XirReadScratch *scratch) {
    if(!scratch) return;
    XR_CHECK(!scratch->active,"Retire read storage only after every claim closes");
    XirReadScratchBlock *memory=scratch->memory;*scratch=(XirReadScratch){0};
    while(memory) {XirReadScratchBlock *next=memory->next;xr_compile_resources_free(memory);memory=next;}
}
#endif // XXIR_READ_SCRATCH_H
