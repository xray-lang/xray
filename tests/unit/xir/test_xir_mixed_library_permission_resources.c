/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_library_permission_resources.c - Complete Source permission resource boundaries
 */
#define XR_LIBRARY_PERMISSION_CUSTOM_MAIN
#include "test_xir_mixed_library_pure_permissions.c"
static size_t library_permission_number(const char *text) {
    char *end = NULL; unsigned long value = strtoul(text, &end, 10);
    CHECK(text[0] && end && !*end && value < 20000); return (size_t)value;
}
static void library_permission_axes(const LibraryPermissionResult *normal) {
    const uint64_t exact[] = {normal->stats.allocated_bytes, normal->stats.peak_bytes, normal->stats.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        CHECK(exact[axis] > 1); XrCompileResourceLimits caps = library_permission_caps();
        if (!axis) caps.allocated_bytes = exact[axis]-minus;
        else if (axis == 1) caps.live_bytes = exact[axis]-minus;
        else caps.work = exact[axis]-minus;
        LibraryPermissionResult run = library_permission_operation(SIZE_MAX, caps);
        printf("Catalog permission compiler axis=%u minus=%u status=%u phase=%u stage=%u completed=%u rejected=%u allocated=%llu peak=%llu work=%llu\n",
            axis, minus, run.status, run.phase, run.stage, run.completed, (unsigned)run.root_rejected,
            (unsigned long long)run.stats.allocated_bytes, (unsigned long long)run.stats.peak_bytes, (unsigned long long)run.stats.work); fflush(stdout);
        CHECK(run.status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(run.stats.allocated_bytes <= caps.allocated_bytes && run.stats.peak_bytes <= caps.live_bytes && run.stats.work <= caps.work);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 5); (void)library_permission_main;
    LibraryPermissionResult normal = library_permission_operation(SIZE_MAX, library_permission_caps());
    printf("Catalog permission compiler status=%u phase=%u stage=%u sites=%zu completed=%u rejected=%u allocated=%llu peak=%llu work=%llu, oneFiniteOwner physical0/0\n",
        normal.status, normal.phase, normal.stage, normal.sites, normal.completed, (unsigned)normal.root_rejected,
        (unsigned long long)normal.stats.allocated_bytes, (unsigned long long)normal.stats.peak_bytes, (unsigned long long)normal.stats.work); fflush(stdout);
    CHECK(normal.status == XR_XIR_OK && normal.completed == 3 && normal.root_rejected && normal.sites && normal.sites < 20000);
    if (!strcmp(argv[1], "baseline")) { CHECK(argc == 2); return 0; }
    if (!strcmp(argv[1], "axes")) { CHECK(argc == 2); library_permission_axes(&normal); return 0; }
    CHECK(argc == 5 && !strcmp(argv[1], "fi"));
    size_t begin = library_permission_number(argv[2]), end = library_permission_number(argv[3]), expected = library_permission_number(argv[4]);
    CHECK(begin < end && end <= expected && normal.sites == expected);
    for (size_t site = begin; site < end; ++site) {
        LibraryPermissionResult run = library_permission_operation(site, library_permission_caps());
        if (run.status != XR_XIR_OUT_OF_MEMORY || !run.injected) fprintf(stderr,
            "Catalog permission FI site=%zu status=%u phase=%u stage=%u completed=%u rejected=%u hit=%u attempts=%zu\n",
            site, run.status, run.phase, run.stage, run.completed, (unsigned)run.root_rejected, (unsigned)run.injected, run.sites);
        CHECK(run.status == XR_XIR_OUT_OF_MEMORY && run.injected && run.sites > site);
    }
    printf("Catalog permission complete FI range=%zu:%zu of%zu allActualOrdinal typedOOM physical0/0\n", begin, end, expected); return 0;
}
