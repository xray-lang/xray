/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_segment_lifetime_probe.h - Separate storage lifetime from shadow state
 *
 * KEY CONCEPT:
 *   Driver poisoning is checked before physical reclamation. Physical free
 *   ends an allocation identity regardless of the allocator's later shadow.
 */
#ifndef XIR_SEGMENT_LIFETIME_PROBE_H
#define XIR_SEGMENT_LIFETIME_PROBE_H
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define XR_XIR_SEGMENT_PROBE_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define XR_XIR_SEGMENT_PROBE_ASAN 1
#endif
#if defined(XR_XIR_SEGMENT_PROBE_ASAN)
#include <sanitizer/asan_interface.h>
#endif
typedef struct SegmentLeaseProbe {
    uintptr_t address;
    uint64_t allocation_identity, poison_events;
    bool released;
} SegmentLeaseProbe;
static SegmentLeaseProbe segment_leases[64];
typedef struct SegmentAllocationProbe { uint64_t identity; bool released; } SegmentAllocationProbe;
static SegmentAllocationProbe segment_allocations[256];
static size_t segment_allocation_count;
static uint64_t segment_observed_allocations, segment_allocation_frees;
static bool segment_probe_active;
static uint64_t allocation_identity;
static uint64_t segment_lease_releases,segment_retired_resident,segment_retired_freed,segment_poison_events;
static uint64_t new_allocation_identity(void) {
    CHECK(allocation_identity!=UINT64_MAX);return ++allocation_identity;
}
static size_t segment_probe_owner(uintptr_t address) {
    for (size_t i=0;i<live;++i) if (address>=(uintptr_t)allocation_records[i].pointer &&
        address-(uintptr_t)allocation_records[i].pointer<allocation_records[i].bytes) return i;
    return live;
}
static void segment_probe_begin(void) {
    memset(segment_leases,0,sizeof(segment_leases));segment_probe_active=true;
    memset(segment_allocations,0,sizeof(segment_allocations));segment_allocation_count=0;
}
static void segment_probe_observe(uint32_t level,const void *state) {
    if (!segment_probe_active) return;
    CHECK(level<64);uintptr_t address=(uintptr_t)state;size_t owner=segment_probe_owner(address);
    CHECK(owner<live);
    uint64_t identity=allocation_records[owner].identity;
    size_t index=0;
    while (index<segment_allocation_count && segment_allocations[index].identity!=identity) ++index;
    if (index==segment_allocation_count) {
        CHECK(index<sizeof(segment_allocations)/sizeof(segment_allocations[0]));
        segment_allocations[segment_allocation_count++]=(SegmentAllocationProbe){identity,false};
        ++segment_observed_allocations;
    } else CHECK(!segment_allocations[index].released);
    segment_leases[level]=(SegmentLeaseProbe){address,allocation_records[owner].identity,0,false};
}
static void segment_probe_free(uint64_t identity) {
    if (!segment_probe_active) return;
    for (size_t i=0;i<segment_allocation_count;++i) if (segment_allocations[i].identity==identity) {
        CHECK(!segment_allocations[i].released);segment_allocations[i].released=true;++segment_allocation_frees;
    }
    for (unsigned i=0;i<64;++i) if (segment_leases[i].allocation_identity==identity) {
        CHECK(!segment_leases[i].released);
#if defined(XR_XIR_SEGMENT_PROBE_ASAN)
        CHECK(segment_leases[i].poison_events);
#endif
        segment_leases[i].released=true;++segment_lease_releases;
    }
}
static void segment_probe_retired(uint32_t level) {
    CHECK(segment_probe_active && level<64);
    const SegmentLeaseProbe *lease=&segment_leases[level];CHECK(lease->allocation_identity);
    size_t owner=segment_probe_owner(lease->address);
    bool resident=owner<live && allocation_records[owner].identity==lease->allocation_identity;
    CHECK(resident ? !lease->released : lease->released);
    if (resident) ++segment_retired_resident;else ++segment_retired_freed;
#if defined(XR_XIR_SEGMENT_PROBE_ASAN)
    CHECK(lease->poison_events);
    if (resident) CHECK(__asan_address_is_poisoned((const void *)lease->address));
#endif
}
static void segment_probe_end(void) {
    for (unsigned i=0;i<64;++i) if (segment_leases[i].allocation_identity) CHECK(segment_leases[i].released);
    for (size_t i=0;i<segment_allocation_count;++i) CHECK(segment_allocations[i].released);
    segment_probe_active=false;
}
#if defined(XR_XIR_SEGMENT_PROBE_ASAN)
static void segment_probe_poison(void const volatile *pointer,size_t size) {
    __asan_poison_memory_region(pointer,size);
    if (!segment_probe_active) return;
    uintptr_t address=(uintptr_t)pointer;
    for (unsigned i=0;i<64;++i) {
        SegmentLeaseProbe *lease=&segment_leases[i];
        if (!lease->allocation_identity || lease->released || lease->address<address || lease->address-address>=size) continue;
        size_t owner=segment_probe_owner(lease->address);
        CHECK(owner<live && allocation_records[owner].identity==lease->allocation_identity);
        CHECK(__asan_address_is_poisoned((const void *)lease->address));++lease->poison_events;++segment_poison_events;
    }
}
#define __asan_poison_memory_region(pointer,size) segment_probe_poison(pointer,size)
#endif
#endif // XIR_SEGMENT_LIFETIME_PROBE_H
