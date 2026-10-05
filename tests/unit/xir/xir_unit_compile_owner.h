/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_unit_compile_owner.h - Finite compiler operations with physical release
 */
#ifndef XIR_UNIT_COMPILE_OWNER_H
#define XIR_UNIT_COMPILE_OWNER_H
#include "xir_effect_execution_owner.h"
typedef struct UnitCompileOwner {
 XrXirCompileContext context;
 XrCompileResourceStats baseline;
 size_t physical_blocks,physical_bytes;
} UnitCompileOwner;
static inline void unit_compile_owner_new(UnitCompileOwner *owner) {
 *owner=(UnitCompileOwner){0};owner->physical_blocks=effects_compile_live;owner->physical_bytes=effects_compile_bytes;
 const XrCompileResourceLimits caps={UINT64_C(32)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(64000000)};
 CHECK(xr_compile_resources_new(&caps,&owner->context.resources)==XR_COMPILE_RESOURCE_OK);
 owner->context.limits=xr_xir_compile_default_limits();
 CHECK(xr_compile_resources_stats(owner->context.resources,&owner->baseline)==XR_COMPILE_RESOURCE_OK);
}
static inline void unit_compile_owner_report(const UnitCompileOwner *owner,const char *label) {
 XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
 fprintf(stderr,"%s compiler: allocations=%llu allocated=%llu peak=%llu work=%llu live=%llu\n",label,
  (unsigned long long)stats.allocation_count,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,
  (unsigned long long)stats.work,(unsigned long long)stats.live_bytes);
}
static inline void unit_compile_owner_free(UnitCompileOwner *owner) {
 XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
 CHECK(stats.live_bytes==owner->baseline.live_bytes);xr_compile_resources_release(owner->context.resources);
 CHECK(effects_compile_live==owner->physical_blocks&&effects_compile_bytes==owner->physical_bytes);
 *owner=(UnitCompileOwner){0};
}
#endif // XIR_UNIT_COMPILE_OWNER_H
