/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_file_lease_posix.inc.c - Descriptor-walked identity and digest leases
 *
 * KEY CONCEPT:
 *   A lease names the canonical path, device/inode identity and content digest
 *   observed through a descriptor walk that refuses every symbolic link. POSIX
 *   cannot share-lock a file against writers, and a build observes hundreds of
 *   headers while the default descriptor limit is 256, so descriptors are
 *   released after observation. A later read reopens the same walk and must
 *   find the same identity and the same digest, or it fails closed.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#define LEASE_READ_CAPACITY 65536u

static bool lease_posix_error(XrXirTargetSnapshot *storage, int error) {
    return xtc_xir_target_fail(storage, error == ENOMEM ? XR_XIR_TARGET_OUT_OF_MEMORY :
        error == ENOENT || error == ENOTDIR ? XR_XIR_TARGET_UNRESOLVED :
        error == ELOOP || error == ENAMETOOLONG ? XR_XIR_TARGET_INVALID : XR_XIR_TARGET_IO);
}
static void lease_posix_close(int fd) {
    XR_CHECK(close(fd) == 0 || errno == EINTR, "Owned lease descriptor could not be closed");
}
/* Walk an absolute canonical path from "/" without following links. */
static int lease_posix_walk(XrXirTargetSnapshot *storage, const char *path, bool directory) {
    if (storage->status != XR_XIR_TARGET_OK) return -1;
    if (path[0] != '/') { xtc_xir_target_fail(storage, XR_XIR_TARGET_UNSUPPORTED); return -1; }
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { lease_posix_error(storage, errno); return -1; }
    char name[NAME_MAX + 1];
    size_t at = 1;
    while (path[at]) {
        size_t length = 0;
        while (path[at + length] && path[at + length] != '/') {
            if (length == NAME_MAX) {
                lease_posix_close(fd); xtc_xir_target_fail(storage, XR_XIR_TARGET_BUDGET); return -1;
            }
            name[length] = path[at + length]; ++length;
        }
        name[length] = 0;
        bool last = !path[at + length];
        if (!length || (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) {
            lease_posix_close(fd); xtc_xir_target_fail(storage, XR_XIR_TARGET_INVALID); return -1;
        }
        /* The leaf is opened non-blocking so a FIFO cannot stall the walk. */
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (!last || directory ? O_DIRECTORY : O_NONBLOCK);
        int next = openat(fd, name, flags);
        int error = errno;
        lease_posix_close(fd);
        if (next < 0) { lease_posix_error(storage, error); return -1; }
        fd = next; at += length + (last ? 0 : 1);
    }
    return fd;
}
static bool lease_posix_stat(XrXirTargetSnapshot *storage, int fd, bool directory, struct stat *info) {
    if (fstat(fd, info)) return lease_posix_error(storage, errno);
    if (directory ? !S_ISDIR(info->st_mode) : !S_ISREG(info->st_mode) || info->st_size < 0)
        return xtc_xir_target_fail(storage, XR_XIR_TARGET_INVALID);
    return true;
}
static void lease_posix_identity(XtcXirFileLease *lease, const struct stat *info) {
    uint64_t inode = (uint64_t)info->st_ino;
    lease->directory_facts.volume = (uint64_t)info->st_dev;
    memset(lease->directory_facts.file_id, 0, sizeof(lease->directory_facts.file_id));
    memcpy(lease->directory_facts.file_id, &inode, sizeof(inode));
    lease->size = (uint64_t)info->st_size;
#ifdef XR_OS_MACOS
    lease->changed[0] = (int64_t)info->st_mtimespec.tv_sec; lease->changed[1] = info->st_mtimespec.tv_nsec;
    lease->changed[2] = (int64_t)info->st_ctimespec.tv_sec; lease->changed[3] = info->st_ctimespec.tv_nsec;
#else
    lease->changed[0] = (int64_t)info->st_mtim.tv_sec; lease->changed[1] = info->st_mtim.tv_nsec;
    lease->changed[2] = (int64_t)info->st_ctim.tv_sec; lease->changed[3] = info->st_ctim.tv_nsec;
#endif
}
/* Hash from the current position; the walk returns a fresh descriptor. */
static bool lease_posix_hash(XrXirTargetSnapshot *storage, int fd, uint64_t length,
    void *scratch, uint8_t digest[32], void *copy) {
    XrSHA256Context hash; xr_sha256_init(&hash);
    uint64_t left = length;
    uint8_t *out = copy;
    while (left) {
        size_t count = left > LEASE_READ_CAPACITY ? LEASE_READ_CAPACITY : (size_t)left;
        ssize_t actual = read(fd, out ? (void *)out : scratch, count);
        if (actual < 0 && errno == EINTR) continue;
        if (actual < 0) return lease_posix_error(storage, errno);
        if (!actual) return xtc_xir_target_fail(storage, XR_XIR_TARGET_IO);
        xr_sha256_update(&hash, out ? out : (const uint8_t *)scratch, (size_t)actual);
        if (out) out += actual;
        left -= (uint64_t)actual;
    }
    xr_sha256_final(&hash, digest);
    return true;
}
static XrXirTargetStatus file_lease_open(XrCompileResources *resources, const char *path,
    bool directory, XtcXirFileLease **output) {
    if (!resources || !path || !output || *output) return XR_XIR_TARGET_INVALID;
    void *memory = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(resources, 1, sizeof(XtcXirFileLease), &memory);
    if (allocated != XR_COMPILE_RESOURCE_OK)
        return allocated == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_TARGET_MEMORY_LIMIT :
            allocated == XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_TARGET_OVERFLOW :
            allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_INVALID;
    XtcXirFileLease *lease = memory;
    lease->storage.resources = resources; lease->directory = directory;
    XrXirTargetSnapshot *storage = &lease->storage;
    /* Resolution and hashing scratch is released before the lease is kept. */
    char *resolved = NULL, *canonical = NULL; void *scratch = NULL;
    XrCompileResourceStatus made = xr_compile_resources_alloc(resources, PATH_MAX + 1, (void **)&resolved);
    if (made == XR_COMPILE_RESOURCE_OK && !directory)
        made = xr_compile_resources_alloc(resources, LEASE_READ_CAPACITY, &scratch);
    if (made != XR_COMPILE_RESOURCE_OK)
        xtc_xir_target_fail(storage, made == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_TARGET_MEMORY_LIMIT :
            made == XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_TARGET_OVERFLOW :
            made == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_INVALID);
    else if (!realpath(path, resolved)) lease_posix_error(storage, errno);
    else canonical = xtc_xir_target_text(storage, resolved);
    int fd = canonical ? lease_posix_walk(storage, canonical, directory) : -1;
    struct stat info;
    if (fd >= 0 && lease_posix_stat(storage, fd, directory, &info)) {
        lease_posix_identity(lease, &info);
        if (directory) {
            lease->directory_facts.path = canonical; lease->directory_facts.native_path = NULL;
        } else if (lease_posix_hash(storage, fd, lease->size, scratch, lease->file.digest, NULL)) {
            lease->file.path = canonical; lease->file.length = lease->size;
        }
    }
    if (fd >= 0) lease_posix_close(fd);
    xr_compile_resources_free(scratch); xr_compile_resources_free(resolved);
    XrXirTargetStatus status = storage->status;
    if (status != XR_XIR_TARGET_OK) { xtc_xir_file_lease_free(lease); return status; }
    *output = lease; return XR_XIR_TARGET_OK;
}
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_open(XrCompileResources *resources,
    const char *path, XtcXirFileLease **output) {
    return file_lease_open(resources, path, false, output);
}
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_directory_open(XrCompileResources *resources,
    const char *path, XtcXirFileLease **output) {
    return file_lease_open(resources, path, true, output);
}
/* Reopen the leased walk; identity and digest must equal the observation. */
static XrXirTargetStatus file_lease_posix_read(XtcXirFileLease *lease, size_t limit,
    void **owned_bytes, size_t *length) {
    XR_DCHECK(!lease->directory && lease->file.path, "only file leases are read");
    XrXirTargetSnapshot *storage = &lease->storage;
    if (!limit || !owned_bytes || *owned_bytes || !length || *length) return XR_XIR_TARGET_INVALID;
    if (storage->status != XR_XIR_TARGET_OK) return storage->status;
    if (lease->file.length > SIZE_MAX) { xtc_xir_target_fail(storage, XR_XIR_TARGET_OVERFLOW); return storage->status; }
    if (lease->file.length > limit) { xtc_xir_target_fail(storage, XR_XIR_TARGET_BUDGET); return storage->status; }
    size_t bytes = (size_t)lease->file.length;
    void *data = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_alloc(storage->resources, bytes ? bytes : 1, &data);
    if (allocated != XR_COMPILE_RESOURCE_OK) {
        xtc_xir_target_fail(storage, allocated == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_TARGET_MEMORY_LIMIT :
            allocated == XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_TARGET_OVERFLOW :
            allocated == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_INVALID);
        return storage->status;
    }
    XtcXirFileLease observed = *lease;
    int fd = lease_posix_walk(storage, lease->file.path, false);
    struct stat info; uint8_t digest[32];
    bool okay = fd >= 0 && lease_posix_stat(storage, fd, false, &info);
    if (okay) {
        lease_posix_identity(&observed, &info);
        okay = (observed.directory_facts.volume == lease->directory_facts.volume &&
            !memcmp(observed.directory_facts.file_id, lease->directory_facts.file_id, 16) &&
            observed.size == lease->size && !memcmp(observed.changed, lease->changed, sizeof(lease->changed))) ||
            xtc_xir_target_fail(storage, XR_XIR_TARGET_INVALID);
    }
    if (okay) okay = lease_posix_hash(storage, fd, lease->file.length, NULL, digest, data);
    if (okay && memcmp(digest, lease->file.digest, 32)) okay = xtc_xir_target_fail(storage, XR_XIR_TARGET_INVALID);
    if (fd >= 0) lease_posix_close(fd);
    if (!bytes && okay) *(uint8_t *)data = 0;
    if (!okay) { xr_compile_resources_free(data); return storage->status; }
    *owned_bytes = data; *length = bytes; return XR_XIR_TARGET_OK;
}
XR_FUNC XrXirTargetStatus xtc_xir_sysroot_read(XrXirTargetSnapshot *storage,
    XtcXirLock *lock, uint64_t file_length, size_t limit, void **owned_bytes, size_t *length) {
    (void)file_length;
    if (!storage || !lock || lock->directory || !limit || !owned_bytes || *owned_bytes || !length || *length)
        return XR_XIR_TARGET_INVALID;
    return XR_XIR_TARGET_UNSUPPORTED;
}
/* Target snapshots and image observation require Windows process images. */
XR_FUNC bool xtc_xir_sysroot_capture(XrXirTargetSnapshot *snapshot, const XrXirTargetSnapshotRequest *request) {
    (void)request;
    return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_UNSUPPORTED);
}
XR_FUNC void xtc_xir_sysroot_close(XrXirTargetSnapshot *snapshot) { (void)snapshot; }
XR_FUNC bool xtc_xir_sysroot_observe(XrXirImageCollector *images, const XrProcImageEvent *event) {
    (void)event; return xtc_xir_target_fail(&images->storage, XR_XIR_TARGET_UNSUPPORTED);
}
XR_FUNC bool xtc_xir_sysroot_hash(XrXirTargetSnapshot *snapshot, XtcXirLock *lock, XrXirTargetFile *file) {
    (void)lock; (void)file; return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_UNSUPPORTED);
}
