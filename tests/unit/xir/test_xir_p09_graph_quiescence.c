/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_graph_quiescence.c - Preserve deferred Domain retirement
 *
 * KEY CONCEPT:
 *   Pending close remains live through nested operations and late external roots.
 *   Retirement is allocation-free and the last owner physically releases storage.
 */
#define XR_COMPILE_DOMAIN_ENTRY original_compile_domain_main
#include "test_xir_compile_domain.c"
#undef XR_COMPILE_DOMAIN_ENTRY
static void quiescence_empty_domains(void) {
    XrXirDomain *domains[16] = {0};
    for (unsigned i = 0; i < 16; ++i) CHECK(xr_xir_domain_new(65536, &domains[i]) == XR_XIR_VALUE_OK);
    size_t before_live = live, before_calls = calls;
    xr_xir_value_graph_begin(); xr_xir_value_graph_begin();
    for (unsigned i = 0; i < 16; ++i) { xr_xir_domain_close(domains[i]); xr_xir_domain_drop(domains[i]); }
    CHECK(live == before_live && calls == before_calls);
    xr_xir_value_graph_end(); CHECK(live == before_live && calls == before_calls);
    fail_at = calls; xr_xir_value_graph_end(); fail_at = SIZE_MAX;
    CHECK(!live && calls == before_calls);
}
static void quiescence_external_roots(void) {
    XrXirDomain *domains[2] = {0}; XrXirValue roots[2] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_domain_new(65536, &domains[i]) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_new(domains[i], "live\0\xce\xa9", 7, &roots[i]) == XR_XIR_VALUE_OK);
        xr_xir_domain_close(domains[i]); xr_xir_domain_drop(domains[i]);
    }
    for (unsigned repetition = 0; repetition < 100; ++repetition) {
        for (unsigned i = 0; i < 2; ++i) {
            XrXirValue copy = {0};
            CHECK(xr_xir_value_copy(&roots[i], &copy) == XR_XIR_VALUE_OK);
            const char *bytes = NULL; size_t length = 0;
            CHECK(xr_xir_string_view(&copy, &bytes, &length) && length == 7 && !memcmp(bytes, "live\0\xce\xa9", 7));
            xr_xir_value_drop(&copy);
        }
    }
    size_t before_calls = calls;
    xr_xir_value_graph_begin();
    xr_xir_value_drop(&roots[0]); CHECK(live);
    fail_at = calls; xr_xir_value_graph_end(); fail_at = SIZE_MAX;
    CHECK(live && calls == before_calls);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&roots[1], &bytes, &length) && length == 7);
    before_calls = calls; fail_at = calls; xr_xir_value_drop(&roots[1]); fail_at = SIZE_MAX;
    CHECK(!live && calls == before_calls);
}
int main(void) {
    quiescence_empty_domains(); quiescence_external_roots();
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("Nested operation close, idle closed owners, dirty last roots and no-allocation physical0 PASS");
    return 0;
}
