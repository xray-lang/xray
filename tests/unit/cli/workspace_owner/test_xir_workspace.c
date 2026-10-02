/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_workspace.c - Partial creations, private descendants and cleanup
 */
#include "app/toolchain/xtc_xir_workspace.h"
#include <windows.h>
#include <winternl.h>
#include <bcrypt.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s error=%lu\n", __LINE__, #c, GetLastError()); exit(1); } } while (0)
#include "workspace_faults.inc.h"

static DWORD handle_count(void) { DWORD count = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &count)); return count; }
static DWORD native_random_boundary(void) {
    DWORD initial = handle_count(), counts[3]; UCHAR nonce[16];
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(BCryptGenRandom(NULL, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0);
        counts[i] = handle_count();
    }
    printf("raw BCrypt sequence handles initial=%lu first=%lu second=%lu third=%lu\n", initial, counts[0], counts[1], counts[2]);
    CHECK(counts[1] == counts[2]); return counts[2];
}
static XtcXirWorkspaceRequest request_for(const char *parent) {
    return (XtcXirWorkspaceRequest){parent, {4096, 32, 4096}};
}
static void path_join(char *out, size_t capacity, const char *parent, const char *leaf) {
    CHECK(sprintf_s(out, capacity, "%s/%s", parent, leaf) > 0);
}
static void write_file(const char *path, const char *text) {
    HANDLE handle = CreateFileA(path, GENERIC_WRITE, 7, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE); DWORD length = (DWORD)strlen(text), written = 0;
    CHECK(WriteFile(handle, text, length, &written, NULL) && written == length); CHECK(CloseHandle(handle));
}
static void descendants(XtcXirWorkspace *owner) {
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(owner); char path[8192], nested[8192];
    path_join(path, sizeof(path), paths->input, "source.c"); write_file(path, "int main(void){return 0;}\n");
    path_join(path, sizeof(path), paths->output, "compiler-temp"); CHECK(CreateDirectoryA(path, NULL));
    path_join(nested, sizeof(nested), path, "unrecorded.tmp"); write_file(nested, "unrecorded private temporary contents\n");
    path_join(path, sizeof(path), paths->output, "partial.obj"); write_file(path, "partial failure artifact\n");
}
static void drain(XtcXirWorkspace **owner, uint32_t steps) {
    for (unsigned calls = 0; *owner && calls < 100000; ++calls) {
#ifndef WORKSPACE_PRODUCTION
        size_t before = io_count;
#endif
        XtcXirWorkspaceStatus status = xtc_xir_workspace_close(owner, steps);
#ifndef WORKSPACE_PRODUCTION
        CHECK(io_count - before <= steps);
#endif
        CHECK(status == XTC_XIR_WORKSPACE_PENDING || status == XTC_XIR_WORKSPACE_OK);
    }
    CHECK(!*owner);
}
static XrCompileResourceStats run(const char *parent, const XrCompileResourceLimits *limits,
    XtcXirWorkspaceStatus expected, bool contents) {
    XrCompileResources *resources = NULL; XtcXirWorkspace *owner = NULL;
    XrCompileResourceStatus admitted = xr_compile_resources_new(limits, &resources);
    if (admitted != XR_COMPILE_RESOURCE_OK) {
        CHECK(admitted == (expected == XTC_XIR_WORKSPACE_BUDGET ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY));
        CHECK(!runtime_live && !runtime_bytes); return (XrCompileResourceStats){0};
    }
    XtcXirWorkspaceRequest request = request_for(parent);
    XtcXirWorkspaceStatus status = xtc_xir_workspace_new(resources, &request, &owner);
    if (status == XTC_XIR_WORKSPACE_OK) status = xtc_xir_workspace_create(owner);
    CHECK(status == expected);
    if (contents && status == XTC_XIR_WORKSPACE_OK) descendants(owner);
    XrCompileResourceStats stats = sdk_stats(resources);
    size_t attempts = runtime_attempts;
    drain(&owner, 1);
    CHECK(runtime_attempts == attempts && sdk_stats(resources).work == stats.work);
    xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
#ifndef WORKSPACE_PRODUCTION
    CHECK(!owner_handle_count);
#endif
    return stats;
}
static void basics(const char *parent) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest request = request_for(parent);
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_resources(owner) == resources);
    CHECK(!xtc_xir_workspace_paths(owner)->root);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_INVALID);
    char original[8192]; strcpy_s(original, sizeof(original), xtc_xir_workspace_paths(owner)->root);
    descendants(owner);
    XrCompileResourceStats stats = sdk_stats(resources);
    CHECK(xr_compile_resources_work(resources, UINT64_MAX - stats.work) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(resources, 1) == XR_COMPILE_RESOURCE_BUDGET);
    size_t attempts = runtime_attempts; runtime_fail_at = attempts;
    xr_compile_resources_release(resources);
    drain(&owner, 1);
    runtime_fail_at = SIZE_MAX;
    CHECK(runtime_attempts == attempts && !runtime_live && !runtime_bytes);
    CHECK(GetFileAttributesA(original) == INVALID_FILE_ATTRIBUTES);
    CHECK(xtc_xir_workspace_close(&owner, 1) == XTC_XIR_WORKSPACE_OK);
    puts("exhausted-ledger nested TEMP cleanup, producer death, step=1, disk/heap/handles PASS");
}
static void compiler_transaction(const char *parent) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest request = request_for(parent); char path[8192];
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(owner);
    path_join(path, sizeof(path), paths->input, "source.c"); write_file(path, "int workspace_answer(void){return 42;}\n");
    path_join(path, sizeof(path), paths->input, "bad.c"); write_file(path, "#error workspace-compiler-failure\n");
    printf("compiler_root=%s\ncompiler_input=%s\ncompiler_output=%s\n", paths->root, paths->input, paths->output);
    fflush(stdout); CHECK(getchar() == 'c');
    path_join(path, sizeof(path), paths->output, "compiler-unrecorded.tmp"); CHECK(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
    XrCompileResourceStats stats = sdk_stats(resources);
    CHECK(xr_compile_resources_work(resources, UINT64_MAX - stats.work) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(resources, 1) == XR_COMPILE_RESOURCE_BUDGET);
    size_t attempts = runtime_attempts; runtime_fail_at = attempts;
    xr_compile_resources_release(resources); drain(&owner, 1); runtime_fail_at = SIZE_MAX;
    CHECK(attempts == runtime_attempts && !runtime_live && !runtime_bytes);
#ifndef WORKSPACE_PRODUCTION
    CHECK(!owner_handle_count);
#endif
    puts("real compiler unregistered output cleanup after BUDGET PASS");
}
static void arguments_and_depth(const char *parent) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest request = request_for(parent);
    XtcXirWorkspace *canary = (XtcXirWorkspace *)(uintptr_t)0x1234;
    CHECK(xtc_xir_workspace_new(resources, &request, &canary) == XTC_XIR_WORKSPACE_INVALID && (uintptr_t)canary == 0x1234);
    CHECK(xtc_xir_workspace_new(NULL, &request, &owner) == XTC_XIR_WORKSPACE_INVALID && !owner);
    request.absolute_parent = "relative";
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_INVALID && !owner);
    request.absolute_parent = "C:\\bad\xff";
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_INVALID && !owner);
    request = request_for(parent); request.limits.depth = 1;
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_BUDGET && !owner);
    request = request_for(parent); request.limits.enum_bytes = 100;
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_BUDGET && !owner);
    request = request_for(parent); request.limits.path_bytes = 2;
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_BUDGET && !owner);
    char copied[8192]; strcpy_s(copied, sizeof(copied), parent); request = request_for(copied); request.limits.depth = 2;
#ifndef WORKSPACE_PRODUCTION
    size_t no_io = io_count;
#endif
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
#ifndef WORKSPACE_PRODUCTION
    CHECK(io_count == no_io);
#endif
    memset(copied, '?', strlen(copied));
    CHECK(xtc_xir_workspace_close(&owner, 0) == XTC_XIR_WORKSPACE_INVALID && owner);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    char path[8192], file[8192]; path_join(path, sizeof(path), xtc_xir_workspace_paths(owner)->output, "too-deep");
    CHECK(CreateDirectoryA(path, NULL));
    path_join(file, sizeof(file), xtc_xir_workspace_paths(owner)->input, "leaf.c"); write_file(file, "leaf uses reserved file frame\n");
    XrCompileResourceStats stats = sdk_stats(resources);
    CHECK(xtc_xir_workspace_close(&owner, 1000) == XTC_XIR_WORKSPACE_BUDGET && owner);
    CHECK(sdk_stats(resources).work == stats.work && xtc_xir_workspace_status(owner) == XTC_XIR_WORKSPACE_BUDGET);
    CHECK(RemoveDirectoryA(path)); drain(&owner, 1); xr_compile_resources_release(resources); CHECK(!runtime_live);
    puts("argument/output preservation, no-I/O new, copied producer text, directory depth bound PASS");
}
static void cumulative(const char *parent) {
    XtcXirWorkspaceRequest request = request_for(parent);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    uint64_t initial = sdk_stats(resources).work, first = 0;
    for (unsigned i = 0; i < 2; ++i) {
        XtcXirWorkspace *owner = NULL;
        CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
        CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK); drain(&owner, 1);
        if (!i) first = sdk_stats(resources).work;
    }
    uint64_t total = sdk_stats(resources).work; CHECK(total == initial + 2 * (first - initial));
    xr_compile_resources_release(resources);
    XrCompileResourceLimits limit = sdk_unlimited; limit.work = total - 1; resources = sdk_ledger(&limit);
    for (unsigned i = 0; i < 2; ++i) {
        XtcXirWorkspace *owner = NULL;
        XtcXirWorkspaceStatus result = xtc_xir_workspace_new(resources, &request, &owner);
        if (result == XTC_XIR_WORKSPACE_OK) result = xtc_xir_workspace_create(owner);
        CHECK(result == (i ? XTC_XIR_WORKSPACE_BUDGET : XTC_XIR_WORKSPACE_OK)); drain(&owner, 1);
    }
    xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
    puts("two successive owners consume the original cumulative ledger PASS");
}
#ifndef WORKSPACE_PRODUCTION
static void release_running_leases(XtcXirWorkspace *owner) {
    for (unsigned i = 0; i < 3; ++i) {
        xtc_xir_file_lease_free(owner->nodes[i].lease); owner->nodes[i].lease = NULL;
    }
}
static void fixed_identity_retry(const char *parent) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest request = request_for(parent);
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    unsigned attempts = 0;
    while (!(owner->close_phase == 3 && owner->frame_count == 2 && owner->frames[1].state == 1)) {
        CHECK(++attempts < 1000); CHECK(xtc_xir_workspace_close(&owner, 1) == XTC_XIR_WORKSPACE_PENDING);
    }
    CHECK(owner->frames[1].fixed > 0); corrupt_identity = true;
    CHECK(xtc_xir_workspace_close(&owner, 1) == XTC_XIR_WORKSPACE_PENDING && !corrupt_identity);
    CHECK(owner && owner->frames[1].handle && owner->frames[1].state == 10);
    CHECK(xtc_xir_workspace_close(&owner, 1) == XTC_XIR_WORKSPACE_IDENTITY_MISMATCH);
    CHECK(xtc_xir_workspace_status(owner) == XTC_XIR_WORKSPACE_IDENTITY_MISMATCH);
    drain(&owner, 1); xr_compile_resources_release(resources); CHECK(!runtime_live && !owner_handle_count);
    puts("fixed child ID rejection retains anchor/creation ID and reacquires cleanup pin on retry PASS");
}
static void collision(const char *parent) {
    char existing[8192], marker[8192];
    path_join(existing, sizeof(existing), parent, "xray-00000000000000000000000000000000");
    CHECK(CreateDirectoryA(existing, NULL)); path_join(marker, sizeof(marker), existing, "keep.txt");
    write_file(marker, "existing object preserved\n");
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest request = request_for(parent);
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    fixed_nonce = true; CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_IO); fixed_nonce = false;
    CHECK(!owner->nodes[0].exists && owner->diagnostic.os_error == ERROR_ALREADY_EXISTS);
    drain(&owner, 1); CHECK(GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES);
    CHECK(DeleteFileA(marker) && RemoveDirectoryA(existing)); xr_compile_resources_release(resources);
    CHECK(!runtime_live && !owner_handle_count);
}
static void replacement(const char *parent, unsigned which) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest request = request_for(parent);
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    stop_before_input = which == 0;
    XtcXirWorkspaceStatus first = stop_before_input ? XTC_XIR_WORKSPACE_IO : XTC_XIR_WORKSPACE_OK;
    CHECK(xtc_xir_workspace_create(owner) == first); stop_before_input = false;
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(owner);
    const char *original = which == 0 ? paths->root : which == 1 ? paths->input : paths->output;
    char moved[8192], marker[8192]; CHECK(sprintf_s(moved, sizeof(moved), "%s-moved", original) > 0);
    release_running_leases(owner);
    if (which == 0) {
        CHECK(owner->nodes[0].created && owner->nodes[0].anchor);
        char child[8192], renamed[8192];
        path_join(child, sizeof(child), original, "owned-child.txt");
        path_join(renamed, sizeof(renamed), original, "renamed-child.txt");
        write_file(child, "private child writes and renames remain legal\n");
        CHECK(MoveFileA(child, renamed));
        CHECK(!MoveFileA(original, moved) && GetLastError() == ERROR_SHARING_VIOLATION);
        corrupt_identity = true;
        CHECK(xtc_xir_workspace_close(&owner, 1000) == XTC_XIR_WORKSPACE_IDENTITY_MISMATCH);
        CHECK(owner && !corrupt_identity && xtc_xir_workspace_status(owner) == first);
        drain(&owner, 1); xr_compile_resources_release(resources); CHECK(!runtime_live && !owner_handle_count); return;
    }
    CHECK(MoveFileA(original, moved)); CHECK(CreateDirectoryA(original, NULL));
    path_join(marker, sizeof(marker), original, "keep.txt"); write_file(marker, "replacement preserved\n");
    CHECK(xtc_xir_workspace_close(&owner, 1000) == XTC_XIR_WORKSPACE_IDENTITY_MISMATCH);
    CHECK(owner && GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES);
    CHECK(DeleteFileA(marker) && RemoveDirectoryA(original)); CHECK(MoveFileA(moved, original));
    CHECK(xtc_xir_workspace_status(owner) == (first == XTC_XIR_WORKSPACE_OK ? XTC_XIR_WORKSPACE_IDENTITY_MISMATCH : first));
    drain(&owner, 1); xr_compile_resources_release(resources); CHECK(!runtime_live && !owner_handle_count);
}
static void set_junction(const char *link, const char *outside) {
    struct MountPoint { ULONG tag; USHORT bytes, reserved, sub_offset, sub_length, print_offset, print_length; WCHAR path[4096]; } data = {0};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    memcpy(data.path, L"\\??\\", 4 * sizeof(wchar_t));
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, outside, -1, data.path + 4, 4092); CHECK(count > 0);
    for (int i = 4; i < count + 3; ++i) if (data.path[i] == '/') data.path[i] = '\\';
    data.sub_length = (USHORT)((count + 3) * 2); data.print_offset = data.sub_length + 2;
    data.bytes = (USHORT)(8 + data.sub_length + 4);
    HANDLE h = CreateFileA(link, GENERIC_WRITE, 7, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    CHECK(h != INVALID_HANDLE_VALUE); DWORD returned = 0;
    CHECK(DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &data, data.bytes + 8, NULL, 0, &returned, NULL)); CHECK(CloseHandle(h));
}
static void reparse_refusal(const char *parent) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest request = request_for(parent);
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    char outside[8192], marker[8192], link[8192];
    path_join(outside, sizeof(outside), parent, "outside-junction"); CHECK(CreateDirectoryA(outside, NULL));
    path_join(marker, sizeof(marker), outside, "keep.txt"); write_file(marker, "outside junction contents\n");
    path_join(link, sizeof(link), xtc_xir_workspace_paths(owner)->output, "junction"); CHECK(CreateDirectoryA(link, NULL));
    set_junction(link, outside);
    CHECK(xtc_xir_workspace_close(&owner, 1000) == XTC_XIR_WORKSPACE_REPARSE);
    CHECK(owner && GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES);
    CHECK(RemoveDirectoryA(link)); drain(&owner, 1);
    CHECK(DeleteFileA(marker) && RemoveDirectoryA(outside)); xr_compile_resources_release(resources); CHECK(!runtime_live && !owner_handle_count);
}
static void root_reparse_refusal(const char *parent) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest request = request_for(parent);
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    stop_before_input = true; CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_IO); stop_before_input = false;
    release_running_leases(owner);
    char outside[8192], marker[8192];
    path_join(outside, sizeof(outside), parent, "outside-root-junction"); CHECK(CreateDirectoryA(outside, NULL));
    path_join(marker, sizeof(marker), outside, "keep.txt"); write_file(marker, "outside root contents\n");
    set_junction(xtc_xir_workspace_paths(owner)->root, outside);
    CHECK(xtc_xir_workspace_close(&owner, 1000) == XTC_XIR_WORKSPACE_REPARSE);
    CHECK(owner && GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES && owner->nodes[0].anchor);
    HANDLE handle = CreateFileA(xtc_xir_workspace_paths(owner)->root, GENERIC_WRITE, 7, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL); CHECK(handle != INVALID_HANDLE_VALUE);
    struct { DWORD tag; WORD bytes, reserved; } data = {IO_REPARSE_TAG_MOUNT_POINT, 0, 0}; DWORD returned = 0;
    CHECK(DeviceIoControl(handle, FSCTL_DELETE_REPARSE_POINT, &data, sizeof(data), NULL, 0, &returned, NULL)); CHECK(CloseHandle(handle));
    CHECK(xtc_xir_workspace_status(owner) == XTC_XIR_WORKSPACE_IO);
    drain(&owner, 1); CHECK(DeleteFileA(marker) && RemoveDirectoryA(outside));
    xr_compile_resources_release(resources); CHECK(!runtime_live && !owner_handle_count);
    puts("original partial root changed to junction rejects before enumeration; outside bytes survive; retry PASS");
}
static void hardlink(const char *parent) {
    char outside[8192], link[8192]; path_join(outside, sizeof(outside), parent, "outside.txt");
    write_file(outside, "external hard-link bytes stay\n");
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest request = request_for(parent);
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    path_join(link, sizeof(link), xtc_xir_workspace_paths(owner)->output, "linked.tmp");
    /* The Windows hard-link API requests directory access excluded by the
     * running leases. Fixture mutation happens after their explicit release. */
    release_running_leases(owner);
    xtc_xir_file_lease_free(owner->parent_lease); owner->parent_lease = NULL;
    CHECK(CreateHardLinkA(link, outside, NULL)); drain(&owner, 2);
    HANDLE file = CreateFileA(outside, GENERIC_READ, 7, NULL, OPEN_EXISTING, 0, NULL); CHECK(file != INVALID_HANDLE_VALUE);
    char bytes[64] = {0}; DWORD length = 0;
    CHECK(ReadFile(file, bytes, sizeof(bytes), &length, NULL)); CHECK(!strcmp(bytes, "external hard-link bytes stay\n"));
    CHECK(CloseHandle(file)); CHECK(DeleteFileA(outside)); xr_compile_resources_release(resources); CHECK(!runtime_live);
}
#endif
static void resources_matrix(const char *parent) {
    record_physical = record_work = true; physical_total = physical_peak = work_count = 0; runtime_attempts = 0;
    XrCompileResourceStats stats = run(parent, &sdk_unlimited, XTC_XIR_WORKSPACE_OK, false);
    record_physical = record_work = false; size_t allocations = runtime_attempts, edges = work_count;
    CHECK(stats.allocated_bytes == physical_total && stats.peak_bytes == physical_peak);
    for (size_t n = 0; n < allocations; ++n) {
        runtime_attempts = 0; runtime_fail_at = n;
        run(parent, &sdk_unlimited, XTC_XIR_WORKSPACE_OUT_OF_MEMORY, false); runtime_fail_at = SIZE_MAX;
    }
    XrCompileResourceLimits exact = {stats.allocated_bytes, stats.peak_bytes, stats.work};
    run(parent, &exact, XTC_XIR_WORKSPACE_OK, false);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits limited = exact;
        if (axis == 0) --limited.allocated_bytes;
        if (axis == 1) --limited.live_bytes;
        if (axis == 2) --limited.work;
        run(parent, &limited, XTC_XIR_WORKSPACE_BUDGET, false);
    }
    for (size_t i = 0; i < edges; ++i) {
        XrCompileResourceLimits limited = sdk_unlimited; limited.work = work_edges[i] - 1;
        run(parent, &limited, XTC_XIR_WORKSPACE_BUDGET, false);
    }
    printf("allocations=%zu real OOM; work_edges=%zu; allocated=%llu peak=%llu work=%llu; physical oracle PASS\n",
        allocations, edges, (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.peak_bytes,
        (unsigned long long)stats.work);
}
#ifndef WORKSPACE_PRODUCTION
static void os_matrix(const char *parent) {
    io_count = 0;
    run(parent, &sdk_unlimited, XTC_XIR_WORKSPACE_OK, true);
    size_t all_calls = io_count;
    for (unsigned kind = 0; kind < 2; ++kind) for (size_t n = 0; n < all_calls; ++n) {
        XrCompileResources *resources = sdk_ledger(&sdk_unlimited); XtcXirWorkspace *owner = NULL;
        XtcXirWorkspaceRequest request = request_for(parent);
        CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
        io_count = 0; io_fail_at = n; io_error = kind ? ERROR_NOT_ENOUGH_MEMORY : ERROR_ACCESS_DENIED;
        XtcXirWorkspaceStatus status = xtc_xir_workspace_create(owner);
        XtcXirWorkspaceStatus failure = kind ? XTC_XIR_WORKSPACE_OUT_OF_MEMORY : XTC_XIR_WORKSPACE_IO;
        CHECK(status == XTC_XIR_WORKSPACE_OK || status == failure);
        if (status == XTC_XIR_WORKSPACE_OK) descendants(owner);
        while (owner) {
            status = xtc_xir_workspace_close(&owner, 1);
            CHECK(status == XTC_XIR_WORKSPACE_OK || status == XTC_XIR_WORKSPACE_PENDING || status == failure);
        }
        CHECK(io_count > n); io_fail_at = SIZE_MAX;
        xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes && !owner_handle_count);
    }
    printf("workspace OS operation failures=%zu x2; recoverable create and close PASS\n", all_calls);
}
#endif
int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3); DWORD before = native_random_boundary();
    if (argc == 3) {
#ifndef WORKSPACE_PRODUCTION
        if (!strcmp(argv[2], "--anchor-only")) {
            for (unsigned i = 0; i < 3; ++i) replacement(argv[1], i);
            reparse_refusal(argv[1]); root_reparse_refusal(argv[1]); fixed_identity_retry(argv[1]);
            CHECK(handle_count() == before && !runtime_live && !runtime_bytes && !owner_handle_count);
            puts("root pin/child rename/replacement/reparse/close-one-step retry physical zero PASS"); return 0;
        }
#endif
        CHECK(!strcmp(argv[2], "--compiler")); compiler_transaction(argv[1]);
        CHECK(handle_count() == before); return 0;
    }
    printf("initial handles=%lu\n", before);
    basics(argv[1]); printf("after basics handles=%lu\n", handle_count());
    arguments_and_depth(argv[1]); cumulative(argv[1]);
    resources_matrix(argv[1]);
    printf("after resources handles=%lu\n", handle_count());
#ifndef WORKSPACE_PRODUCTION
    hardlink(argv[1]); printf("after hardlink handles=%lu\n", handle_count());
    collision(argv[1]);
    for (unsigned i = 0; i < 3; ++i) replacement(argv[1], i);
    reparse_refusal(argv[1]); root_reparse_refusal(argv[1]); fixed_identity_retry(argv[1]);
    os_matrix(argv[1]); printf("after OS handles=%lu\n", handle_count());
#endif
    DWORD after = handle_count(); printf("final handles=%lu live=%zu bytes=%zu\n", after, runtime_live, runtime_bytes);
    CHECK(before == after && !runtime_live && !runtime_bytes);
    printf("handles before=%lu after=%lu; PASS workspace owner\n", before, after); return 0;
}
