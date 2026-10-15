/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_graph_idle.c - Measure idle operations with retained closed owners
 *
 * KEY CONCEPT:
 *   Real closed Domains remain readable until their final external root is gone.
 *   The measured interval has no heap request, graph mutation or copy hook.
 */
#define XR_COMPILE_DOMAIN_ENTRY original_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
#include <time.h>
static double graph_idle_wall(void) {
    struct timespec value;
    CHECK(timespec_get(&value, TIME_UTC) == TIME_UTC);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}
int main(void) {
    enum { owner_count = 1000, operation_count = 200000 };
    XrXirValue roots[owner_count] = {{0}};
    for (unsigned i = 0; i < owner_count; ++i) {
        XrXirDomain *domain = NULL;
        CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(domain, "held", 4, &roots[i]) == XR_XIR_VALUE_OK);
        xr_xir_domain_close(domain); xr_xir_domain_drop(domain);
    }
    size_t before_calls = calls, before_live = live;
    double start = graph_idle_wall();
    for (unsigned i = 0; i < operation_count; ++i) {
        xr_xir_value_graph_begin(); xr_xir_value_graph_end();
    }
    double seconds = graph_idle_wall() - start;
    CHECK(calls == before_calls && live == before_live);
    for (unsigned i = 0; i < owner_count; ++i) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&roots[i], &bytes, &length) && length == 4 && !memcmp(bytes, "held", 4));
    }
    before_calls = calls; fail_at = calls;
    for (unsigned i = 0; i < owner_count; ++i) xr_xir_value_drop(&roots[i]);
    fail_at = SIZE_MAX;
    CHECK(calls == before_calls && !live && !value_compile_live && !value_compile_bytes);
    printf("{\"closed_owners\":%u,\"idle_operations\":%u,\"wall_seconds\":%.9f,"
        "\"timed_heap_requests\":0,\"copy_observer\":false,\"physical_after_drop\":0}\n",
        owner_count, operation_count, seconds);
    return 0;
}
