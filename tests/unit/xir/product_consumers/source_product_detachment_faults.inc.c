/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_detachment_faults.inc.c - Exact private input replay and faults
 */
typedef enum FixtureReplay { REPLAY_LOAD, REPLAY_WRITE, REPLAY_REMOVE } FixtureReplay;

/* Test fixture preparation has its own finite ledger and releases every block
 * before the observed two-producer operation starts. It never writes originals.
 * Physical source deletion inside that operation uses its sole compiler ledger. */
static void fixture_replay(DetachmentCase *test, FixtureReplay mode) {
    CHECK(private_root(test->root) && instance_compile_fail_at == SIZE_MAX);
    instance_compile_zero();
    XrCompileResources *resources = NULL;
    XrCompileResourceLimits budget = limits();
    CHECK(xr_compile_resources_new(&budget, &resources) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    uint8_t *bytes = NULL;
    size_t length = 0;
    if (mode != REPLAY_LOAD) {
        XrOsIoStatus removed = xr_os_io_remove(&policy, test->file);
        CHECK(removed == XR_OS_IO_OK || removed == XR_OS_IO_NOT_FOUND);
    }
    if (mode == REPLAY_WRITE) {
        CHECK(test->source_length && test->source_length <= sizeof(test->source));
        CHECK(xr_os_io_write_new_file_sync(&policy, test->file, test->source, test->source_length) == XR_OS_IO_OK);
    }
    if (mode != REPLAY_REMOVE) {
        CHECK(xr_os_io_read_regular_file(&policy, test->file, sizeof(test->source), &bytes, &length) == XR_OS_IO_OK);
        CHECK(length && length <= sizeof(test->source));
        CHECK(xr_compile_resources_work(resources, length) == XR_COMPILE_RESOURCE_OK);
        if (mode == REPLAY_LOAD) {
            memcpy(test->source, bytes, length);
            test->source_length = length;
        } else {
            CHECK(length == test->source_length && !memcmp(bytes, test->source, length));
        }
        policy.free(policy.context, bytes);
    } else {
        CHECK(xr_file_probe_owned(&policy, test->file, false) == XR_OS_IO_NOT_FOUND);
    }
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == instance_compile_bytes);
    xr_compile_resources_release(resources);
    instance_compile_zero();
}

static size_t ordinal(const char *text) {
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    CHECK(text[0] >= '0' && text[0] <= '9' && end && !*end && value <= SIZE_MAX);
    return (size_t)value;
}

static void fault(DetachmentCase *test, size_t index) {
    fixture_replay(test, REPLAY_WRITE);
    DetachmentRun result = run(test, index, limits());
    printf("detachment-fault ordinal=%zu status=%u injected=%u attempts=%zu physical=%zu/%zu\n",
        index, (unsigned)result.status, (unsigned)result.injected, result.attempts,
        instance_compile_live, instance_compile_bytes);
    CHECK(result.status == XR_XIR_OUT_OF_MEMORY && result.injected && result.attempts > index);
    printf("detachment ordinal=%zu physical=0/0\n", index);
}

static void axes(DetachmentCase *test, const DetachmentRun *baseline) {
    const uint64_t values[] = {baseline->stats.allocated_bytes, baseline->stats.peak_bytes, baseline->stats.work};
    for (unsigned axis = 0; axis < 3; ++axis) {
        CHECK(values[axis]);
        for (unsigned below = 0; below < 2; ++below) {
            fixture_replay(test, REPLAY_WRITE);
            XrCompileResourceLimits budget = limits();
            uint64_t *threshold = axis == 0 ? &budget.allocated_bytes : axis == 1 ? &budget.live_bytes : &budget.work;
            *threshold = values[axis] - below;
            DetachmentRun result = run(test, SIZE_MAX, budget);
            CHECK(result.status == (below ? XR_XIR_BUDGET : XR_XIR_OK) && !result.injected);
            printf("detachment axis=%u below=%u limit=%llu status=%u physical=0/0\n",
                axis, below, (unsigned long long)*threshold, result.status);
        }
    }
}

static void exercise(DetachmentCase *test, const DetachmentRun *baseline, int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[4], "--compiler-baseline")) return;
    if (argc == 5 && !strcmp(argv[4], "--axes")) {
        axes(test, baseline);
        return;
    }
    if (argc == 6 && !strcmp(argv[4], "--compiler-site")) {
        size_t index = ordinal(argv[5]);
        CHECK(index < baseline->attempts);
        fault(test, index);
        return;
    }
    CHECK(argc == 7 && !strcmp(argv[4], "--compiler-shard"));
    size_t shard = ordinal(argv[5]), jobs = ordinal(argv[6]), covered = 0;
    CHECK(jobs > 0 && jobs <= 8 && shard < jobs);
    for (size_t index = shard; index < baseline->attempts; index += jobs) {
        fault(test, index);
        ++covered;
    }
    printf("detachment-summary case=%s sites=%zu shard=%zu shards=%zu covered=%zu physical=0/0\n",
        test->name, baseline->attempts, shard, jobs, covered);
}
