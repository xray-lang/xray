/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_domain_owned.c - Complete managed-value consumer before FI
 */
#define XR_COMPILE_DOMAIN_ENTRY original_compile_domain_main
#include "test_xir_compile_domain.c"
int main(void) {
    size_t sites = 0; XrCompileResourceStats baseline = compile_domain_baseline(&sites);
    CHECK(sites > 2 && !calls); compile_domain_late_owner();
    printf("compile Domain owned baseline sites=%zu allocated=%llu peak=%llu work=%llu physical=0/0\n",
        sites, (unsigned long long)baseline.allocated_bytes, (unsigned long long)baseline.peak_bytes,
        (unsigned long long)baseline.work);
    return 0;
}
