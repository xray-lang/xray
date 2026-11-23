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
#include <wchar.h>
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
        CHECK(admitted == (expected == XTC_XIR_WORKSPACE_MEMORY_LIMIT ? XR_COMPILE_RESOURCE_MEMORY_LIMIT :
            expected == XTC_XIR_WORKSPACE_OVERFLOW ? XR_COMPILE_RESOURCE_OVERFLOW : XR_COMPILE_RESOURCE_OUT_OF_MEMORY));
        CHECK(!runtime_live && !runtime_bytes); return (XrCompileResourceStats){0};
    }
    uint64_t baseline = sdk_stats(resources).held_bytes;
    XtcXirWorkspaceRequest request = request_for(parent);
    XtcXirWorkspaceStatus status = xtc_xir_workspace_new(resources, &request, &owner);
    if (status == XTC_XIR_WORKSPACE_OK) status = xtc_xir_workspace_create(owner);
    CHECK(status == expected);
    if (contents && status == XTC_XIR_WORKSPACE_OK) descendants(owner);
    XrCompileResourceStats stats = sdk_stats(resources);
    size_t attempts = runtime_attempts;
    drain(&owner, 1);
    XrCompileResourceStats closed = sdk_stats(resources);
    CHECK(runtime_attempts == attempts && closed.allocation_count == stats.allocation_count &&
        closed.allocated_bytes == stats.allocated_bytes && closed.held_bytes == baseline);
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
    size_t attempts = runtime_attempts; runtime_fail_at = attempts;
    xr_compile_resources_release(resources);
    drain(&owner, 1);
    runtime_fail_at = SIZE_MAX;
    CHECK(runtime_attempts == attempts && !runtime_live && !runtime_bytes);
    CHECK(GetFileAttributesA(original) == INVALID_FILE_ATTRIBUTES);
    CHECK(xtc_xir_workspace_close(&owner, 1) == XTC_XIR_WORKSPACE_OK);
    puts("allocator-denied nested TEMP cleanup, producer death, step=1, disk/heap/handles PASS");
}
static void busy_regular_file(const char *parent) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XtcXirWorkspace *owner = NULL;
    XtcXirWorkspaceRequest request = request_for(parent);
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    char path[8192];
    path_join(path, sizeof(path), xtc_xir_workspace_paths(owner)->input, "source.c");
    write_file(path, "private C source with an outstanding reader\n");
    HANDLE reader = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(reader != INVALID_HANDLE_VALUE);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        CHECK(xtc_xir_workspace_close(&owner, 1000) == XTC_XIR_WORKSPACE_SHARING_RETRY && owner);
        CHECK(xtc_xir_workspace_status(owner) == XTC_XIR_WORKSPACE_OK);
        CHECK(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
    }
    CHECK(CloseHandle(reader));
    drain(&owner, 1);
    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
    xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
#ifndef WORKSPACE_PRODUCTION
    CHECK(!owner_handle_count);
#endif
    puts("real Windows reader blocks an enumerated file until its handle closes PASS");
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
    size_t attempts = runtime_attempts; runtime_fail_at = attempts;
    xr_compile_resources_release(resources); drain(&owner, 1); runtime_fail_at = SIZE_MAX;
    CHECK(attempts == runtime_attempts && !runtime_live && !runtime_bytes);
#ifndef WORKSPACE_PRODUCTION
    CHECK(!owner_handle_count);
#endif
    puts("real compiler unregistered output cleanup after producer death and allocator denial PASS");
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
    CHECK(sdk_stats(resources).allocation_count == stats.allocation_count &&
        sdk_stats(resources).allocated_bytes == stats.allocated_bytes &&
        xtc_xir_workspace_status(owner) == XTC_XIR_WORKSPACE_BUDGET);
    CHECK(RemoveDirectoryA(path)); drain(&owner, 1); xr_compile_resources_release(resources); CHECK(!runtime_live);
    puts("argument/output preservation, no-I/O new, copied producer text, directory depth bound PASS");
}
static void memory_owner_reuse(const char *parent) {
    XrCompileResourceStats measured = run(parent, &sdk_unlimited, XTC_XIR_WORKSPACE_OK, false);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, measured.peak_bytes, true};
    XrCompileResources *resources = sdk_ledger(&limits);
    uint64_t baseline = sdk_stats(resources).held_bytes;
    XtcXirWorkspaceRequest request = request_for(parent);
    for (unsigned i = 0; i < 2; ++i) {
        XtcXirWorkspace *owner = NULL;
        CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
        CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
        void *rejected = NULL;
        size_t attempts = runtime_attempts;
        CHECK(limits.memory_limit <= SIZE_MAX);
        CHECK(xr_compile_resources_alloc(resources, (size_t)limits.memory_limit, &rejected) ==
            XR_COMPILE_RESOURCE_MEMORY_LIMIT && !rejected);
        CHECK(runtime_attempts == attempts && xtc_xir_workspace_status(owner) == XTC_XIR_WORKSPACE_OK);
        drain(&owner, 1);
        CHECK(runtime_attempts == attempts && sdk_stats(resources).held_bytes == baseline);
    }
    xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
    puts("two successive owners reuse held memory after precise admission refusal; allocation-free close PASS");
}
#ifndef WORKSPACE_PRODUCTION
static void release_running_leases(XtcXirWorkspace *owner) {
    for (unsigned i = 0; i < 3; ++i) {
        xtc_xir_file_lease_free(owner->nodes[i].lease); owner->nodes[i].lease = NULL;
    }
}
static WorkspaceFrame *enumerate_output_child(XtcXirWorkspace **owner, const wchar_t *name) {
    for (unsigned steps = 0;; ++steps) {
        XtcXirWorkspace *workspace = *owner;
        CHECK(steps < 1000 && workspace);
        if (workspace->close_phase == 3 && workspace->frame_count == 3) {
            WorkspaceFrame *frame = &workspace->frames[2];
            if (frame->state == 0 && !wcscmp(frame->name, name)) {
                CHECK(frame->fixed < 0 && !frame->handle);
                return frame;
            }
        }
        CHECK(xtc_xir_workspace_close(owner, 1) == XTC_XIR_WORKSPACE_PENDING);
    }
}

static void enumerated_child_open(const char *parent, bool disappears) {
    XtcXirWorkspaceRequest request = request_for(parent);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XtcXirWorkspace *owner = NULL;
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(owner);
    char root[8192], vanished[8192], survivor[8192];
    strcpy_s(root, sizeof(root), paths->root);
    path_join(vanished, sizeof(vanished), paths->output, "vanish.tmp");
    path_join(survivor, sizeof(survivor), paths->output, "zz-program.obj");
    write_file(vanished, "directory entry will disappear after enumeration\n");
    write_file(survivor, "remaining output must still be cleaned\n");

    enumerate_output_child(&owner, L"vanish.tmp");
    size_t before = io_count;
    CHECK(GetFileAttributesA(survivor) != INVALID_FILE_ATTRIBUTES);
    if (disappears) {
        CHECK(DeleteFileA(vanished));
        XtcXirWorkspaceStatus status = xtc_xir_workspace_close(&owner, 1);
        DWORD error = RtlNtStatusToDosError(last_relative_open);
        printf("disappeared child relative open NTSTATUS=0x%08lx, Win32=%lu, close status=%u\n",
            (unsigned long)last_relative_open, error, (unsigned)status);
        CHECK(error == ERROR_FILE_NOT_FOUND);
        CHECK(status == XTC_XIR_WORKSPACE_PENDING && owner->frame_count == 2 && owner->frames[1].restart);
        CHECK(xtc_xir_workspace_status(owner) == XTC_XIR_WORKSPACE_OK);
    } else {
        io_error = ERROR_PATH_NOT_FOUND;
        io_fail_at = before;
        CHECK(xtc_xir_workspace_close(&owner, 1) == XTC_XIR_WORKSPACE_UNRESOLVED);
        io_fail_at = SIZE_MAX;
        CHECK(owner->frame_count == 3 && owner->frames[2].state == 0 && !owner->frames[2].handle);
        const XtcXirWorkspaceDiagnostic *diagnostic = xtc_xir_workspace_diagnostic(owner);
        CHECK(diagnostic->status == XTC_XIR_WORKSPACE_UNRESOLVED && diagnostic->os_error == ERROR_PATH_NOT_FOUND);
        CHECK(GetFileAttributesA(vanished) != INVALID_FILE_ATTRIBUTES);
    }
    CHECK(io_count - before == 1);
    drain(&owner, 1);
    CHECK(GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(survivor) == INVALID_FILE_ATTRIBUTES);
    xr_compile_resources_release(resources);
    CHECK(!runtime_live && !owner_handle_count);
}

static void enumerated_child_sharing(const char *parent, bool directory) {
    XtcXirWorkspaceRequest request = request_for(parent);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XtcXirWorkspace *owner = NULL;
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    const XtcXirWorkspacePaths *paths = xtc_xir_workspace_paths(owner);
    char path[8192];
    path_join(path, sizeof(path), paths->output, "busy");
    if (directory) CHECK(CreateDirectoryA(path, NULL));
    else write_file(path, "identity must still be checked after reopen\n");

    WorkspaceFrame *frame = enumerate_output_child(&owner, L"busy");
    CHECK(frame->directory == directory);
    FILE_ID_INFO identity = frame->identity;
    HANDLE parent_pin = owner->frames[1].handle;
    CHECK(parent_pin);
    size_t before = io_count, handles = owner_handle_count;
    io_error = ERROR_SHARING_VIOLATION;
    io_fail_at = before;
    XtcXirWorkspaceStatus expected = directory ? XTC_XIR_WORKSPACE_IO : XTC_XIR_WORKSPACE_SHARING_RETRY;
    CHECK(xtc_xir_workspace_close(&owner, 1000) == expected);
    io_fail_at = SIZE_MAX;
    CHECK(io_count == before + 1 && owner_handle_count == handles);
    CHECK(owner->frame_count == 3 && owner->frames[1].handle == parent_pin);
    CHECK(frame->state == 0 && !frame->handle && !wcscmp(frame->name, L"busy"));
    CHECK(workspace_identity_equal(&identity, &frame->identity));
    CHECK(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);

    const XtcXirWorkspaceDiagnostic *diagnostic = xtc_xir_workspace_diagnostic(owner);
    if (directory) {
        CHECK(diagnostic->status == XTC_XIR_WORKSPACE_IO && diagnostic->os_error == ERROR_SHARING_VIOLATION);
    } else {
        CHECK(diagnostic->status == XTC_XIR_WORKSPACE_OK && diagnostic->os_error == 0);
        CHECK(xtc_xir_workspace_close(&owner, 1) == XTC_XIR_WORKSPACE_PENDING);
        CHECK(frame->state == 1 && frame->handle && workspace_identity_equal(&identity, &frame->identity));
        corrupt_identity = true;
        CHECK(xtc_xir_workspace_close(&owner, 1000) == XTC_XIR_WORKSPACE_IDENTITY_MISMATCH);
        CHECK(!corrupt_identity && diagnostic->status == XTC_XIR_WORKSPACE_IDENTITY_MISMATCH);
        CHECK(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
    }
    drain(&owner, 1);
    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
    xr_compile_resources_release(resources);
    CHECK(!runtime_live && !owner_handle_count);
}

static void fixed_child_open_failure(const char *parent, DWORD error, XtcXirWorkspaceStatus expected) {
    XtcXirWorkspaceRequest request = request_for(parent);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XtcXirWorkspace *owner = NULL;
    CHECK(xtc_xir_workspace_new(resources, &request, &owner) == XTC_XIR_WORKSPACE_OK);
    CHECK(xtc_xir_workspace_create(owner) == XTC_XIR_WORKSPACE_OK);
    for (unsigned steps = 0; owner->close_phase != 2 || owner->close_index != 1; ++steps) {
        CHECK(steps < 1000 && xtc_xir_workspace_close(&owner, 1) == XTC_XIR_WORKSPACE_PENDING);
    }
    io_error = error;
    io_fail_at = io_count;
    CHECK(xtc_xir_workspace_close(&owner, 1) == expected);
    CHECK(owner->close_index == 1 && !owner->nodes[1].deletion && owner->nodes[1].anchor);
    const XtcXirWorkspaceDiagnostic *diagnostic = xtc_xir_workspace_diagnostic(owner);
    CHECK(diagnostic->status == expected && diagnostic->os_error == error);
    io_fail_at = SIZE_MAX;
    drain(&owner, 1);
    xr_compile_resources_release(resources);
    CHECK(!runtime_live && !owner_handle_count);
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
static FILE_ID_INFO fixture_identity(HANDLE directory) {
    FILE_ID_INFO identity = {0};
    CHECK(GetFileInformationByHandleEx(directory, FileIdInfo, &identity, sizeof(identity)));
    return identity;
}
/* The public SDK omits this ntdll entry and its native information class. */
NTSYSAPI NTSTATUS NTAPI NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
enum { FIXTURE_FILE_RENAME_INFORMATION = 10 };

static void fixture_rename_sibling(HANDLE directory, const wchar_t *leaf) {
    size_t units = wcslen(leaf);
    CHECK(units && units <= 255 && !wcspbrk(leaf, L"/\\:"));
    size_t name_bytes = units * sizeof(*leaf);
    /* With a simple name and a null RootDirectory, NT keeps the source parent.
     * An absolute target would reopen that parent without sharing DELETE on
     * Windows Server 2022, conflicting with the workspace's retained pin. */
    size_t bytes = sizeof(FILE_RENAME_INFO) + name_bytes;
    FILE_RENAME_INFO *info = xr_malloc(bytes);
    CHECK(info);
    memset(info, 0, bytes);
    info->FileNameLength = (DWORD)name_bytes;
    memcpy(info->FileName, leaf, name_bytes);
    IO_STATUS_BLOCK io = {0};
    NTSTATUS status = NtSetInformationFile(directory, &io, info, (ULONG)bytes,
        (FILE_INFORMATION_CLASS)FIXTURE_FILE_RENAME_INFORMATION);
    xr_free(info);
    if (status != 0) {
        fprintf(stderr, "fixture sibling rename destination: %ls, NTSTATUS=%08lx\n", leaf, (unsigned long)status);
        SetLastError(RtlNtStatusToDosError(status));
    }
    CHECK(status == 0);
}
static void fixture_delete(HANDLE entry) {
    FILE_DISPOSITION_INFO disposition = {TRUE};
    CHECK(SetFileInformationByHandle(entry, FileDispositionInfo, &disposition, sizeof(disposition)));
    CHECK(CloseHandle(entry));
}
static HANDLE rejected_directory_pin(XtcXirWorkspace **owner, unsigned which) {
    XtcXirWorkspace *workspace = *owner;
    unsigned calls = 0;
    while (!(workspace->close_phase == 2 && workspace->close_index == which && workspace->close_substage == 2)) {
        CHECK(++calls < 1000);
        CHECK(xtc_xir_workspace_close(owner, 1) == XTC_XIR_WORKSPACE_PENDING && *owner == workspace);
    }
    /* The identity check has failed. Keep the exact DELETE handle for fixture
     * retirement while letting the workspace release its own reference. */
    HANDLE process = GetCurrentProcess(), pin = NULL;
    CHECK(DuplicateHandle(process, workspace->nodes[which].deletion, process, &pin, 0, FALSE, DUPLICATE_SAME_ACCESS));
    size_t tracked = owner_handle_count;
    CHECK(xtc_xir_workspace_close(owner, 1) == XTC_XIR_WORKSPACE_IDENTITY_MISMATCH);
    CHECK(*owner == workspace && workspace->close_index == which && workspace->close_substage == 0);
    CHECK(!workspace->nodes[which].deletion && owner_handle_count + 1 == tracked);
    return pin;
}
static const char replacement_contents[] = "replacement preserved\n";
static HANDLE replacement_marker_create(const char *path) {
    HANDLE marker = CreateFileA(path, GENERIC_READ | GENERIC_WRITE | DELETE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(marker != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    CHECK(WriteFile(marker, replacement_contents, sizeof(replacement_contents) - 1, &written, NULL));
    CHECK(written == sizeof(replacement_contents) - 1);
    return marker;
}
static void replacement_marker_verify_and_remove(HANDLE marker, const char *path) {
    CHECK(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
    CHECK(SetFilePointerEx(marker, (LARGE_INTEGER){0}, NULL, FILE_BEGIN));
    char contents[sizeof(replacement_contents)];
    DWORD received = 0;
    CHECK(ReadFile(marker, contents, sizeof(contents), &received, NULL));
    CHECK(received == sizeof(replacement_contents) - 1 && !memcmp(contents, replacement_contents, received));
    fixture_delete(marker);
    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);
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
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    DWORD observe = share | FILE_SHARE_DELETE;
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE original_pin = CreateFileA(original, DELETE | FILE_READ_ATTRIBUTES, share, NULL, OPEN_EXISTING, flags, NULL);
    CHECK(original_pin != INVALID_HANDLE_VALUE);
    FILE_ID_INFO original_id = fixture_identity(original_pin);
    CHECK(workspace_identity_equal(&original_id, &owner->nodes[which].identity));
    const wchar_t *leaf = owner->nodes[which].name;
    wchar_t moved_leaf[64];
    CHECK(swprintf_s(moved_leaf, sizeof(moved_leaf) / sizeof(*moved_leaf), L"%ls-moved", leaf) > 0);
    fixture_rename_sibling(original_pin, moved_leaf);
    CHECK(CreateDirectoryA(original, NULL));
    /* A second DELETE open must fail while the fixture owns the rename right. */
    HANDLE blocker = CreateFileA(moved, DELETE, observe,
        NULL, OPEN_EXISTING, flags, NULL);
    CHECK(blocker == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    path_join(marker, sizeof(marker), original, "keep.txt");
    HANDLE marker_pin = replacement_marker_create(marker);
    HANDLE observer = CreateFileA(original, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, observe, NULL, OPEN_EXISTING, flags, NULL);
    CHECK(observer != INVALID_HANDLE_VALUE);
    FILE_ID_INFO replacement_id = fixture_identity(observer);
    CHECK(!workspace_identity_equal(&original_id, &replacement_id));
    HANDLE rejected_pin = rejected_directory_pin(&owner, which);
    FILE_ID_INFO rejected_id = fixture_identity(rejected_pin);
    CHECK(workspace_identity_equal(&replacement_id, &rejected_id));
    replacement_marker_verify_and_remove(marker_pin, marker);
    /* The production close has refused this exact directory and left its
     * contents intact. Close the observer before deleting the empty fixture. */
    CHECK(CloseHandle(observer));
    fixture_delete(rejected_pin);
    CHECK(GetFileAttributesA(original) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);
    HANDLE parent_pin = owner->nodes[0].deletion;
    CHECK(parent_pin);
    FILE_ID_INFO parent_id = fixture_identity(parent_pin);
    CHECK(workspace_identity_equal(&parent_id, &owner->nodes[0].identity));
    fixture_rename_sibling(original_pin, leaf);
    CHECK(owner->nodes[0].deletion == parent_pin);
    FILE_ID_INFO held_parent_id = fixture_identity(parent_pin);
    CHECK(workspace_identity_equal(&held_parent_id, &parent_id));
    HANDLE restored = CreateFileA(original, FILE_READ_ATTRIBUTES, observe, NULL, OPEN_EXISTING, flags, NULL);
    CHECK(restored != INVALID_HANDLE_VALUE);
    FILE_ID_INFO restored_id = fixture_identity(restored);
    CHECK(workspace_identity_equal(&restored_id, &original_id));
    CHECK(CloseHandle(restored) && CloseHandle(original_pin));
    CHECK(xtc_xir_workspace_status(owner) == (first == XTC_XIR_WORKSPACE_OK ? XTC_XIR_WORKSPACE_IDENTITY_MISMATCH : first));
    drain(&owner, 1); xr_compile_resources_release(resources); CHECK(!runtime_live && !owner_handle_count);
    printf("fixed directory %u replacement preserved through refusal and original identity restored PASS\n", which);
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
    record_physical = true; physical_total = physical_peak = 0; runtime_attempts = 0;
    XrCompileResourceStats stats = run(parent, &sdk_unlimited, XTC_XIR_WORKSPACE_OK, false);
    record_physical = false; size_t allocations = runtime_attempts;
    CHECK(stats.allocated_bytes == physical_total && stats.peak_bytes == physical_peak);
    for (size_t n = 0; n < allocations; ++n) {
        runtime_attempts = 0; runtime_fail_at = n;
        run(parent, &sdk_unlimited, XTC_XIR_WORKSPACE_OUT_OF_MEMORY, false); runtime_fail_at = SIZE_MAX;
    }
    CHECK(stats.peak_bytes > 0 && stats.peak_bytes < UINT64_MAX);
    XrCompileResourceLimits exact = {XR_COMPILE_MEMORY_FINITE, stats.peak_bytes, true};
    run(parent, &exact, XTC_XIR_WORKSPACE_OK, false);
    --exact.memory_limit;
    run(parent, &exact, XTC_XIR_WORKSPACE_MEMORY_LIMIT, false);
    exact.memory_limit += 2;
    run(parent, &exact, XTC_XIR_WORKSPACE_OK, false);
    printf("allocations=%zu real OOM; held peak minus1/exact/plus1; allocated=%llu peak=%llu; physical oracle PASS\n",
        allocations, (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.peak_bytes);
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
    (void)sdk_fixture_malloc; (void)sdk_fixture_free;
    CHECK(argc == 2 || argc == 3); DWORD before = native_random_boundary();
    if (argc == 3) {
#ifndef WORKSPACE_PRODUCTION
        if (!strcmp(argv[2], "--anchor-only")) {
            for (unsigned i = 0; i < 3; ++i) replacement(argv[1], i);
            reparse_refusal(argv[1]); root_reparse_refusal(argv[1]); fixed_identity_retry(argv[1]);
            enumerated_child_open(argv[1], true); enumerated_child_open(argv[1], false);
            enumerated_child_sharing(argv[1], false); enumerated_child_sharing(argv[1], true);
            fixed_child_open_failure(argv[1], ERROR_FILE_NOT_FOUND, XTC_XIR_WORKSPACE_UNRESOLVED);
            fixed_child_open_failure(argv[1], ERROR_SHARING_VIOLATION, XTC_XIR_WORKSPACE_IO);
            CHECK(handle_count() == before && !runtime_live && !runtime_bytes && !owner_handle_count);
            puts("root pin/child rename/replacement/reparse/close-one-step retry physical zero PASS"); return 0;
        }
#endif
        CHECK(!strcmp(argv[2], "--compiler")); compiler_transaction(argv[1]);
        CHECK(handle_count() == before); return 0;
    }
    printf("initial handles=%lu\n", before);
    basics(argv[1]); printf("after basics handles=%lu\n", handle_count());
    busy_regular_file(argv[1]);
    arguments_and_depth(argv[1]); memory_owner_reuse(argv[1]);
    resources_matrix(argv[1]);
    printf("after resources handles=%lu\n", handle_count());
#ifndef WORKSPACE_PRODUCTION
    hardlink(argv[1]); printf("after hardlink handles=%lu\n", handle_count());
    collision(argv[1]);
    for (unsigned i = 0; i < 3; ++i) replacement(argv[1], i);
    reparse_refusal(argv[1]); root_reparse_refusal(argv[1]); fixed_identity_retry(argv[1]);
    enumerated_child_open(argv[1], true); enumerated_child_open(argv[1], false);
    enumerated_child_sharing(argv[1], false); enumerated_child_sharing(argv[1], true);
    fixed_child_open_failure(argv[1], ERROR_FILE_NOT_FOUND, XTC_XIR_WORKSPACE_UNRESOLVED);
    fixed_child_open_failure(argv[1], ERROR_SHARING_VIOLATION, XTC_XIR_WORKSPACE_IO);
    os_matrix(argv[1]); printf("after OS handles=%lu\n", handle_count());
#endif
    DWORD after = handle_count(); printf("final handles=%lu live=%zu bytes=%zu\n", after, runtime_live, runtime_bytes);
    CHECK(before == after && !runtime_live && !runtime_bytes);
    printf("handles before=%lu after=%lu; PASS workspace owner\n", before, after); return 0;
}
