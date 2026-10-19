/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_array_query_resources.c - Full compiler resource boundaries for query isolation
 */
#define XR_ARRAY_QUERY_CUSTOM_MAIN
#include "test_xir_mixed_array_query_types.c"
static size_t array_query_number(const char *text) {
    char *end = NULL; unsigned long number = strtoul(text, &end, 10);
    CHECK(text[0] && end && !*end && number < 20000); return (size_t)number;
}
static void array_query_compile_axes(const ArrayQueryCompileResult *normal) {
    const uint64_t exact[] = {normal->stats.allocated_bytes, normal->stats.peak_bytes, normal->stats.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        CHECK(exact[axis] > 1); XrCompileResourceLimits caps = array_query_compile_caps();
        if (!axis) caps.allocated_bytes = exact[axis]-minus;
        else if (axis == 1) caps.live_bytes = exact[axis]-minus;
        else caps.work = exact[axis]-minus;
        ArrayQueryCompileResult run = array_query_compile_operation(SIZE_MAX, caps, false);
        printf("Array query compiler axis=%u minus=%u status=%u case=%u stage=%u completed=%u allocated=%llu peak=%llu work=%llu\n",
            axis, minus, run.status, run.case_index, run.stage, run.completed, (unsigned long long)run.stats.allocated_bytes,
            (unsigned long long)run.stats.peak_bytes, (unsigned long long)run.stats.work); fflush(stdout);
        CHECK(run.status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(run.stats.allocated_bytes <= caps.allocated_bytes && run.stats.peak_bytes <= caps.live_bytes && run.stats.work <= caps.work);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 5); (void)array_query_main;
    ArrayQueryCompileResult normal = array_query_compile_operation(SIZE_MAX, array_query_compile_caps(), false);
    printf("Array query compiler status=%u case=%u stage=%u completed=%u sites=%zu allocated=%llu peak=%llu work=%llu, oneFiniteOwner sixOriginalTransactions physical0/0\n",
        normal.status, normal.case_index, normal.stage, normal.completed, normal.sites,
        (unsigned long long)normal.stats.allocated_bytes, (unsigned long long)normal.stats.peak_bytes, (unsigned long long)normal.stats.work); fflush(stdout);
    CHECK(normal.status == XR_XIR_OK && normal.completed == 6 && normal.sites && normal.sites < 20000 && !normal.injected);
    if (!strcmp(argv[1], "baseline")) { CHECK(argc == 2); return 0; }
    if (!strcmp(argv[1], "axes")) { CHECK(argc == 2); array_query_compile_axes(&normal); return 0; }
    CHECK(argc == 5 && !strcmp(argv[1], "fi"));
    size_t begin = array_query_number(argv[2]), end = array_query_number(argv[3]), expected = array_query_number(argv[4]);
    CHECK(begin < end && end <= expected && normal.sites == expected);
    for (size_t site = begin; site < end; ++site) {
        ArrayQueryCompileResult run = array_query_compile_operation(site, array_query_compile_caps(), false);
        if (run.status != XR_XIR_OUT_OF_MEMORY || !run.injected) fprintf(stderr,
            "Array query compiler FI site=%zu status=%u case=%u stage=%u completed=%u hit=%u attempts=%zu\n",
            site, run.status, run.case_index, run.stage, run.completed, (unsigned)run.injected, run.sites);
        CHECK(run.status == XR_XIR_OUT_OF_MEMORY && run.injected && run.sites > site);
    }
    printf("Array query compiler complete FI range=%zu:%zu of%zu everyOriginalOrdinal typedOOM physical0/0\n", begin, end, expected); return 0;
}
