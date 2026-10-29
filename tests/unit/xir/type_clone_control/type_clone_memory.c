/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * type_clone_memory.c - Real compiler allocation attempts and frees, including failed metadata.
 */
#include "type_clone_probe.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"Type memory %d: %s\n",__LINE__,#c);exit(1);}}while(0)
typedef union TcAllocation {union {long double floating;uint64_t integer;void *pointer;} alignment;struct {size_t bytes;uintptr_t active;} record;} TcAllocation;
static size_t live,held,attempts,fail_at=SIZE_MAX;
static struct {XrCompileResources *resources;uint64_t initial;} ledgers[128];
static unsigned ledger_count;
static void *allocate(size_t bytes) {
    if(attempts++==fail_at)return NULL;if(bytes>SIZE_MAX-sizeof(TcAllocation))return NULL;
    TcAllocation *header=xr_malloc(sizeof(*header)+bytes);if(!header)return NULL;
    header->record.bytes=bytes;header->record.active=UINTPTR_MAX^(uintptr_t)header;++live;held+=bytes;return header+1;
}
static void release(void *pointer) {
    if(!pointer)return;TcAllocation *header=(TcAllocation *)pointer-1;CHECK(header->record.active==(UINTPTR_MAX^(uintptr_t)header));
    CHECK(live&&held>=header->record.bytes);--live;held-=header->record.bytes;header->record.active=0;xr_free(header);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc allocate
#define xr_free release
#define xr_compile_resources_new tc_actual_resources_new
#include "base/xcompile_resources.c"
#undef xr_compile_resources_new
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(const XrCompileResourceLimits *limits,XrCompileResources **output) {
    XrCompileResourceStatus status=tc_actual_resources_new(limits,output);
    if(status==XR_COMPILE_RESOURCE_OK) {CHECK(ledger_count<128);XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(*output,&stats)==XR_COMPILE_RESOURCE_OK);ledgers[ledger_count].resources=*output;ledgers[ledger_count++].initial=stats.held_bytes;}
    return status;
}
XR_FUNC size_t tc_memory_live(void) {return live;}
XR_FUNC size_t tc_memory_bytes(void) {return held;}
XR_FUNC size_t tc_memory_attempts(void) {return attempts;}
XR_FUNC void tc_memory_fail_next(void) {fail_at=attempts;}
XR_FUNC void tc_memory_reset_failure(void) {fail_at=SIZE_MAX;}
XR_FUNC void tc_memory_reset(void) {CHECK(!live&&!held);ledger_count=0;fail_at=SIZE_MAX;}
XR_FUNC uint64_t tc_memory_initial(const XrCompileResources *resources) {
    for(unsigned i=ledger_count;i;--i)if(ledgers[i-1].resources==resources)return ledgers[i-1].initial;
    CHECK(false);return 0;
}
