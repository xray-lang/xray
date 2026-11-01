/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_workspace_posix.inc.c - Directory-descriptor private roots
 *
 * KEY CONCEPT:
 *   The admitted parent descriptor creates the private root with mkdirat, and
 *   the root descriptor creates input/output. Cleanup walks descendants from
 *   those descriptors with *at calls that never follow a symbolic link, and
 *   removes the root only while it still has the identity it was created with.
 */
#include "../../os/os_random.h"
#include "../../base/xchecks.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static XtcXirWorkspaceStatus workspace_posix_error(XtcXirWorkspace *owner, int error) {
    return workspace_fail(owner, error == ENOMEM ? XTC_XIR_WORKSPACE_OUT_OF_MEMORY :
        error == ENOENT || error == ENOTDIR ? XTC_XIR_WORKSPACE_UNRESOLVED :
        error == ELOOP ? XTC_XIR_WORKSPACE_REPARSE : XTC_XIR_WORKSPACE_IO, (uint32_t)error);
}
static bool workspace_target(XtcXirWorkspace *owner, XrXirTargetStatus status) {
    if (status == XR_XIR_TARGET_OK) return true;
    workspace_fail(owner, status == XR_XIR_TARGET_UNRESOLVED ? XTC_XIR_WORKSPACE_UNRESOLVED :
        status == XR_XIR_TARGET_UNSUPPORTED ? XTC_XIR_WORKSPACE_UNSUPPORTED :
        status == XR_XIR_TARGET_BUDGET ? XTC_XIR_WORKSPACE_BUDGET :
        status == XR_XIR_TARGET_MEMORY_LIMIT ? XTC_XIR_WORKSPACE_MEMORY_LIMIT :
        status == XR_XIR_TARGET_OVERFLOW ? XTC_XIR_WORKSPACE_OVERFLOW :
        status == XR_XIR_TARGET_OUT_OF_MEMORY ? XTC_XIR_WORKSPACE_OUT_OF_MEMORY :
        status == XR_XIR_TARGET_IO ? XTC_XIR_WORKSPACE_IO : XTC_XIR_WORKSPACE_INVALID, (uint32_t)status);
    return false;
}
static bool workspace_posix_same(const struct stat *info, const XtcXirDirectoryFacts *facts) {
    uint64_t inode = (uint64_t)info->st_ino;
    return (uint64_t)info->st_dev == facts->volume && !memcmp(facts->file_id, &inode, sizeof(inode));
}
static bool workspace_paths_make(XtcXirWorkspace *owner) {
    const char *parent = xtc_xir_file_lease_directory_facts(owner->parent_lease)->path;
    size_t length = 0;
    if (!workspace_length(owner, parent, &length)) return false;
    bool slash = length && parent[length - 1] == '/';
    size_t root_length = length + (slash ? 0 : 1) + 37;
    if (root_length + 7 > owner->limits.path_bytes) {
        workspace_fail(owner, XTC_XIR_WORKSPACE_BUDGET, 0); return false;
    }
    char *root = owner->text, *input = root + owner->limits.path_bytes + 1;
    char *output = input + owner->limits.path_bytes + 1;
    memcpy(root, parent, length);
    size_t offset = length; if (!slash) root[offset++] = '/';
    memcpy(root + offset, owner->root_name, 37); offset += 37;
    root[offset] = 0;
    memcpy(input, root, offset); memcpy(input + offset, "/input", 7);
    memcpy(output, root, offset); memcpy(output + offset, "/output", 8);
    owner->paths = (XtcXirWorkspacePaths){root, input, output}; return true;
}
/* The path text of every created directory must resolve to the descriptor's
 * own identity, so consumers that reopen by path reach the private tree. */
static bool workspace_posix_admit(XtcXirWorkspace *owner, uint32_t index, int fd) {
    struct stat info;
    if (fstat(fd, &info)) { workspace_posix_error(owner, errno); return false; }
    if (!S_ISDIR(info.st_mode)) { workspace_fail(owner, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, 0); return false; }
    if (!index) {
        owner->root_device = (uint64_t)info.st_dev; owner->root_inode = (uint64_t)info.st_ino;
        owner->identified = true;
    }
    const char *path = index == 0 ? owner->paths.root : index == 1 ? owner->paths.input : owner->paths.output;
    if (!workspace_target(owner, xtc_xir_file_lease_directory_open(owner->resources, path, &owner->leases[index])))
        return false;
    if (!workspace_posix_same(&info, xtc_xir_file_lease_directory_facts(owner->leases[index]))) {
        workspace_fail(owner, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, 0); return false;
    }
    return true;
}
static XtcXirWorkspaceStatus workspace_create_posix(XtcXirWorkspace *owner) {
    if (!workspace_target(owner, xtc_xir_file_lease_directory_open(owner->resources, owner->parent, &owner->parent_lease)))
        return owner->diagnostic.status;
    const XtcXirDirectoryFacts *parent = xtc_xir_file_lease_directory_facts(owner->parent_lease);
    owner->parent_fd = open(parent->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (owner->parent_fd < 0) return workspace_posix_error(owner, errno);
    struct stat info;
    if (fstat(owner->parent_fd, &info)) return workspace_posix_error(owner, errno);
    if (!workspace_posix_same(&info, parent)) return workspace_fail(owner, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, 0);
    static const char hex[] = "0123456789abcdef";
    for (uint32_t attempt = 0; attempt < 16; ++attempt) {
        uint8_t nonce[16];
        xr_random_bytes(nonce, sizeof(nonce));
        memcpy(owner->root_name, "xray-", 5);
        for (size_t i = 0; i < 16; ++i) {
            owner->root_name[5 + i * 2] = hex[nonce[i] >> 4];
            owner->root_name[6 + i * 2] = hex[nonce[i] & 15];
        }
        owner->root_name[37] = 0;
        if (!workspace_paths_make(owner)) return owner->diagnostic.status;
        if (!mkdirat(owner->parent_fd, owner->root_name, 0700)) { owner->exists[0] = true; break; }
        if (errno != EEXIST) return workspace_posix_error(owner, errno);
        if (attempt == 15) return workspace_fail(owner, XTC_XIR_WORKSPACE_IO, EEXIST);
    }
    owner->root_fd = openat(owner->parent_fd, owner->root_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (owner->root_fd < 0) return workspace_posix_error(owner, errno);
    if (!workspace_posix_admit(owner, 0, owner->root_fd)) return owner->diagnostic.status;
    for (uint32_t i = 1; i < 3; ++i) {
        const char *name = i == 1 ? "input" : "output";
        if (mkdirat(owner->root_fd, name, 0700)) return workspace_posix_error(owner, errno);
        owner->exists[i] = true;
        int fd = openat(owner->root_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return workspace_posix_error(owner, errno);
        bool admitted = workspace_posix_admit(owner, i, fd);
        close(fd);
        if (!admitted) return owner->diagnostic.status;
    }
    return XTC_XIR_WORKSPACE_OK;
}
/* One explicit OS operation per step. A frame holds a directory descriptor
 * and, once enumeration starts, a stream over a duplicate of it. Removals
 * during a pass may hide entries from readdir, so a pass that removed anything
 * rewinds; the directory is empty only after a pass that removed nothing. */
static XtcXirWorkspaceStatus workspace_close_step(XtcXirWorkspace *owner) {
    XR_DCHECK(owner->frame_count >= 1 && owner->frame_count <= owner->limits.depth, "cleanup frames stay in budget");
    WorkspaceFrame *top = &owner->frames[owner->frame_count - 1];
    if (!top->stream) {
        int copy = dup(top->fd);
        if (copy < 0) return workspace_posix_error(owner, errno);
        top->stream = fdopendir(copy);
        if (!top->stream) { int error = errno; close(copy); return workspace_posix_error(owner, error); }
        top->removed = false; return XTC_XIR_WORKSPACE_PENDING;
    }
    errno = 0;
    struct dirent *entry = readdir(top->stream);
    if (!entry) {
        if (errno) return workspace_posix_error(owner, errno);
        if (top->removed) { rewinddir(top->stream); top->removed = false; return XTC_XIR_WORKSPACE_PENDING; }
        closedir(top->stream); top->stream = NULL;
        if (owner->frame_count == 1) { owner->close_phase = 2; return XTC_XIR_WORKSPACE_PENDING; }
        close(top->fd); top->fd = -1; --owner->frame_count;
        WorkspaceFrame *parent = &owner->frames[owner->frame_count - 1];
        if (unlinkat(parent->fd, top->name, AT_REMOVEDIR) && errno != ENOENT) return workspace_posix_error(owner, errno);
        parent->removed = true; return XTC_XIR_WORKSPACE_PENDING;
    }
    const char *name = entry->d_name;
    if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) return XTC_XIR_WORKSPACE_PENDING;
    struct stat info;
    if (fstatat(top->fd, name, &info, AT_SYMLINK_NOFOLLOW)) {
        if (errno == ENOENT) return XTC_XIR_WORKSPACE_PENDING;
        return workspace_posix_error(owner, errno);
    }
    if (!S_ISDIR(info.st_mode)) {
        if (unlinkat(top->fd, name, 0) && errno != ENOENT) return workspace_posix_error(owner, errno);
        top->removed = true; return XTC_XIR_WORKSPACE_PENDING;
    }
    size_t length = strlen(name);
    if (owner->frame_count >= owner->limits.depth || length >= sizeof(top->name))
        return workspace_fail(owner, XTC_XIR_WORKSPACE_BUDGET, 0);
    int fd = openat(top->fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return workspace_posix_error(owner, errno);
    WorkspaceFrame *child = &owner->frames[owner->frame_count++];
    child->fd = fd; child->stream = NULL; child->removed = false;
    memcpy(child->name, name, length + 1);
    return XTC_XIR_WORKSPACE_PENDING;
}
/* Leases hold no descriptors on POSIX; the root is removed only if the parent
 * still names the directory this owner created. A root whose identity was
 * never observed can only be removed while it is still empty. */
static XtcXirWorkspaceStatus workspace_close_root(XtcXirWorkspace *owner) {
    if (owner->root_fd >= 0) { close(owner->root_fd); owner->root_fd = -1; owner->frames[0].fd = -1; }
    if (owner->exists[0]) {
        struct stat info;
        if (fstatat(owner->parent_fd, owner->root_name, &info, AT_SYMLINK_NOFOLLOW)) {
            if (errno != ENOENT) return workspace_posix_error(owner, errno);
        } else if (owner->identified && (!S_ISDIR(info.st_mode) || (uint64_t)info.st_dev != owner->root_device ||
            (uint64_t)info.st_ino != owner->root_inode)) {
            return workspace_fail(owner, XTC_XIR_WORKSPACE_IDENTITY_MISMATCH, 0);
        } else if (unlinkat(owner->parent_fd, owner->root_name, AT_REMOVEDIR) && errno != ENOENT) {
            return workspace_posix_error(owner, errno);
        }
        owner->exists[0] = false;
    }
    if (owner->parent_fd >= 0) { close(owner->parent_fd); owner->parent_fd = -1; }
    return XTC_XIR_WORKSPACE_OK;
}
static XtcXirWorkspaceStatus workspace_close_posix(XtcXirWorkspace *owner, uint32_t step_limit) {
    for (uint32_t step = 0; step < step_limit; ++step) {
        if (owner->close_phase == 0) {
            for (uint32_t i = 0; i < 3; ++i) { xtc_xir_file_lease_free(owner->leases[i]); owner->leases[i] = NULL; }
            xtc_xir_file_lease_free(owner->parent_lease); owner->parent_lease = NULL;
            if (owner->root_fd < 0) { owner->close_phase = 2; continue; }
            owner->frames[0] = (WorkspaceFrame){owner->root_fd, NULL, false, {0}};
            owner->frame_count = 1; owner->close_phase = 1; continue;
        }
        if (owner->close_phase == 2) return workspace_close_root(owner);
        XtcXirWorkspaceStatus status = workspace_close_step(owner);
        if (status != XTC_XIR_WORKSPACE_PENDING) return status;
    }
    return XTC_XIR_WORKSPACE_PENDING;
}
