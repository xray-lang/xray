/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_file_mutation.c - Controlled path writes using real owned file handles
 */
/* Reuse the exercised policy, Unicode paths and real junction fixture. Its
 * original driver remains compiled but is not executed by this focused gate. */
#define main metadata_fixture_main
#include "test_file_metadata.c"
#undef main

#if defined(XR_OS_WINDOWS)
static size_t mutation_basic_writes, mutation_delete_requests, mutation_time_writes;
static bool cancel_after_basic, fail_time_protocol;
static BOOL WINAPI mutation_set_information(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind,
    LPVOID data, DWORD bytes) {
    if (kind == FileBasicInfo) ++mutation_basic_writes;
    if (kind == FileDispositionInfo) ++mutation_delete_requests;
    BOOL changed = SetFileInformationByHandle(handle, kind, data, bytes);
    if (changed && kind == FileBasicInfo && cancel_after_basic) metadata_memory.cancelled = true;
    return changed;
}
static BOOL WINAPI mutation_set_time(HANDLE handle, const FILETIME *created,
    const FILETIME *accessed, const FILETIME *written) {
    ++mutation_time_writes;
    if (fail_time_protocol) { SetLastError(ERROR_SUCCESS); return FALSE; }
    return SetFileTime(handle, created, accessed, written);
}
#define CreateFileW observed_create
#define GetFileAttributesW observed_attributes
#define SetFileInformationByHandle mutation_set_information
#define SetFileTime mutation_set_time
#include "os/win/file_mutation_win.c"
#undef SetFileTime
#undef SetFileInformationByHandle
#undef GetFileAttributesW
#undef CreateFileW

static XrOsPathSpan mutation_span(const char *path) { return (XrOsPathSpan){path, strlen(path)}; }
static int mutation_wide_span(const wchar_t *wide, char *bytes, size_t capacity) {
    return xr_win_utf16_to_utf8(wide, wcslen(wide), bytes, capacity);
}
static int mutation_file_times(const wchar_t *wide, bool set_old, BY_HANDLE_FILE_INFORMATION *output) {
    wchar_t locator[32768];
    int count = swprintf(locator, 32768, L"\\\\?\\%ls", wide);
    CHECK(count > 0 && count < 32768);
    HANDLE handle = CreateFileW(locator, FILE_READ_ATTRIBUTES | (set_old ? FILE_WRITE_ATTRIBUTES : 0),
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    FILETIME old = known_time(946684800);
    bool changed = !set_old || SetFileTime(handle, NULL, &old, &old);
    bool read = changed && GetFileInformationByHandle(handle, output);
    bool closed = CloseHandle(handle) != 0;
    CHECK(read && closed);
    return 1;
}
static int mutation_directories_and_rename(void) {
    XrOsIoPolicy policy = test_policy(); bool changed = false;
    wchar_t directory[32768], renamed[32768], renamed_locator[32768];
    char directory_utf8[65536], renamed_utf8[65536], protected_utf8[65536];
    CHECK(wide_join(directory, paths.child, L"new-\x6587-\xd83d\xde00"));
    CHECK(wide_join(renamed, paths.child, L"renamed-\x6587.bin"));
    CHECK(mutation_wide_span(directory, directory_utf8, sizeof(directory_utf8)));
    CHECK(mutation_wide_span(renamed, renamed_utf8, sizeof(renamed_utf8)));
    CHECK(mutation_wide_span(paths.marker, protected_utf8, sizeof(protected_utf8)));
    int count = swprintf(renamed_locator, 32768, L"\\\\?\\%ls", renamed);
    CHECK(count > 0 && count < 32768);
    reset_memory();
    CHECK(xr_os_io_path_mkdir(&policy, mutation_span(directory_utf8), test_current, &changed) == XR_OS_IO_OK && changed);
    changed = false;
    CHECK(xr_os_io_path_mkdir(&policy, mutation_span(directory_utf8), NULL, &changed) == XR_OS_IO_OK && changed);
    CHECK(xr_os_io_path_mkdir(&policy, mutation_span(paths.utf8), NULL, &changed) == XR_OS_IO_OK && !changed);
    CHECK(xr_os_io_path_rmdir(&policy, mutation_span(directory_utf8), test_current, &changed) == XR_OS_IO_OK && changed);
    CHECK(xr_os_io_path_rmdir(&policy, mutation_span(directory_utf8), NULL, &changed) == XR_OS_IO_OK && !changed);
    CHECK(xr_os_io_path_rename(&policy, mutation_span(paths.utf8), mutation_span(protected_utf8),
        test_current, &changed) == XR_OS_IO_OK && !changed);
    CHECK(GetFileAttributesW(paths.extended) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(paths.marker) != INVALID_FILE_ATTRIBUTES);
    CHECK(xr_os_io_path_rename(&policy, mutation_span(paths.utf8), mutation_span(renamed_utf8),
        test_current, &changed) == XR_OS_IO_OK && changed);
    CHECK(GetFileAttributesW(paths.extended) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(renamed_locator) != INVALID_FILE_ATTRIBUTES);
    CHECK(xr_os_io_path_rename(&policy, mutation_span(renamed_utf8), mutation_span(paths.utf8),
        NULL, &changed) == XR_OS_IO_OK && changed);
    const char bad[] = {'x', 0, 'z'};
    changed = true;
    CHECK(xr_os_io_path_rename(&policy, mutation_span(paths.utf8), (XrOsPathSpan){bad, sizeof(bad)},
        NULL, &changed) == XR_OS_IO_BAD_ARGUMENT && changed && !metadata_memory.live && !kernel_handles);
    CHECK(GetFileAttributesW(paths.extended) != INVALID_FILE_ATTRIBUTES);
    puts("file mutation: Unicode long paths, existing mkdir, empty rmdir, no-replace rename and complete second span");
    return 1;
}
static int mutation_timestamps_and_faults(void) {
    XrOsIoPolicy policy = test_policy(); XrOsFileMetadataOwner owner = {0};
    bool changed = false; BY_HANDLE_FILE_INFORMATION before, after;
    reset_memory();
    CHECK(mutation_file_times(paths.file, true, &before));
    CHECK(xr_os_io_path_utime_now(&policy, mutation_span(paths.utf8), test_current, &owner, &changed) == XR_OS_IO_OK && changed);
    size_t sites = metadata_memory.attempts;
    CHECK(mutation_file_times(paths.file, false, &after));
    CHECK(CompareFileTime(&before.ftLastAccessTime, &after.ftLastAccessTime) != 0 &&
        CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0 &&
        CompareFileTime(&before.ftCreationTime, &after.ftCreationTime) == 0 && after.nFileSizeLow == 6);
    char directory[65536]; CHECK(mutation_wide_span(paths.child, directory, sizeof(directory)));
    CHECK(mutation_file_times(paths.child, true, &before));
    CHECK(xr_os_io_path_utime_now(&policy, mutation_span(directory), NULL, &owner, &changed) == XR_OS_IO_OK && changed);
    CHECK(mutation_file_times(paths.child, false, &after));
    CHECK(CompareFileTime(&before.ftLastAccessTime, &after.ftLastAccessTime) != 0 &&
        CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0);
    size_t writes = mutation_time_writes;
    for (size_t point = 0; point < sites; ++point) {
        reset_memory(); metadata_memory.fail_at = point; changed = false;
        CHECK(xr_os_io_path_utime_now(&policy, mutation_span(paths.utf8), NULL, &owner, &changed) ==
            XR_OS_IO_OUT_OF_MEMORY && !changed && !owner.handle && !metadata_memory.live &&
            !metadata_memory.held && !kernel_handles && mutation_time_writes == writes);
    }
    reset_memory(); metadata_memory.cancel_at = sites - 1; changed = false;
    CHECK(xr_os_io_path_utime_now(&policy, mutation_span(paths.utf8), test_current, &owner, &changed) ==
        XR_OS_IO_CANCELLED && !changed && !owner.handle && !metadata_memory.live && !kernel_handles && mutation_time_writes == writes);
    reset_memory(); cancel_after_open = true;
    CHECK(xr_os_io_path_utime_now(&policy, mutation_span(paths.utf8), test_current, &owner, &changed) ==
        XR_OS_IO_CANCELLED && !changed && !owner.handle && !metadata_memory.live && !kernel_handles && mutation_time_writes == writes);
    cancel_after_open = false;
    reset_memory(); fail_time_protocol = true;
    CHECK(xr_os_io_path_utime_now(&policy, mutation_span(paths.utf8), NULL, &owner, &changed) ==
        XR_OS_IO_IO && !changed && !owner.handle && !kernel_handles);
    fail_time_protocol = false;
    reset_memory(); close_failures = 1; writes = mutation_time_writes;
    CHECK(xr_os_io_path_utime_now(&policy, mutation_span(paths.utf8), NULL, &owner, &changed) ==
        XR_OS_IO_IO && !changed && owner.handle && kernel_handles == 1 && mutation_time_writes == writes + 1);
    CHECK(xr_os_io_file_metadata_close(&owner) == XR_OS_IO_OK && !owner.handle && !kernel_handles &&
        mutation_time_writes == writes + 1);
    puts("file mutation: atime+mtime update-only including directories, OOM/cancel, protocol and no replay on close retry");
    return 1;
}
static int mutation_remove_leaf(void) {
    XrOsIoPolicy policy = test_policy(); XrOsFileMetadataOwner owner = {0}; bool changed = true;
    char root[65536], route[65536];
    CHECK(mutation_wide_span(paths.root, root, sizeof(root)) && mutation_wide_span(paths.junction, route, sizeof(route)));
    reset_memory();
    CHECK(xr_os_io_path_remove(&policy, mutation_span(root), NULL, &owner, &changed) == XR_OS_IO_OK && !changed);
    CHECK(xr_os_io_path_remove(&policy, mutation_span(route), test_current, &owner, &changed) == XR_OS_IO_OK && changed);
    paths.junction_created = false;
    CHECK(GetFileAttributesW(paths.junction) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(paths.marker) != INVALID_FILE_ATTRIBUTES);
    HANDLE marker = CreateFileW(paths.marker, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(marker != INVALID_HANDLE_VALUE); uint8_t bytes[6]; DWORD read = 0;
    bool intact = ReadFile(marker, bytes, sizeof(bytes), &read, NULL) && read == sizeof(bytes) &&
        !memcmp(bytes, "m\0\xff" "eta", sizeof(bytes));
    CHECK(CloseHandle(marker) && intact);
    CHECK(SetFileAttributesW(paths.extended, FILE_ATTRIBUTE_READONLY));
    reset_memory(); cancel_after_basic = true; changed = false;
    size_t deletes = mutation_delete_requests, basics = mutation_basic_writes;
    CHECK(xr_os_io_path_remove(&policy, mutation_span(paths.utf8), test_current, &owner, &changed) == XR_OS_IO_CANCELLED &&
        !changed && !owner.handle && !kernel_handles && mutation_delete_requests == deletes && mutation_basic_writes == basics + 1);
    cancel_after_basic = false;
    DWORD attributes = GetFileAttributesW(paths.extended);
    CHECK(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_READONLY));
    CHECK(SetFileAttributesW(paths.extended, FILE_ATTRIBUTE_READONLY));
    reset_memory(); close_failures = 1; changed = false;
    CHECK(xr_os_io_path_remove(&policy, mutation_span(paths.utf8), NULL, &owner, &changed) == XR_OS_IO_IO &&
        !changed && owner.handle && kernel_handles == 1 && mutation_delete_requests == deletes + 1);
    CHECK(xr_os_io_file_metadata_close(&owner) == XR_OS_IO_OK && !owner.handle && !kernel_handles &&
        mutation_delete_requests == deletes + 1);
    CHECK(GetFileAttributesW(paths.extended) == INVALID_FILE_ATTRIBUTES);
    paths.file_created = false;
    changed = true;
    CHECK(xr_os_io_path_remove(&policy, mutation_span(paths.utf8), NULL, &owner, &changed) == XR_OS_IO_OK && !changed);
    puts("file mutation: readonly leaf removal, real junction target protected, partial-clear cancellation and delete-pending close retry");
    return 1;
}
int main(void) {
    reset_memory();
    int passed = setup_paths() && mutation_directories_and_rename() && mutation_timestamps_and_faults() && mutation_remove_leaf();
    cleanup_paths();
    if (metadata_memory.live || metadata_memory.held || kernel_handles) return 1;
    return passed ? 0 : 1;
}
#else
#include "os/unix/file_mutation_unix.c"
int main(void) {
    char root[] = "/tmp/xray-file-mutation-XXXXXX", path[256], renamed[256];
    if (!mkdtemp(root)) return 1;
    int count = snprintf(path, sizeof(path), "%s/\xe4\xb8\xad", root);
    if (count < 0 || (size_t)count >= sizeof(path)) return 1;
    count = snprintf(renamed, sizeof(renamed), "%s/renamed", root);
    if (count < 0 || (size_t)count >= sizeof(renamed)) return 1;
    reset_memory(); XrOsIoPolicy policy = test_policy(); XrOsFileMetadataOwner owner = {0}; bool changed = false;
    XrOsPathSpan from = {path, strlen(path)}, to = {renamed, strlen(renamed)};
    int passed = xr_os_io_path_mkdir(&policy, from, test_current, &changed) == XR_OS_IO_OK && changed &&
        xr_os_io_path_mkdir(&policy, from, NULL, &changed) == XR_OS_IO_OK && changed &&
        xr_os_io_path_utime_now(&policy, from, test_current, &owner, &changed) == XR_OS_IO_OK && changed &&
        xr_os_io_path_rename(&policy, from, to, NULL, &changed) == XR_OS_IO_OK && changed &&
        xr_os_io_path_rmdir(&policy, to, NULL, &changed) == XR_OS_IO_OK && changed &&
        xr_os_io_path_remove(&policy, to, NULL, &owner, &changed) == XR_OS_IO_OK && !changed &&
        !owner.handle && !metadata_memory.live && !metadata_memory.held;
    (void)rmdir(path); (void)rmdir(renamed); if (rmdir(root)) passed = 0;
    return passed ? 0 : 1;
}
#endif
