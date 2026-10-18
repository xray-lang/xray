/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "test_allocator.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"allocator:%d:%s\n",__LINE__,#c);exit(2);}}while(0)
typedef struct Block {void *pointer;size_t bytes;} Block;
static Block blocks[4096];
static CloneStats stats;
static size_t failure=SIZE_MAX;
static size_t slot(void *pointer) {
    for(size_t i=0;i<4096;++i)if(blocks[i].pointer==pointer)return i;
    CHECK(false);return 0;
}
static bool fault(void) {
    bool hit=stats.attempts++==failure;
    if(hit)stats.injected=true;
    return hit;
}
static void *track(void *pointer,size_t bytes) {
    if(pointer){blocks[slot(NULL)]=(Block){pointer,bytes};++stats.live;stats.bytes+=bytes;}
    return pointer;
}
XR_FUNC void *clone_test_malloc(size_t bytes) {
    if(fault())return NULL;
    return track(xr_malloc(bytes),bytes);
}
XR_FUNC void *clone_test_calloc(size_t count,size_t bytes) {
    CHECK(!count||bytes<=SIZE_MAX/count);
    if(fault())return NULL;
    return track(xr_calloc(count,bytes),count*bytes);
}
XR_FUNC void *clone_test_realloc(void *pointer,size_t bytes) {
    CHECK(bytes);
    if(fault())return NULL;
    bool had=pointer!=NULL;size_t index=had?slot(pointer):0;
    void *next=xr_realloc(pointer,bytes);if(!next)return NULL;
    if(!had)return track(next,bytes);
    stats.bytes-=blocks[index].bytes;stats.bytes+=bytes;
    blocks[index]=(Block){next,bytes};return next;
}
XR_FUNC char *clone_test_strdup(const char *text) {
    CHECK(text);if(fault())return NULL;
    return track(xr_strdup(text),strlen(text)+1);
}
XR_FUNC void clone_test_free(void *pointer) {
    if(!pointer)return;
    size_t index=slot(pointer);CHECK(stats.live&&stats.bytes>=blocks[index].bytes);
    --stats.live;stats.bytes-=blocks[index].bytes;blocks[index]=(Block){0};xr_free(pointer);
}
XR_FUNC CloneStats clone_stats(void) {return stats;}
XR_FUNC void clone_fault(size_t prefix) {stats.attempts=0;stats.injected=false;failure=prefix;}
/* Only a negative control may drain allocations that its old implementation leaked. */
XR_FUNC size_t clone_forced_cleanup(void) {
    size_t leaked=stats.live;
    for(size_t i=0;i<4096;++i)if(blocks[i].pointer)clone_test_free(blocks[i].pointer);
    return leaked;
}
