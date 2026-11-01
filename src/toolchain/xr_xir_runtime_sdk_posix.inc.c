/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_runtime_sdk_posix.inc.c - Descriptor-walked bundle files and digests
 *
 * KEY CONCEPT:
 *   Every published path is reopened component by component from "/" with
 *   O_NOFOLLOW, so no symbolic link can redirect it, and its bytes must equal
 *   the manifest digest. POSIX has no share modes that would pin hundreds of
 *   files against writers, so descriptors are released after verification and
 *   the installed bundle is trusted not to change until the linker exits.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#define SDK_READ_CAPACITY 65536u

static bool sdk_posix_error(XrXirRuntimeSdk *sdk, int error) {
    return sdk_fail(sdk, error == ENOMEM ? XR_XIR_SDK_OUT_OF_MEMORY :
        error == ENOENT || error == ENOTDIR ? XR_XIR_SDK_UNRESOLVED :
        error == ELOOP || error == ENAMETOOLONG ? XR_XIR_SDK_INVALID : XR_XIR_SDK_IO);
}
static void sdk_posix_close(int fd) {
    XR_CHECK(close(fd) == 0 || errno == EINTR, "Owned SDK descriptor could not be closed");
}
/* Open an absolute canonical path without following any symbolic link. The
 * returned descriptor names a directory or a regular file as requested. */
static int sdk_posix_open(XrXirRuntimeSdk *sdk, const char *path, bool directory) {
    if (sdk->status != XR_XIR_SDK_OK) return -1;
    if (path[0] != '/') { sdk_fail(sdk, XR_XIR_SDK_UNSUPPORTED); return -1; }
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { sdk_posix_error(sdk, errno); return -1; }
    char name[NAME_MAX + 1];
    size_t at = 1;
    while (path[at]) {
        size_t length = 0;
        while (path[at + length] && path[at + length] != '/') {
            if (length == NAME_MAX) { sdk_posix_close(fd); sdk_fail(sdk, XR_XIR_SDK_BUDGET); return -1; }
            name[length] = path[at + length]; ++length;
        }
        name[length] = 0;
        bool last = !path[at + length];
        if (!length || (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) {
            sdk_posix_close(fd); sdk_fail(sdk, XR_XIR_SDK_INVALID); return -1;
        }
        /* A non-blocking leaf open cannot stall on a FIFO planted in place
         * of a regular file; the type is checked before any read. */
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (!last || directory ? O_DIRECTORY : O_NONBLOCK);
        int next = openat(fd, name, flags);
        int error = errno;
        sdk_posix_close(fd);
        if (next < 0) { sdk_posix_error(sdk, error); return -1; }
        fd = next; at += length + (last ? 0 : 1);
    }
    struct stat info;
    if (fstat(fd, &info)) {
        int error = errno;
        sdk_posix_close(fd); sdk_posix_error(sdk, error); return -1;
    }
    if (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) {
        sdk_posix_close(fd); sdk_fail(sdk, XR_XIR_SDK_INVALID); return -1;
    }
    return fd;
}
static bool sdk_root_admit(XrXirRuntimeSdk *sdk, const char *root) {
    char *canonical = sdk_allocate(sdk, PATH_MAX + 1);
    if (!canonical) return false;
    if (!realpath(root, canonical)) return sdk_posix_error(sdk, errno);
    size_t length = 0;
    while (canonical[length]) ++length;
    if (length < 2 || canonical[0] != '/') return sdk_fail(sdk, XR_XIR_SDK_UNSUPPORTED);
    int fd = sdk_posix_open(sdk, canonical, true);
    if (fd < 0) return false;
    sdk_posix_close(fd);
    sdk->root = canonical; sdk->root_length = length;
    return true;
}
static bool sdk_file_hash(XrXirRuntimeSdk *sdk, XrXirSdkFile *file, int fd, uint8_t *buffer) {
    struct stat info;
    if (fstat(fd, &info)) return sdk_posix_error(sdk, errno);
    if (info.st_size < 0 || (uint64_t)info.st_size != file->length) return sdk_fail(sdk, XR_XIR_SDK_INVALID);
    XrSHA256Context hash; xr_sha256_init(&hash);
    uint64_t left = file->length;
    while (left) {
        size_t count = left > SDK_READ_CAPACITY ? SDK_READ_CAPACITY : (size_t)left;
        ssize_t actual = read(fd, buffer, count);
        if (actual < 0 && errno == EINTR) continue;
        if (actual < 0) return sdk_posix_error(sdk, errno);
        if (!actual) return sdk_fail(sdk, XR_XIR_SDK_IO);
        xr_sha256_update(&hash, buffer, (size_t)actual); left -= (uint64_t)actual;
    }
    uint8_t digest[32]; xr_sha256_final(&hash, digest);
    return !memcmp(digest, file->digest, 32) || sdk_fail(sdk, XR_XIR_SDK_INVALID);
}
static char *sdk_file_path(XrXirRuntimeSdk *sdk, const char *relative) {
    size_t length = 0;
    while (relative[length]) {
        unsigned char byte = (unsigned char)relative[length];
        if (byte >= 128 || byte < 32 || byte == '\\') { sdk_fail(sdk, XR_XIR_SDK_INVALID); return NULL; }
        if (++length > XR_XIR_SDK_PATH_LIMIT) { sdk_fail(sdk, XR_XIR_SDK_BUDGET); return NULL; }
    }
    if (!length || relative[0] == '/') { sdk_fail(sdk, XR_XIR_SDK_INVALID); return NULL; }
    if (sdk->root_length + length + 2 > PATH_MAX) { sdk_fail(sdk, XR_XIR_SDK_BUDGET); return NULL; }
    char *path = sdk_allocate(sdk, sdk->root_length + length + 2);
    if (!path) return NULL;
    memcpy(path, sdk->root, sdk->root_length); path[sdk->root_length] = '/';
    memcpy(path + sdk->root_length + 1, relative, length + 1);
    return path;
}
static bool sdk_files_admit(XrXirRuntimeSdk *sdk, const char *root) {
    if (sdk->status != XR_XIR_SDK_OK || !sdk_root_admit(sdk, root)) return false;
    uint8_t *buffer = sdk_allocate(sdk, SDK_READ_CAPACITY);
    if (!buffer) return false;
    for (uint32_t i = 0; i < sdk->manifest.file_count; ++i) {
        XrXirSdkFile *file = &sdk->manifest.files[i];
        char *path = sdk_file_path(sdk, file->path);
        if (!path) return false;
        int fd = sdk_posix_open(sdk, path, false);
        if (fd < 0) return false;
        bool verified = sdk_file_hash(sdk, file, fd, buffer);
        sdk_posix_close(fd);
        if (!verified) return false;
        file->absolute_path = path;
    }
    return true;
}
static void sdk_files_close(XrXirRuntimeSdk *sdk) { (void)sdk; }
