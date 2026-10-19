/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_library_pure_permissions.c - One Catalog admits pure GO and rejects Root GO
 */
#define XR_LIBRARY_COUNTER_PRODUCER_CUSTOM_MAIN
#include "test_xir_mixed_library_producer.c"

#include "xir_mixed_library_permissions.h"
static int library_permission_main(void) {
    (void)counter_producer_main;
    LibraryPermissionResult result = library_permission_operation(SIZE_MAX, library_permission_caps());
    CHECK(result.status == XR_XIR_OK && result.completed == 3 && result.root_rejected && !result.injected);
    for (unsigned round = 0; round < 3; ++round) {
        const XrCompileResourceStats *cost = &result.rounds[round];
        printf("Catalog permission ledger round=%u allocated=%llu peak=%llu work=%llu live=%llu\n", round,
            (unsigned long long)cost->allocated_bytes, (unsigned long long)cost->peak_bytes,
            (unsigned long long)cost->work, (unsigned long long)cost->live_bytes);
    }
    puts("Catalog permissions sameLibrary sameCatalog pureOK RootRejected pureOK failurePathOwned physical0/0"); return 0;
}
#ifndef XR_LIBRARY_PERMISSION_CUSTOM_MAIN
int main(void) { return library_permission_main(); }
#endif
