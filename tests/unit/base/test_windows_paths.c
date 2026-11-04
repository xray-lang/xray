/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_windows_paths.c - UTF-8 filesystem and publication boundary checks
 */
#include "base/xfileio.h"
#include "base/xmalloc.h"
#include "os/os_file_read.h"
#include "os/os_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <windows.h>

typedef union PermissionBlock {
    long double alignment;
    void *pointer_alignment;
    size_t bytes;
} PermissionBlock;
_Static_assert(_Alignof(PermissionBlock) >= _Alignof(wchar_t), "policy blocks preserve wide buffer alignment");
typedef struct PermissionMemory {
    size_t attempts, fail_at, cancel_at, live, held;
    bool cancelled;
    XrOsIoStatus failure_status;
} PermissionMemory;

static XrOsIoStatus permission_alloc(void *context, size_t size, void **output) {
    PermissionMemory *memory = context;
    if (!output || *output) return XR_OS_IO_BAD_ARGUMENT;
    size_t attempt = memory->attempts++;
    if (attempt == memory->fail_at) return memory->failure_status;
    if (size > SIZE_MAX - sizeof(PermissionBlock)) return XR_OS_IO_OVERFLOW;
    size_t total = sizeof(PermissionBlock) + size;
    PermissionBlock *block = xr_malloc(total);
    if (!block) return XR_OS_IO_OUT_OF_MEMORY;
    block->bytes = total; ++memory->live; memory->held += total;
    *output = block + 1;
    if (attempt == memory->cancel_at) memory->cancelled = true;
    return XR_OS_IO_OK;
}
static bool permission_current(void *context) {
    return !((PermissionMemory *)context)->cancelled;
}
static void permission_free(void *context, void *data) {
    if (!data) return;
    PermissionMemory *memory = context;
    PermissionBlock *block = (PermissionBlock *)data - 1;
    if (!memory->live || memory->held < block->bytes) abort();
    --memory->live; memory->held -= block->bytes; xr_free(block);
}

/* An independent Win32 lookup checks the intended UTF-8 filename, including
 * a long non-BMP component, rather than trusting the shim's converted path. */
static DWORD permission_attributes(const char *path) {
    wchar_t wide[4096], absolute[4096], extended[4104];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 4096))
        return INVALID_FILE_ATTRIBUTES;
    DWORD length = GetFullPathNameW(wide, 4096, absolute, NULL);
    if (!length || length >= 4096) return INVALID_FILE_ATTRIBUTES;
    bool unc = absolute[0] == L'\\' && absolute[1] == L'\\';
    size_t prefix = unc ? 8u : 4u, skip = unc ? 2u : 0u;
    memcpy(extended, unc ? L"\\\\?\\UNC\\" : L"\\\\?\\", prefix * sizeof(wchar_t));
    memcpy(extended + prefix, absolute + skip, (length - skip + 1) * sizeof(wchar_t));
    return GetFileAttributesW(extended);
}

#define PERMISSION_CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "chmod %d: %s\n", __LINE__, #condition); \
        goto cleanup; \
    } \
} while (0)

static int verify_permissions(const char *root) {
    XrOsIoPolicy system = xr_os_io_system_policy();
    PermissionMemory memory = {.fail_at = SIZE_MAX, .cancel_at = SIZE_MAX, .failure_status = XR_OS_IO_OUT_OF_MEMORY};
    XrOsIoPolicy observed = {&memory, permission_alloc, permission_free};
    char tail[221], path[4096], missing[4096];
    memset(tail, 'p', sizeof(tail) - 1); tail[sizeof(tail) - 1] = 0;
    int length = snprintf(path, sizeof(path), "%s/permission-%s-\xe4\xb8\xad-\xf0\x9f\x98\x80.bin", root, tail);
    if (length <= 0 || (size_t)length >= sizeof(path)) return 0;
    length = snprintf(missing, sizeof(missing), "%s/missing-chmod-\xe6\x96\x87", root);
    if (length <= 0 || (size_t)length >= sizeof(missing)) return 0;
    static const uint8_t payload[] = {'m', 0, 0xff};
    bool created = false, changed = true;
    int passed = 0;
    size_t sites = 0;
    PERMISSION_CHECK(xr_os_io_write_new_file_sync(&system, path, payload, sizeof(payload)) == XR_OS_IO_OK);
    created = true;
    PERMISSION_CHECK(permission_attributes(path) != INVALID_FILE_ATTRIBUTES);
    PERMISSION_CHECK(xr_os_io_chmod(&system, path, _S_IREAD, NULL, &changed) == XR_OS_IO_OK && changed);
    PERMISSION_CHECK(permission_attributes(path) & FILE_ATTRIBUTE_READONLY);
    PERMISSION_CHECK(xr_os_io_chmod(&system, path, _S_IWRITE, NULL, &changed) == XR_OS_IO_OK && changed);
    PERMISSION_CHECK(!(permission_attributes(path) & FILE_ATTRIBUTE_READONLY));
    PERMISSION_CHECK(xr_os_io_chmod(&system, path, _S_IREAD | _S_IWRITE, NULL, &changed) == XR_OS_IO_OK && changed);
    PERMISSION_CHECK(!(permission_attributes(path) & FILE_ATTRIBUTE_READONLY));
    PERMISSION_CHECK(xr_os_io_chmod(&system, path, INT_MAX, NULL, &changed) == XR_OS_IO_OK && changed);
    PERMISSION_CHECK(!(permission_attributes(path) & FILE_ATTRIBUTE_READONLY));
    static const int64_t invalid_modes[] = {-1, INT64_MIN, (int64_t)INT_MAX + 1, INT64_MAX};
    for (size_t i = 0; i < sizeof(invalid_modes) / sizeof(invalid_modes[0]); ++i) {
        size_t before = memory.attempts;
        PERMISSION_CHECK(xr_os_io_chmod(&observed, path, invalid_modes[i], NULL, &changed) == XR_OS_IO_BAD_ARGUMENT);
        PERMISSION_CHECK(changed && memory.attempts == before && !memory.live && !memory.held);
        PERMISSION_CHECK(!(permission_attributes(path) & FILE_ATTRIBUTE_READONLY));
    }
    PERMISSION_CHECK(xr_os_io_chmod(&observed, missing, _S_IREAD, NULL, &changed) == XR_OS_IO_OK && !changed);
    changed = true;
    PERMISSION_CHECK(xr_os_io_chdir(&observed, missing, NULL, &changed) == XR_OS_IO_OK && !changed);
    PERMISSION_CHECK(!memory.live && !memory.held);
    changed = true;
    PERMISSION_CHECK(xr_os_io_chmod(&observed, "\xc0\xaf", _S_IREAD, NULL, &changed) == XR_OS_IO_BAD_ARGUMENT && changed);
    changed = false;
    PERMISSION_CHECK(xr_os_io_chdir(&observed, "\xc0\xaf", NULL, &changed) == XR_OS_IO_BAD_ARGUMENT && !changed);
    PERMISSION_CHECK(!memory.live && !memory.held);
    PERMISSION_CHECK(xr_os_io_chmod(NULL, path, _S_IREAD, NULL, &changed) == XR_OS_IO_BAD_ARGUMENT);
    PERMISSION_CHECK(xr_os_io_chmod(&observed, NULL, _S_IREAD, NULL, &changed) == XR_OS_IO_BAD_ARGUMENT);
    size_t before = memory.attempts;
    PERMISSION_CHECK(xr_os_io_chmod(&observed, path, _S_IREAD, NULL, NULL) == XR_OS_IO_BAD_ARGUMENT);
    PERMISSION_CHECK(xr_os_io_chdir(&observed, root, NULL, NULL) == XR_OS_IO_BAD_ARGUMENT);
    PERMISSION_CHECK(memory.attempts == before && !memory.live && !memory.held);
    char platform_limit[32769];
    memset(platform_limit, 'p', sizeof(platform_limit) - 1); platform_limit[sizeof(platform_limit) - 1] = 0;
    changed = true;
    PERMISSION_CHECK(xr_os_io_chmod(&observed, platform_limit, _S_IREAD, NULL, &changed) == XR_OS_IO_OK && !changed);
    changed = true;
    PERMISSION_CHECK(xr_os_io_chdir(&observed, platform_limit, NULL, &changed) == XR_OS_IO_OK && !changed);
    static const XrOsIoStatus protocol_faults[] = {XR_OS_IO_BAD_ARGUMENT, XR_OS_IO_OVERFLOW};
    for (size_t i = 0; i < sizeof(protocol_faults) / sizeof(protocol_faults[0]); ++i) {
        memory.fail_at = 0; memory.failure_status = protocol_faults[i]; memory.attempts = 0; changed = true;
        PERMISSION_CHECK(xr_os_io_chmod(&observed, path, _S_IREAD, NULL, &changed) == protocol_faults[i]);
        PERMISSION_CHECK(changed && memory.attempts == 1 && !memory.live && !memory.held);
        memory.attempts = 0; changed = false;
        PERMISSION_CHECK(xr_os_io_chdir(&observed, root, NULL, &changed) == protocol_faults[i]);
        PERMISSION_CHECK(!changed && memory.attempts == 1 && !memory.live && !memory.held);
    }
    memory.fail_at = SIZE_MAX; memory.failure_status = XR_OS_IO_OUT_OF_MEMORY;
    changed = true; memory.cancelled = true; memory.attempts = 0;
    PERMISSION_CHECK(xr_os_io_chmod(&observed, path, _S_IREAD, permission_current, &changed) == XR_OS_IO_CANCELLED);
    PERMISSION_CHECK(changed && !memory.attempts && !memory.live && !memory.held);
    PERMISSION_CHECK(!(permission_attributes(path) & FILE_ATTRIBUTE_READONLY));
    changed = false; memory.cancelled = false; memory.cancel_at = 0;
    PERMISSION_CHECK(xr_os_io_chmod(&observed, path, _S_IREAD, permission_current, &changed) == XR_OS_IO_CANCELLED);
    PERMISSION_CHECK(!changed && memory.cancelled && memory.attempts && !memory.live && !memory.held);
    PERMISSION_CHECK(!(permission_attributes(path) & FILE_ATTRIBUTE_READONLY));
    char saved[4096], current[4096];
    PERMISSION_CHECK(xr_os_io_getcwd(&system, saved, sizeof(saved)) == XR_OS_IO_OK && strcmp(saved, root));
    changed = true; memory.attempts = 0;
    PERMISSION_CHECK(xr_os_io_chdir(&observed, root, permission_current, &changed) == XR_OS_IO_CANCELLED);
    PERMISSION_CHECK(changed && !memory.attempts && !memory.live && !memory.held);
    PERMISSION_CHECK(xr_os_io_getcwd(&system, current, sizeof(current)) == XR_OS_IO_OK && !strcmp(current, saved));
    changed = false; memory.cancelled = false;
    PERMISSION_CHECK(xr_os_io_chdir(&observed, root, permission_current, &changed) == XR_OS_IO_CANCELLED);
    PERMISSION_CHECK(!changed && memory.cancelled && memory.attempts && !memory.live && !memory.held);
    PERMISSION_CHECK(xr_os_io_getcwd(&system, current, sizeof(current)) == XR_OS_IO_OK && !strcmp(current, saved));
    memory.cancelled = false; memory.cancel_at = SIZE_MAX;
    memory.attempts = 0;
    PERMISSION_CHECK(xr_os_io_chmod(&observed, path, _S_IREAD, permission_current, &changed) == XR_OS_IO_OK && changed);
    sites = memory.attempts;
    PERMISSION_CHECK(sites && !memory.live && !memory.held);
    for (size_t failure = 0; failure < sites; ++failure) {
        PERMISSION_CHECK(xr_os_io_chmod(&system, path, _S_IWRITE, NULL, &changed) == XR_OS_IO_OK && changed);
        memory.attempts = 0; memory.fail_at = failure; changed = true;
        PERMISSION_CHECK(xr_os_io_chmod(&observed, path, _S_IREAD, NULL, &changed) == XR_OS_IO_OUT_OF_MEMORY);
        PERMISSION_CHECK(changed && memory.attempts == failure + 1 && !memory.live && !memory.held);
        PERMISSION_CHECK(!(permission_attributes(path) & FILE_ATTRIBUTE_READONLY));
    }
    memory.fail_at = SIZE_MAX;
    PERMISSION_CHECK(xr_os_io_chmod(&observed, path, _S_IREAD, NULL, &changed) == XR_OS_IO_OK && changed);
    PERMISSION_CHECK(!memory.live && !memory.held);
    passed = 1;
cleanup:
    if (created) {
        if (xr_os_io_chmod(&system, path, _S_IWRITE, NULL, &changed) != XR_OS_IO_OK || !changed ||
            xr_os_io_remove(&system, path) != XR_OS_IO_OK) passed = 0;
    }
    printf("Unicode chmod: read/write attributes, typed faults versus platform false, prewrite cancellation, OOM sites %zu, owners %zu/%zu\n",
        sites, memory.live, memory.held);
    return passed;
}
static int verify_publication(const char *root) {
    XrOsIoPolicy policy = xr_os_io_system_policy();
    bool changed = false;
    char first[4096], final[4096], other[4096], current[4096], saved[4096];
    if (snprintf(first,sizeof(first),"%s/tmp-\xf0\x9f\x98\x80",root)<=0 ||
        snprintf(final,sizeof(final),"%s/final-\xe4\xb8\xad",root)<=0 ||
        snprintf(other,sizeof(other),"%s/other-\xe6\x96\x87",root)<=0) return 0;
    static const uint8_t data[]={1,2,3,4};
    if (xr_os_io_write_new_file_sync(&policy,first,data,sizeof(data)) != XR_OS_IO_OK ||
        xr_os_io_write_new_file_sync(&policy,first,data,sizeof(data)) != XR_OS_IO_EXISTS) return 0;
    uint8_t *read=NULL;size_t size=0;
    if (xr_os_io_read_regular_file(&policy,first,3,&read,&size) != XR_OS_IO_OVERFLOW || read || size) return 0;
    if (xr_os_io_read_regular_file(&policy,first,4,&read,&size) != XR_OS_IO_OK || size!=4 || memcmp(read,data,4)) return 0;
    xr_free(read);
    if (xr_os_io_rename(&policy,first,final) != XR_OS_IO_OK || xr_os_io_touch(&policy,final) != XR_OS_IO_OK) return 0;
    if (xr_os_io_write_new_file_sync(&policy,other,data,sizeof(data)) != XR_OS_IO_OK) return 0;
    bool exists=false;
    if (xr_os_io_publish_noreplace(&policy,other,final)!=XR_OS_IO_EXISTS ||
        xr_os_io_exists(&policy,other,&exists)!=XR_OS_IO_OK || !exists) return 0;
    if (xr_os_io_publish_noreplace(&policy,other,first)!=XR_OS_IO_OK) return 0;
    if (xr_os_io_getcwd(&policy,saved,sizeof(saved))!=XR_OS_IO_OK || xr_os_io_chdir(&policy,root,NULL, &changed)!=XR_OS_IO_OK || !changed) return 0;
    int same=xr_os_io_getcwd(&policy,current,sizeof(current))==XR_OS_IO_OK && !strcmp(current,root);
    if (xr_os_io_chdir(&policy,saved,NULL, &changed)!=XR_OS_IO_OK || !changed || !same) return 0;
    if (xr_os_io_remove(&policy,first)!=XR_OS_IO_OK || xr_os_io_remove(&policy,final)!=XR_OS_IO_OK) return 0;
    return 1;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    size_t size = 0;
    char *root = xr_file_read_all(argv[1], "rb", &size);
    if (!root || !size) return 3;
    XrOsIoPolicy policy = xr_os_io_system_policy();
    char *resolved = NULL, *path = NULL;
    if (xr_realpath_owned(&policy,root,&resolved)!=XR_OS_IO_OK ||
        xr_path_join_owned(&policy,root,"main.xr",&path)!=XR_OS_IO_OK) return 4;
    char *legacy = path ? xr_file_read_all(path, "rb", NULL) : NULL;
    XrFileBytes bytes = {0};
    XrFileReadStatus status = xr_os_io_read_under_root(&policy,root, "main.xr", 4096, &bytes);
    printf("{\"realpath_preserves_utf8\":%s,\"narrow_read_ok\":%s,\"rooted_read_status\":%d,\"rooted_bytes\":%zu}\n",
        resolved && !strcmp(root, resolved) ? "true" : "false", legacy ? "true" : "false", (int)status, bytes.size);
    int exact = resolved && !strcmp(root,resolved) && legacy && !strcmp(legacy,"print(41)\n") &&
        bytes.size == 10 && !memcmp(bytes.data,"print(41)\n",10);
    int published = verify_publication(root);
    printf("publication_and_cwd=%d\n",published);
    int permissions = verify_permissions(root);
    xr_free(bytes.data); xr_free(legacy); xr_free(path); xr_free(resolved); xr_free(root);
    return status == XR_FILE_READ_OK && published && exact && permissions ? 0 : 1;
}
