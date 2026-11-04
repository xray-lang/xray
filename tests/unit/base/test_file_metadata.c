/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_file_metadata.c - Full metadata, routing links and owned failure cleanup
 */
#include "os/os_file_metadata.h"
#include "base/xmalloc.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef union MetadataBlock { long double alignment; size_t bytes; void *pointer; } MetadataBlock;
typedef struct MetadataMemory {
    size_t attempts, fail_at, cancel_at, live, held;
    size_t polls, cancel_poll, copy_polls;
    bool cancelled, cancel_copy;
    XrOsIoStatus failure;
} MetadataMemory;
static MetadataMemory metadata_memory;
static XrOsIoStatus test_allocate(void *context, size_t bytes, void **output) {
    MetadataMemory *state = context;
    if (!output || *output || bytes > SIZE_MAX - sizeof(MetadataBlock)) return XR_OS_IO_BAD_ARGUMENT;
    size_t attempt = state->attempts++;
    if (attempt == state->fail_at) return state->failure;
    MetadataBlock *block = xr_malloc(sizeof(*block) + bytes);
    if (!block) return XR_OS_IO_OUT_OF_MEMORY;
    block->bytes = bytes; ++state->live; state->held += bytes;
    *output = block + 1;
    if (attempt == state->cancel_at) state->cancelled = true;
    return XR_OS_IO_OK;
}
static void test_free(void *context, void *data) {
    if (!data) return;
    MetadataMemory *state = context;
    MetadataBlock *block = (MetadataBlock *)data - 1;
    if (!state->live || state->held < block->bytes) abort();
    --state->live; state->held -= block->bytes; xr_free(block);
}
static bool test_current(void *context) {
    MetadataMemory *state = context;
    if (++state->polls == state->cancel_poll) state->cancelled = true;
    if (state->cancel_copy && state->live && ++state->copy_polls == 2) state->cancelled = true;
    return !state->cancelled;
}
static XrOsIoPolicy test_policy(void) { return (XrOsIoPolicy){&metadata_memory, test_allocate, test_free}; }
static void reset_memory(void) {
    if (metadata_memory.live || metadata_memory.held) abort();
    metadata_memory = (MetadataMemory){.fail_at = SIZE_MAX, .cancel_at = SIZE_MAX,
        .cancel_poll = SIZE_MAX, .failure = XR_OS_IO_OUT_OF_MEMORY};
}
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "file metadata %d: %s\n", __LINE__, #condition); return 0; \
} } while (0)

#if defined(XR_OS_WINDOWS)
#include "base/xwindows_utf8.h"
#include <winioctl.h>
static size_t kernel_handles, kernel_opens, kernel_closes, close_failures, kernel_attributes;
static bool cancel_after_open, information_failure;
static HANDLE WINAPI observed_create(LPCWSTR name, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
    HANDLE handle = CreateFileW(name, access, sharing, security, creation, flags, template_file);
    if (handle != INVALID_HANDLE_VALUE) {
        ++kernel_handles; ++kernel_opens;
        if (cancel_after_open) metadata_memory.cancelled = true;
    }
    return handle;
}
static BOOL WINAPI observed_close(HANDLE handle) {
    ++kernel_closes;
    if (close_failures) { --close_failures; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    BOOL closed = CloseHandle(handle);
    if (closed) { if (!kernel_handles) abort(); --kernel_handles; }
    return closed;
}
static BOOL WINAPI observed_information(HANDLE handle, LPBY_HANDLE_FILE_INFORMATION output) {
    if (information_failure) { SetLastError(ERROR_SUCCESS); return FALSE; }
    return GetFileInformationByHandle(handle, output);
}
static DWORD WINAPI observed_attributes(LPCWSTR path) {
    ++kernel_attributes;
    return GetFileAttributesW(path);
}
#define CreateFileW observed_create
#define CloseHandle observed_close
#define GetFileInformationByHandle observed_information
#define GetFileAttributesW observed_attributes
#include "os/win/file_metadata_win.c"
#undef GetFileAttributesW
#undef GetFileInformationByHandle
#undef CloseHandle
#undef CreateFileW

typedef struct MetadataPaths {
    wchar_t root[32768], outside[32768], child[32768], file[32768], extended[32768];
    wchar_t junction[32768], marker[32768];
    char utf8[65536];
    bool root_created, outside_created, child_created, file_created, junction_created, marker_created;
} MetadataPaths;
static MetadataPaths paths;
static int wide_join(wchar_t *output, const wchar_t *root, const wchar_t *tail) {
    size_t length = wcslen(root);
    const wchar_t *separator = length && (root[length - 1] == L'\\' || root[length - 1] == L'/') ? L"" : L"\\";
    int count = swprintf(output, 32768, L"%ls%ls%ls", root, separator, tail);
    return count > 0 && count < 32768;
}
static int directory_operation(const wchar_t *path, bool create) {
    wchar_t locator[32768];
    int count = swprintf(locator, 32768, L"\\\\?\\%ls", path);
    if (count <= 0 || count >= 32768) return 0;
    bool changed = create ? CreateDirectoryW(locator, NULL) != 0 : RemoveDirectoryW(locator) != 0;
    if (!changed) fwprintf(stderr, L"metadata directory error %lu: %ls\n", (unsigned long)GetLastError(), locator);
    return changed;
}
static int write_marker(const wchar_t *path, const uint8_t *bytes, DWORD length) {
    HANDLE handle = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    bool okay = WriteFile(handle, bytes, length, &written, NULL) && written == length;
    return CloseHandle(handle) && okay;
}
static int make_junction(const wchar_t *link, const wchar_t *target) {
    struct JunctionData {
        DWORD tag;
        WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        WCHAR path[16384];
    } data = {0};
    size_t target_length = wcslen(target);
    CHECK(target_length + 4 + 1 + target_length + 1 <= 16384);
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    memcpy(data.path, L"\\??\\", 4 * sizeof(WCHAR));
    memcpy(data.path + 4, target, (target_length + 1) * sizeof(WCHAR));
    data.substitute_length = (WORD)((target_length + 4) * sizeof(WCHAR));
    data.print_offset = data.substitute_length + (WORD)sizeof(WCHAR);
    data.print_length = (WORD)(target_length * sizeof(WCHAR));
    memcpy(data.path + target_length + 5, target, (target_length + 1) * sizeof(WCHAR));
    data.length = (WORD)(8 + (target_length * 2 + 6) * sizeof(WCHAR));
    HANDLE handle = CreateFileW(link, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    bool assigned = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        (DWORD)data.length + 8, NULL, 0, &returned, NULL) != 0;
    bool closed = CloseHandle(handle) != 0;
    CHECK(assigned && closed);
    return 1;
}
static int setup_paths(void) {
    wchar_t temporary[32768], suffix[128], child[101], name[205];
    DWORD length = GetTempPathW(32768, temporary);
    CHECK(length && length < 32768);
    int count = swprintf(suffix, 128, L"xray-metadata-%lu-\x4e2d-\xd83d\xde00", (unsigned long)GetCurrentProcessId());
    CHECK(count > 0 && count < 128 && wide_join(paths.root, temporary, suffix));
    CHECK(directory_operation(paths.root, true)); paths.root_created = true;
    count = swprintf(suffix, 128, L"xray-metadata-protected-%lu", (unsigned long)GetCurrentProcessId());
    CHECK(count > 0 && count < 128 && wide_join(paths.outside, temporary, suffix));
    CHECK(directory_operation(paths.outside, true)); paths.outside_created = true;
    CHECK(wide_join(paths.marker, paths.outside, L"protected.bin"));
    static const uint8_t bytes[] = {'m', 0, 0xff, 'e', 't', 'a'};
    CHECK(write_marker(paths.marker, bytes, sizeof(bytes))); paths.marker_created = true;
    for (size_t i = 0; i < 100; ++i) child[i] = L'd'; child[100] = 0;
    CHECK(wide_join(paths.child, paths.root, child));
    CHECK(directory_operation(paths.child, true)); paths.child_created = true;
    for (size_t i = 0; i < 200; ++i) name[i] = L'f';
    memcpy(name + 200, L".bin", 5 * sizeof(wchar_t));
    CHECK(wide_join(paths.file, paths.child, name) && wcslen(paths.file) > 260);
    count = swprintf(paths.extended, 32768, L"\\\\?\\%ls", paths.file);
    CHECK(count > 0 && count < 32768);
    CHECK(write_marker(paths.extended, bytes, sizeof(bytes))); paths.file_created = true;
    int converted = xr_win_utf16_to_utf8_required(paths.file, wcslen(paths.file));
    CHECK(converted > 0 && converted <= (int)sizeof(paths.utf8));
    CHECK(xr_win_utf16_to_utf8(paths.file, wcslen(paths.file), paths.utf8, sizeof(paths.utf8)));
    CHECK(wide_join(paths.junction, paths.root, L"route-outside"));
    CHECK(directory_operation(paths.junction, true)); paths.junction_created = true;
    CHECK(make_junction(paths.junction, paths.outside));
    return 1;
}
static FILETIME known_time(uint64_t seconds) {
    uint64_t ticks = (seconds + UINT64_C(11644473600)) * UINT64_C(10000000);
    return (FILETIME){(DWORD)ticks, (DWORD)(ticks >> 32)};
}
static int verify_fields(void) {
    HANDLE file = CreateFileW(paths.extended, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    FILETIME created = known_time(1500000001), accessed = known_time(1500000002), written = known_time(1500000003);
    bool set = SetFileTime(file, &created, &accessed, &written) != 0;
    CHECK(CloseHandle(file) && set);
    XrOsIoPolicy policy = test_policy();
    XrOsFileMetadataOwner owner = {0};
    XrOsFileMetadata result;
    reset_memory();
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), test_current, &owner, &result) == XR_OS_IO_OK);
    CHECK(result.present && result.size == 6 && result.is_file && !result.is_dir && !result.is_symlink &&
        !result.uid.present && !result.gid.present && result.mtime == 1500000003 &&
        result.atime == 1500000002 && result.ctime == 1500000001 && result.mode == (_S_IREAD | _S_IWRITE));
    CHECK(!metadata_memory.live && !metadata_memory.held && !kernel_handles && !owner.handle);
    char span[65536]; size_t length = strlen(paths.utf8);
    CHECK(length + 1 < sizeof(span)); memcpy(span, paths.utf8, length);
    span[length] = 'X'; span[length + 1] = 0;
    CHECK(xr_os_io_file_metadata(&policy, span, length, NULL, &owner, &result) == XR_OS_IO_OK &&
        result.present && result.size == 6 && !metadata_memory.live && !kernel_handles);
    CHECK(SetFileAttributesW(paths.extended, FILE_ATTRIBUTE_READONLY));
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), NULL, &owner, &result) == XR_OS_IO_OK &&
        result.present && result.mode == _S_IREAD && result.size == 6);
    CHECK(SetFileAttributesW(paths.extended, FILE_ATTRIBUTE_NORMAL));
    char route[65536];
    CHECK(xr_win_utf16_to_utf8(paths.junction, wcslen(paths.junction), route, sizeof(route)));
    size_t opened = kernel_opens, closed = kernel_closes;
    CHECK(xr_os_io_file_metadata(&policy, route, strlen(route), test_current, &owner, &result) == XR_OS_IO_OK &&
        result.present && result.is_dir && !result.is_file && result.is_symlink &&
        kernel_opens == opened + 2 && kernel_closes == closed + 2 && !kernel_handles);
    HANDLE protected_file = CreateFileW(paths.marker, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(protected_file != INVALID_HANDLE_VALUE);
    uint8_t bytes[6] = {0}; DWORD read = 0;
    bool unchanged = ReadFile(protected_file, bytes, sizeof(bytes), &read, NULL) && read == sizeof(bytes) &&
        !memcmp(bytes, "m\0\xff" "eta", sizeof(bytes));
    CHECK(CloseHandle(protected_file) && unchanged);
    puts("metadata: full 10 fields, epoch seconds, Unicode long path, readonly and real external junction protected");
    return 1;
}
static int verify_failures(void) {
    XrOsIoPolicy policy = test_policy();
    XrOsFileMetadataOwner owner = {0};
    XrOsFileMetadata output, sentinel;
    memset(&sentinel, 0x5a, sizeof(sentinel)); memcpy(&output, &sentinel, sizeof(output));
    reset_memory();
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), test_current, &owner, &output) == XR_OS_IO_OK);
    size_t sites = metadata_memory.attempts;
    CHECK(sites && !metadata_memory.live && !kernel_handles);
    for (size_t point = 0; point < sites; ++point) {
        reset_memory(); metadata_memory.fail_at = point; memcpy(&output, &sentinel, sizeof(output));
        CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), test_current, &owner, &output) ==
            XR_OS_IO_OUT_OF_MEMORY && !memcmp(&output, &sentinel, sizeof(output)) &&
            !metadata_memory.live && !metadata_memory.held && !kernel_handles && !owner.handle);
    }
    reset_memory(); metadata_memory.fail_at = 0; metadata_memory.failure = XR_OS_IO_MEMORY_LIMIT; memcpy(&output, &sentinel, sizeof(output));
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), NULL, &owner, &output) ==
        XR_OS_IO_MEMORY_LIMIT && !memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.live);
    const XrOsIoStatus policy_errors[] = {XR_OS_IO_BAD_ARGUMENT, XR_OS_IO_OVERFLOW};
    for (size_t i = 0; i < sizeof(policy_errors) / sizeof(policy_errors[0]); ++i) {
        reset_memory(); metadata_memory.fail_at = sites - 1; metadata_memory.failure = policy_errors[i];
        CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), NULL, &owner, &output) ==
            policy_errors[i] && !memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.live && !kernel_handles);
    }
    reset_memory(); metadata_memory.fail_at = 0; metadata_memory.failure = XR_OS_IO_OK;
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), NULL, &owner, &output) ==
        XR_OS_IO_BAD_ARGUMENT && !memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.live);
    reset_memory(); metadata_memory.cancelled = true; memcpy(&output, &sentinel, sizeof(output));
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), test_current, &owner, &output) ==
        XR_OS_IO_CANCELLED && !memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.attempts);
    reset_memory(); metadata_memory.cancel_at = 0;
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), test_current, &owner, &output) ==
        XR_OS_IO_CANCELLED && !memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.live && !kernel_handles);
    reset_memory(); cancel_after_open = true;
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), test_current, &owner, &output) ==
        XR_OS_IO_CANCELLED && !memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.live && !kernel_handles);
    cancel_after_open = false;
    reset_memory(); information_failure = true;
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), NULL, &owner, &output) ==
        XR_OS_IO_IO && !memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.live && !kernel_handles);
    information_failure = false;
    reset_memory(); close_failures = 1;
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), NULL, &owner, &output) ==
        XR_OS_IO_IO && !memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.live && kernel_handles == 1 && owner.handle);
    intptr_t saved = owner.handle; size_t closed = kernel_closes;
    CHECK(xr_os_io_file_metadata(&policy, paths.utf8, strlen(paths.utf8), NULL, &owner, &output) ==
        XR_OS_IO_BAD_ARGUMENT && owner.handle == saved && kernel_closes == closed);
    CHECK(xr_os_io_file_metadata_close(&owner) == XR_OS_IO_OK && !owner.handle && !kernel_handles &&
        kernel_closes == closed + 1);
    CHECK(xr_os_io_file_metadata_close(&owner) == XR_OS_IO_OK && kernel_closes == closed + 1);
    reset_memory();
    const char nul[] = {'x', 0, 'z'};
    CHECK(xr_os_io_file_metadata(&policy, nul, sizeof(nul), NULL, &owner, &output) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_os_io_file_metadata(&policy, "\xc0\x80", 2, NULL, &owner, &output) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_os_io_file_metadata(&policy, "x", (size_t)INT_MAX + 1, NULL, &owner, &output) == XR_OS_IO_OVERFLOW);
    CHECK(!memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.attempts && !metadata_memory.live && !kernel_handles);
    static char large_span[131072];
    memset(large_span, 'p', sizeof(large_span));
    size_t attributes = kernel_attributes, opened = kernel_opens;
    reset_memory(); metadata_memory.cancel_poll = 3;
    CHECK(xr_os_io_file_metadata(&policy, large_span, sizeof(large_span), test_current, &owner, &output) ==
        XR_OS_IO_CANCELLED && !memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.attempts &&
        !owner.handle && !kernel_handles && kernel_attributes == attributes && kernel_opens == opened);
    reset_memory(); metadata_memory.cancel_copy = true;
    CHECK(xr_os_io_file_metadata(&policy, large_span, sizeof(large_span), test_current, &owner, &output) ==
        XR_OS_IO_CANCELLED && !memcmp(&output, &sentinel, sizeof(output)) && metadata_memory.attempts == 1 &&
        metadata_memory.copy_polls == 2 && !metadata_memory.live && !metadata_memory.held && !owner.handle &&
        !kernel_handles && kernel_attributes == attributes && kernel_opens == opened);
    reset_memory(); memset(large_span + sizeof(large_span) - 1, 0xff, 1);
    CHECK(xr_os_io_file_metadata(&policy, large_span, sizeof(large_span), test_current, &owner, &output) ==
        XR_OS_IO_BAD_ARGUMENT && !memcmp(&output, &sentinel, sizeof(output)) && !metadata_memory.attempts &&
        !owner.handle && kernel_attributes == attributes && kernel_opens == opened);
    reset_memory();
    large_span[sizeof(large_span) - 1] = 'p';
    CHECK(xr_os_io_file_metadata(&policy, large_span, sizeof(large_span), test_current, &owner, &output) ==
        XR_OS_IO_OK && !output.present && !metadata_memory.live && !metadata_memory.held && !owner.handle &&
        kernel_attributes == attributes && kernel_opens == opened);
    reset_memory();
    CHECK(xr_os_io_file_metadata(&policy, "", 0, NULL, &owner, &output) == XR_OS_IO_OK && !output.present);
    wchar_t missing_wide[32768]; char missing[65536];
    CHECK(wide_join(missing_wide, paths.root, L"missing-entry"));
    CHECK(xr_win_utf16_to_utf8(missing_wide, wcslen(missing_wide), missing, sizeof(missing)));
    CHECK(xr_os_io_file_metadata(&policy, missing, strlen(missing), NULL, &owner, &output) ==
        XR_OS_IO_OK && !output.present);
    CHECK(!metadata_memory.live && !metadata_memory.held && !kernel_handles);
    printf("metadata: %zu observed allocation failures, limit, cancel, protocol and retained-close cleanup\n", sites);
    return 1;
}
static void cleanup_paths(void) {
    close_failures = 0;
    if (paths.file_created) { (void)SetFileAttributesW(paths.extended, FILE_ATTRIBUTE_NORMAL); (void)DeleteFileW(paths.extended); }
    if (paths.junction_created) (void)directory_operation(paths.junction, false);
    if (paths.child_created) (void)directory_operation(paths.child, false);
    if (paths.root_created) (void)directory_operation(paths.root, false);
    if (paths.marker_created) (void)DeleteFileW(paths.marker);
    if (paths.outside_created) (void)directory_operation(paths.outside, false);
}
int main(void) {
    reset_memory();
    int passed = setup_paths() && verify_fields() && verify_failures();
    cleanup_paths();
    if (metadata_memory.live || metadata_memory.held || kernel_handles) return 1;
    return passed ? 0 : 1;
}
#else
#include "os/unix/file_metadata_unix.c"
#include <fcntl.h>
int main(void) {
    char root[] = "/tmp/xray-file-metadata-XXXXXX", file[256], link[256];
    if (!mkdtemp(root)) return 1;
    int length = snprintf(file, sizeof(file), "%s/\xe4\xb8\xad.bin", root);
    if (length < 0 || (size_t)length >= sizeof(file)) return 1;
    int descriptor = open(file, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (descriptor < 0) return 1;
    static const uint8_t bytes[] = {0, 0xff, 42};
    bool written = write(descriptor, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes);
    if (close(descriptor) || !written) return 1;
    length = snprintf(link, sizeof(link), "%s/link", root);
    if (length < 0 || (size_t)length >= sizeof(link) || symlink(file, link)) return 1;
    reset_memory(); XrOsIoPolicy policy = test_policy();
    XrOsFileMetadataOwner owner = {0}; XrOsFileMetadata output;
    struct stat reference;
    int passed = !stat(file, &reference) && xr_os_io_file_metadata(&policy, link, strlen(link),
        test_current, &owner, &output) == XR_OS_IO_OK && output.present && output.size == (int64_t)sizeof(bytes) &&
        output.uid.present && output.uid.value == (int64_t)reference.st_uid &&
        output.gid.present && output.gid.value == (int64_t)reference.st_gid && output.mode == 0600 &&
        output.mtime == reference.st_mtime && output.atime == reference.st_atime && output.ctime == reference.st_ctime &&
        output.is_file && !output.is_dir && output.is_symlink && !owner.handle && !metadata_memory.live && !metadata_memory.held;
    if (unlink(link) || unlink(file) || rmdir(root)) passed = 0;
    return passed ? 0 : 1;
}
#endif
