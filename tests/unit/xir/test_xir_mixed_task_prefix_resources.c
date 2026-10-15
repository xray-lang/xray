/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_prefix_resources.c - Finite budgets cover every actual Task cancellation prefix
 */
#define XR_TASK_CANCEL_PREFIX_RESOURCE_MAIN
#include "test_xir_mixed_task_cancel_prefix.c"

static size_t task_prefix_resource_axes(unsigned backend, unsigned prefix, unsigned mode) {
    TaskPrefixResult normal = task_prefix_operation(backend, prefix, mode, SIZE_MAX, NULL);
    CHECK(normal.complete && normal.polls == prefix && normal.sites && normal.sites < 20000);
    const uint64_t axes[] = {normal.cost.requested_bytes, normal.cost.requested_call_bytes, normal.cost.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY && axes[axis]);
        if (!axis) config.requested_value_limit = axes[axis]-minus;
        else if (axis == 1) config.requested_call_limit = axes[axis]-minus;
        else config.work_limit = axes[axis]-minus;
        TaskPrefixResult result = task_prefix_operation(backend, prefix, mode, SIZE_MAX, &config);
        if (result.complete != !minus) fprintf(stderr,
            "Task prefix axis backend=%u prefix=%u mode=%u axis=%u minus=%u complete=%u normal=%llu/%llu/%llu actual=%llu/%llu/%llu\n",
            backend, prefix, mode, axis, minus, result.complete,
            (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2],
            (unsigned long long)result.cost.requested_bytes, (unsigned long long)result.cost.requested_call_bytes,
            (unsigned long long)result.cost.work);
        CHECK(result.complete == !minus);
        if (!minus) CHECK(result.phase == normal.phase && result.captured == normal.captured && result.terminal == normal.terminal);
        printf("Task prefix resource axis backend=%u prefix=%u mode=%u axis=%u minus=%u complete=%u value=%llu call=%llu work=%llu\n",
            backend, prefix, mode, axis, minus, (unsigned)result.complete,
            (unsigned long long)result.cost.requested_bytes, (unsigned long long)result.cost.requested_call_bytes,
            (unsigned long long)result.cost.work);
    }
    printf("Task prefix resources backend=%u prefix=%u mode=%u phase=%u captured=%u terminal=%u sites=%zu value=%llu call=%llu work=%llu sameOriginalDomain exact/minus1 independentStickyOracle codeLease1 physical0/0\n",
        backend, prefix, mode, normal.phase, normal.captured, normal.terminal, normal.sites,
        (unsigned long long)axes[0], (unsigned long long)axes[1], (unsigned long long)axes[2]);
    return normal.sites;
}
static void task_prefix_resource_fi(unsigned backend, unsigned mode, unsigned prefix, size_t expected) {
    size_t sites = task_prefix_resource_axes(backend, prefix, mode); CHECK(sites == expected);
    for (size_t site = 0; site < sites; ++site) {
        TaskPrefixResult result = task_prefix_operation(backend, prefix, mode, site, NULL);
        CHECK(!result.complete && result.sites > site);
    }
    printf("Task prefix complete runtime FI backend=%u prefix=%u mode=%u sites=%zu everyOrdinalHit typedOOM sameOriginalDomain codeLease1 physical0/0\n",
        backend, prefix, mode, sites);
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 6); (void)task_late_graph; (void)task_result_axes; (void)task_pending_resource_axes;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4);
    unsigned mode = (unsigned)task_pending_resource_number(argv[argc == 3 ? 2 : 3]); CHECK(mode >= 1 && mode <= 3);
    TaskPrefixResult baseline = task_prefix_operation(backend, 0, 0, SIZE_MAX, NULL);
    CHECK(baseline.complete && baseline.polls == 23);
    if (argc == 6) {
        CHECK(!strcmp(argv[2], "fi"));
        unsigned prefix = (unsigned)task_pending_resource_number(argv[4]); CHECK(prefix < baseline.polls);
        task_prefix_resource_fi(backend, mode, prefix, task_pending_resource_number(argv[5])); return 0;
    }
    size_t sites = 0;
    for (unsigned prefix = 0; prefix < baseline.polls; ++prefix) sites += task_prefix_resource_axes(backend, prefix, mode);
    printf("Task prefix resource census backend=%u mode=%u actualPrefixes=%u normalAllocationSum=%zu exactMinus1=%u physical0/0\n",
        backend, mode, baseline.polls, sites, baseline.polls*6);
    return 0;
}
