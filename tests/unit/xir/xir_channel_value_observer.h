/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_value_observer.h - Observe canonical compiler and value physical owners
 */
#ifndef XIR_CHANNEL_VALUE_OBSERVER_H
#define XIR_CHANNEL_VALUE_OBSERVER_H
#include <stdatomic.h>
static _Atomic(bool) *channel_cas_target;
static size_t channel_cas_count, channel_cas_pause_at;
static void (*channel_cas_before)(void);
static _Atomic(uint32_t) *channel_cas32_target;
static size_t channel_cas32_count,channel_cas32_pause_at;
static void (*channel_cas32_before)(void);
static bool channel_observed_cas_bool(_Atomic(bool) *cell, bool *expected, bool desired,
    memory_order success, memory_order failure) {
    CHECK(failure!=memory_order_release && failure!=memory_order_acq_rel);
    if (cell==channel_cas_target && ++channel_cas_count==channel_cas_pause_at && channel_cas_before) {
        void (*callback)(void)=channel_cas_before; channel_cas_before=NULL; callback();
    }
    return atomic_compare_exchange_strong_explicit(cell,expected,desired,success,failure);
}
static bool channel_observed_cas32(_Atomic(uint32_t) *cell, uint32_t *expected, uint32_t desired,
    memory_order success, memory_order failure) {
    CHECK(failure!=memory_order_release && failure!=memory_order_acq_rel);
    if (cell==channel_cas32_target && ++channel_cas32_count==channel_cas32_pause_at && channel_cas32_before) {
        void (*callback)(void)=channel_cas32_before;channel_cas32_before=NULL;callback();
    }
    return atomic_compare_exchange_strong_explicit(cell,expected,desired,success,failure);
}
static bool channel_observed_cas64(_Atomic(uint64_t) *cell, uint64_t *expected, uint64_t desired,
    memory_order success, memory_order failure) {
    CHECK(failure!=memory_order_release && failure!=memory_order_acq_rel);
    return atomic_compare_exchange_strong_explicit(cell,expected,desired,success,failure);
}
typedef struct ChannelBlock { void *pointer; size_t bytes; } ChannelBlock;
typedef struct ChannelObserver {
    ChannelBlock blocks[2048];
    size_t attempts, fail_at, live, bytes;
} ChannelObserver;
static ChannelObserver compiler_memory = {.fail_at=SIZE_MAX};
static ChannelObserver runtime_memory = {.fail_at=SIZE_MAX};
static size_t observed_slot(ChannelObserver *observer, void *pointer) {
    size_t slot=0; while (slot<2048 && observer->blocks[slot].pointer!=pointer) ++slot;
    CHECK(slot<2048); return slot;
}
static void *observed_allocate(ChannelObserver *observer, size_t bytes) {
    if (observer->attempts++==observer->fail_at) return NULL;
    void *pointer=xr_malloc(bytes); CHECK(pointer);
    size_t slot=observed_slot(observer,NULL); observer->blocks[slot]=(ChannelBlock){pointer,bytes};
    ++observer->live; observer->bytes+=bytes; return pointer;
}
static void observed_free(ChannelObserver *observer, void *pointer) {
    if (!pointer) return;
    size_t slot=observed_slot(observer,pointer); CHECK(observer->live);
    --observer->live; observer->bytes-=observer->blocks[slot].bytes;
    observer->blocks[slot]=(ChannelBlock){0}; xr_free(pointer);
}
static void *channel_compiler_malloc(size_t bytes) { return observed_allocate(&compiler_memory,bytes); }
static void channel_compiler_free(void *pointer) { observed_free(&compiler_memory,pointer); }
static void *channel_runtime_malloc(size_t bytes) { return observed_allocate(&runtime_memory,bytes); }
static void channel_runtime_free(void *pointer) { observed_free(&runtime_memory,pointer); }
static void *channel_runtime_calloc(size_t count, size_t bytes) {
    CHECK(!count || bytes<=SIZE_MAX/count);
    void *pointer=channel_runtime_malloc(count*bytes);
    if (pointer) memset(pointer,0,count*bytes);
    return pointer;
}
static void *channel_runtime_realloc(void *pointer, size_t bytes) {
    if (!pointer) return channel_runtime_malloc(bytes);
    CHECK(bytes); size_t slot=observed_slot(&runtime_memory,pointer);
    if (runtime_memory.attempts++==runtime_memory.fail_at) return NULL;
    void *replacement=xr_realloc(pointer,bytes); CHECK(replacement);
    runtime_memory.bytes-=runtime_memory.blocks[slot].bytes; runtime_memory.bytes+=bytes;
    runtime_memory.blocks[slot]=(ChannelBlock){replacement,bytes}; return replacement;
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_realloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc channel_compiler_malloc
#define xr_free channel_compiler_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
#undef xr_calloc
#undef xr_realloc
#define xr_malloc channel_runtime_malloc
#define xr_calloc channel_runtime_calloc
#define xr_realloc channel_runtime_realloc
#define xr_free channel_runtime_free
#pragma push_macro("atomic_compare_exchange_strong_explicit")
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit(cell, expected, desired, success, failure) \
    _Generic((cell), _Atomic(bool) *: channel_observed_cas_bool, _Atomic(uint32_t) *: channel_observed_cas32, \
        _Atomic(uint64_t) *: channel_observed_cas64)(cell, expected, desired, success, failure)
#include "xir/xxir_value.c"
#pragma pop_macro("atomic_compare_exchange_strong_explicit")
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_realloc")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")

#endif
