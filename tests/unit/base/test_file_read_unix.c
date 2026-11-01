/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_file_read_unix.c - Physical-root, bounded input and failure ownership
 */
#include "base/xmalloc.h"
#include "base/xcompile_resources.h"
#include "os/os_file_read.h"
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct Allocation { void *memory; size_t bytes; } Allocation;
typedef struct Probe {
    Allocation blocks[8];
    size_t attempts, fail_at, live;
    XrOsIoStatus failure;
} Probe;

static XrOsIoStatus allocate(void *context, size_t bytes, void **output) {
    Probe *probe = context;
    if (++probe->attempts == probe->fail_at) return probe->failure;
    void *memory = xr_malloc(bytes);
    if (!memory) return XR_OS_IO_OUT_OF_MEMORY;
    for (size_t n = 0; n < 8; ++n) {
        if (!probe->blocks[n].memory) {
            probe->blocks[n] = (Allocation){memory, bytes};
            probe->live += bytes;
            *output = memory;
            return XR_OS_IO_OK;
        }
    }
    REQUIRE(false);
    return XR_OS_IO_IO;
}

static void release(void *context, void *memory) {
    Probe *probe = context;
    for (size_t n = 0; n < 8; ++n) {
        if (probe->blocks[n].memory == memory) {
            probe->live -= probe->blocks[n].bytes;
            probe->blocks[n] = (Allocation){0};
            xr_free(memory);
            return;
        }
    }
    REQUIRE(false);
}

static void check(const XrOsIoPolicy *policy, const char *root, const char *path,
    size_t limit, XrFileReadStatus expected, size_t bytes) {
    char canary;
    XrFileBytes output = {&canary, 999};
    XrFileReadStatus status = xr_os_io_read_under_root(policy, root, path, limit, &output);
    if (status != expected) fprintf(stderr, "%s: got %d expected %d\n", path, status, expected);
    REQUIRE(status == expected);
    if (status == XR_FILE_READ_OK) {
        REQUIRE(output.data != &canary && output.size == bytes && output.data[bytes] == 0);
        if (bytes == 140000) {
            for (size_t n = 0; n < bytes; ++n) REQUIRE(output.data[n] == 'Q');
        } else REQUIRE(!memcmp(output.data, !strcmp(path, "binary") ? "a\0b" : "abc", bytes));
        policy->free(policy->context, output.data);
    } else REQUIRE(output.data == &canary && output.size == 999);
}

static void fault_sweep(const char *root, const char *path, XrFileReadStatus expected) {
    Probe probe = {0};
    XrOsIoPolicy policy = {&probe, allocate, release};
    check(&policy, root, path, 10, expected, expected == XR_FILE_READ_OK ? 3 : 0);
    size_t attempts = probe.attempts;
    REQUIRE(attempts > 0 && !probe.live);
    const XrOsIoStatus failures[] = {XR_OS_IO_OUT_OF_MEMORY, XR_OS_IO_MEMORY_LIMIT, XR_OS_IO_OVERFLOW};
    const XrFileReadStatus statuses[] = {XR_FILE_READ_OUT_OF_MEMORY, XR_FILE_READ_MEMORY_LIMIT, XR_FILE_READ_OVERFLOW};
    for (size_t kind = 0; kind < 3; ++kind) {
        for (size_t failure = 1; failure <= attempts; ++failure) {
            probe = (Probe){0}; probe.fail_at = failure; probe.failure = failures[kind];
            check(&policy, root, path, 10, statuses[kind], 0);
            REQUIRE(!probe.live && probe.attempts == failure);
        }
    }
    printf("%s: %zu allocation sites, three failure statuses, zero residuals\n", path, attempts);
}

static void ledger_checks(const char *root) {
    const XrCompileResourceLimits ordinary = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrCompileResources *owner = NULL;
    REQUIRE(xr_compile_resources_new(&ordinary, &owner) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy = xr_compile_io_policy(owner);
    XrCompileResourceStats before, after;
    REQUIRE(xr_compile_resources_stats(owner, &before) == XR_COMPILE_RESOURCE_OK);
    check(&policy, root, "linked/data", 3, XR_FILE_READ_OK, 3);
    REQUIRE(xr_compile_resources_stats(owner, &after) == XR_COMPILE_RESOURCE_OK);
    REQUIRE(after.held_bytes == before.held_bytes && after.peak_bytes > before.held_bytes);
    uint64_t peak = after.peak_bytes;
    xr_compile_resources_release(owner);
    const XrCompileResourceLimits finite = {XR_COMPILE_MEMORY_FINITE, peak, true};
    owner = NULL;
    REQUIRE(xr_compile_resources_new(&finite, &owner) == XR_COMPILE_RESOURCE_OK);
    policy = xr_compile_io_policy(owner);
    check(&policy, root, "linked/data", 3, XR_FILE_READ_OK, 3);
    xr_compile_resources_release(owner);
    XrCompileResourceLimits short_limit = finite; --short_limit.memory_limit;
    owner = NULL;
    REQUIRE(xr_compile_resources_new(&short_limit, &owner) == XR_COMPILE_RESOURCE_OK);
    policy = xr_compile_io_policy(owner);
    check(&policy, root, "linked/data", 3, XR_FILE_READ_MEMORY_LIMIT, 0);
    xr_compile_resources_release(owner);

    owner = NULL;
    REQUIRE(xr_compile_resources_new(&ordinary, &owner) == XR_COMPILE_RESOURCE_OK);
    policy = xr_compile_io_policy(owner);
    XrFileBytes bytes = {0};
    REQUIRE(xr_os_io_read_under_root(&policy, root, "inside/data", 3, &bytes) == XR_FILE_READ_OK);
    xr_compile_resources_release(owner);
    REQUIRE(bytes.size == 3 && !memcmp(bytes.data, "abc\0", 4));
    policy.free(policy.context, bytes.data);
}

static unsigned descriptor_count(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 256; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 3);
    struct rlimit descriptors;
    REQUIRE(!getrlimit(RLIMIT_NOFILE, &descriptors));
    if (descriptors.rlim_cur > 256) { descriptors.rlim_cur = 256; REQUIRE(!setrlimit(RLIMIT_NOFILE, &descriptors)); }
    unsigned before = descriptor_count();
    XrOsIoPolicy policy = xr_os_io_system_policy();
    const char *root = argv[1];
    const char *const valid[] = {"inside/data", "linked/data", "file-link", "file-absolute",
        "parent-link", "escape/return/data", "chain-0", "\xE4\xB8\xAD\xE6\x96\x87"};
    for (size_t n = 0; n < sizeof(valid) / sizeof(valid[0]); ++n)
        check(&policy, root, valid[n], 3, XR_FILE_READ_OK, 3);
    check(&policy, argv[2], "inside/data", 3, XR_FILE_READ_OK, 3);
    check(&policy, root, "empty", 0, XR_FILE_READ_OK, 0);
    check(&policy, root, "binary", 3, XR_FILE_READ_OK, 3);
    check(&policy, root, "large", 140000, XR_FILE_READ_OK, 140000);
    check(&policy, root, "large", 139999, XR_FILE_READ_LIMIT, 0);
    check(&policy, root, "inside/data", 2, XR_FILE_READ_LIMIT, 0);
    const char *const missing[] = {"missing", "absent/child", "dangling-in"};
    for (size_t n = 0; n < sizeof(missing) / sizeof(missing[0]); ++n)
        check(&policy, root, missing[n], 10, XR_FILE_READ_MISSING, 0);
    const char *const forbidden[] = {"escape/data", "escape-file", "escape-sibling", "escape/missing",
        "escape/absent/child", "dangling-out", "dangling-out-deep", "inside", "directory-link",
        "trailing-file-link", "fifo", "socket", "inside/data/child", "", "../outside/data",
        "/inside/data", "inside//data", "inside/./data", "inside/../data", "inside/data.",
        "inside/data ", "inside\\data", "inside/data:stream", "inside/", "inside/\xFF"};
    for (size_t n = 0; n < sizeof(forbidden) / sizeof(forbidden[0]); ++n)
        check(&policy, root, forbidden[n], 10, XR_FILE_READ_FORBIDDEN, 0);
    check(&policy, root, "loop", 10, XR_FILE_READ_LIMIT, 0);
    check(&policy, root, "too-many", 10, XR_FILE_READ_LIMIT, 0);
    char deep[1024] = {0};
    for (unsigned n = 0; n < 258; ++n) strcat(deep, "d/");
    strcat(deep, "data");
    check(&policy, root, deep, 10, XR_FILE_READ_LIMIT, 0);
    char path[4096];
    REQUIRE(snprintf(path, sizeof(path), "%s/inside/data", root) > 0);
    check(&policy, path, "x", 10, XR_FILE_READ_FORBIDDEN, 0);
    REQUIRE(snprintf(path, sizeof(path), "%s/absent", root) > 0);
    check(&policy, path, "x", 10, XR_FILE_READ_IO, 0);
    check(&policy, "relative", "x", 10, XR_FILE_READ_FORBIDDEN, 0);
    check(&policy, (char *)1, ":", 10, XR_FILE_READ_FORBIDDEN, 0);
    check(NULL, (char *)1, (char *)1, 0, XR_FILE_READ_BAD_ARGUMENT, 0);
    XrFileBytes output = {0};
    REQUIRE(xr_os_io_read_under_root(&policy, NULL, (char *)1, 0, &output) == XR_FILE_READ_BAD_ARGUMENT);
    REQUIRE(xr_os_io_read_under_root(&policy, root, NULL, 0, &output) == XR_FILE_READ_BAD_ARGUMENT);
    REQUIRE(xr_os_io_read_under_root(&policy, root, "inside/data", 0, NULL) == XR_FILE_READ_BAD_ARGUMENT);
    fault_sweep(root, "inside/data", XR_FILE_READ_OK);
    fault_sweep(root, "linked/data", XR_FILE_READ_OK);
    fault_sweep(root, "escape/data", XR_FILE_READ_FORBIDDEN);
    fault_sweep(root, "dangling-out", XR_FILE_READ_FORBIDDEN);
    fault_sweep(root, "dangling-in", XR_FILE_READ_MISSING);
    ledger_checks(root);
    REQUIRE(descriptor_count() == before);
    puts("Physical-root file reads passed with no allocation or descriptor residuals");
    return 0;
}
