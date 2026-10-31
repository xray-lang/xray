/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_input_probe.c - Real receiving clone observation
 *
 * KEY CONCEPT:
 *   The unchanged effect implementation delegates every counted clone to the
 *   real construction receiver. Other compiler clones are not observed here.
 */
#include "xir/xxir_construction_internal.h"
#include "base/xmalloc.h"
#include "base/xchecks.h"
#include "os/os_time.h"
static uint32_t effect_input_fault;
static bool effect_input_cancelled, effect_input_clock_failure, effect_input_oom;
static uint64_t effect_input_clock_value, effect_input_live, effect_input_oom_attempts;
static void *observed_effect_malloc(size_t bytes) {
    if(effect_input_oom){effect_input_oom=false;++effect_input_oom_attempts;return NULL;}
    void *memory=xr_malloc(bytes);if(memory)++effect_input_live;return memory;
}
static void observed_effect_free(void *memory) {
    if(memory){XR_CHECK(effect_input_live,"observed provider allocation must be live");--effect_input_live;}
    xr_free(memory);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc observed_effect_malloc
#define xr_free observed_effect_free
#include "../../../src/base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
static uint64_t observed_effect_clock(void) {
    if(effect_input_clock_failure)return 0;
    return effect_input_clock_value?effect_input_clock_value:xr_time_monotonic_ns();
}
#define xr_time_monotonic_ns observed_effect_clock
#include "../../../src/base/xcompile_request.c"
#undef xr_time_monotonic_ns
static uint64_t effect_input_clone_calls;
static XrXirStatus observed_effect_input_clone(const XrXirCompileContext *context,
    const XrXirTypes *types,const XrXirConstruction *source,
    XrXirConstruction **output,XrCompileRequestControl *control) {
    ++effect_input_clone_calls;
    if(effect_input_fault==1)effect_input_cancelled=true;
    if(effect_input_fault==2){XR_CHECK(control && control->has_deadline,"deadline request is real");effect_input_clock_value=control->deadline_ns;}
    if(effect_input_fault==3)effect_input_clock_failure=true;
    if(effect_input_fault==4)effect_input_oom=true;
    return xir_construction_clone(context,types,source,output,control);
}
#define xir_construction_clone observed_effect_input_clone
#include "../../../src/xir/xxir_effects.c"
#undef xir_construction_clone
XR_FUNC uint64_t xir_test_effect_input_clone_calls(void) { return effect_input_clone_calls; }

XR_FUNC void xir_test_effect_input_arm(uint32_t fault) {
    effect_input_fault=fault;effect_input_cancelled=false;
    effect_input_clock_failure=false;effect_input_clock_value=0;effect_input_oom=false;
}
XR_FUNC bool xir_test_effect_input_cancel(const void *data) { (void)data;return effect_input_cancelled; }
XR_FUNC uint64_t xir_test_effect_input_live(void) { return effect_input_live; }
XR_FUNC uint64_t xir_test_effect_input_oom_attempts(void) { return effect_input_oom_attempts; }
