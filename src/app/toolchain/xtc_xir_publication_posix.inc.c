/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_publication_posix.inc.c - Directory-descriptor staging and rename
 *
 * KEY CONCEPT:
 *   The parent directory descriptor anchors the exclusive temporary file and
 *   the final renameat, so later path changes cannot redirect publication.
 */
#include "../../base/xchecks.h"
#include "../../os/os_random.h"
#include "../../shared/xr_utf8_core.h"
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

/* ".xray-", thirty-two hexadecimal nonce digits and ".tmp". */
#define PUBLICATION_TEMPORARY_BYTES 42u

static bool publication_posix_error(XtcXirPublication *owner, int error) {
    publication_fail(owner, publication_io_status(io_errno_status(error)), (uint32_t)error);
    return false;
}

/* The final segment must name an entry; the parent defaults to the working
 * directory. Neither path is resolved here, so preparation has no I/O. */
static bool publication_prepare(XtcXirPublication *owner, const char *destination) {
    size_t length = 0;
    if (!publication_text_length(owner, destination, &length)) return false;
    if (!length || xr_utf8_core_scan_strict((const uint8_t *)destination, length).error != XR_UTF8_OK) {
        publication_fail(owner, XTC_XIR_PUBLICATION_INVALID, 0); return false;
    }
    size_t slash = length;
    for (size_t i = length; i; --i) {
        if (destination[i - 1] == '/') { slash = i - 1; break; }
    }
    size_t leaf_start = slash == length ? 0 : slash + 1, leaf_bytes = length - leaf_start;
    const char *leaf = destination + leaf_start;
    if (!leaf_bytes || (leaf_bytes == 1 && leaf[0] == '.') ||
        (leaf_bytes == 2 && leaf[0] == '.' && leaf[1] == '.')) {
        publication_fail(owner, XTC_XIR_PUBLICATION_INVALID, 0); return false;
    }
    const char *parent = slash == length ? "." : slash == 0 ? "/" : destination;
    size_t parent_bytes = slash == length ? 1 : slash == 0 ? 1 : slash;
    if (parent_bytes + PUBLICATION_TEMPORARY_BYTES + 1 > owner->limits.path_bytes) {
        publication_fail(owner, XTC_XIR_PUBLICATION_BUDGET, 0); return false;
    }
    owner->parent = publication_alloc(owner, parent_bytes + 1);
    owner->leaf = publication_alloc(owner, leaf_bytes + 1);
    if (!publication_copy(owner, owner->parent, parent, parent_bytes) ||
        !publication_copy(owner, owner->leaf, leaf, leaf_bytes)) return false;
    owner->parent[parent_bytes] = 0; owner->leaf[leaf_bytes] = 0;
    return true;
}

static bool publication_parent(XtcXirPublication *owner) {
    owner->diagnostic.stage = XTC_XIR_PUBLICATION_PARENT;
    owner->parent_fd = open(owner->parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return owner->parent_fd >= 0 ? true : publication_posix_error(owner, errno);
}

/* Replacement is limited to an absent entry or a regular file; links,
 * directories and devices keep their identity. */
static bool publication_destination(XtcXirPublication *owner) {
    if (owner->diagnostic.status != XTC_XIR_PUBLICATION_OK) return false;
    struct stat information;
    if (fstatat(owner->parent_fd, owner->leaf, &information, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? true : publication_posix_error(owner, errno);
    if (!S_ISREG(information.st_mode)) {
        publication_fail(owner, XTC_XIR_PUBLICATION_UNSUPPORTED, 0); return false;
    }
    return true;
}

static bool publication_create(XtcXirPublication *owner) {
    owner->diagnostic.stage = XTC_XIR_PUBLICATION_CREATE;
    if (owner->diagnostic.status != XTC_XIR_PUBLICATION_OK) return false;
    static const char hex[] = "0123456789abcdef";
    for (uint32_t attempt = 0; attempt < owner->limits.create_attempts; ++attempt) {
        unsigned char nonce[16];
        xr_random_bytes(nonce, sizeof(nonce));
        memcpy(owner->temporary, ".xray-", 6);
        for (size_t i = 0; i < sizeof(nonce); ++i) {
            owner->temporary[6 + i * 2] = hex[nonce[i] >> 4];
            owner->temporary[7 + i * 2] = hex[nonce[i] & 15];
        }
        memcpy(owner->temporary + 38, ".tmp", 5);
        owner->file_fd = openat(owner->parent_fd, owner->temporary,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
        if (owner->file_fd >= 0) return true;
        if (errno != EEXIST && errno != EINTR) return publication_posix_error(owner, errno);
    }
    publication_fail(owner, XTC_XIR_PUBLICATION_IO, EEXIST); return false;
}

static XtcXirPublicationStatus publication_posix_write(XtcXirPublication *owner,
    const void *bytes, size_t length) {
    XR_DCHECK(owner->parent_fd < 0 && owner->file_fd < 0, "publication writes once");
    owner->phase = XTC_XIR_PUBLICATION_WRITING;
    if (!publication_parent(owner) || !publication_destination(owner) || !publication_create(owner))
        return owner->diagnostic.status;
    owner->diagnostic.stage = XTC_XIR_PUBLICATION_WRITE;
    size_t offset = 0;
    while (offset < length) {
        size_t chunk = length - offset > (size_t)INT_MAX ? (size_t)INT_MAX : length - offset;
        ssize_t count = write(owner->file_fd, (const uint8_t *)bytes + offset, chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            publication_posix_error(owner, errno); return owner->diagnostic.status;
        }
        if (!count || (size_t)count > chunk) return publication_fail(owner, XTC_XIR_PUBLICATION_IO, 0);
        offset += (size_t)count;
    }
    owner->diagnostic.stage = XTC_XIR_PUBLICATION_FLUSH;
    int synced;
    do synced = fsync(owner->file_fd); while (synced && errno == EINTR);
    if (synced) {
        publication_posix_error(owner, errno); return owner->diagnostic.status;
    }
    owner->phase = XTC_XIR_PUBLICATION_READY; return XTC_XIR_PUBLICATION_OK;
}

static XtcXirPublicationStatus publication_posix_commit(XtcXirPublication *owner) {
    XR_DCHECK(owner->parent_fd >= 0 && owner->file_fd >= 0, "ready publication owns descriptors");
    owner->diagnostic.stage = XTC_XIR_PUBLICATION_RENAME;
    if (!publication_destination(owner)) return owner->diagnostic.status;
    if (renameat(owner->parent_fd, owner->temporary, owner->parent_fd, owner->leaf)) {
        publication_posix_error(owner, errno); return owner->diagnostic.status;
    }
    owner->diagnostic.published = true; owner->phase = XTC_XIR_PUBLICATION_PUBLISHED;
    return XTC_XIR_PUBLICATION_OK;
}

/* An unpublished temporary entry is removed while the directory descriptor
 * still names it; a failed removal stays pending with both descriptors open.
 * close() is never retried because POSIX leaves a failed descriptor state
 * unspecified, and the bytes were already flushed before any commit. */
static XtcXirPublicationStatus publication_posix_release(XtcXirPublication *owner) {
    if (owner->file_fd >= 0) {
        if (!owner->diagnostic.published && unlinkat(owner->parent_fd, owner->temporary, 0) &&
            errno != ENOENT) {
            int error = errno;
            owner->diagnostic.cleanup_pending = true; owner->diagnostic.cleanup_os_error = (uint32_t)error;
            if (owner->diagnostic.status == XTC_XIR_PUBLICATION_OK) {
                owner->diagnostic.stage = XTC_XIR_PUBLICATION_CLEANUP;
                publication_posix_error(owner, error);
            }
            return XTC_XIR_PUBLICATION_PENDING;
        }
        close(owner->file_fd); owner->file_fd = -1;
    }
    if (owner->parent_fd >= 0) { close(owner->parent_fd); owner->parent_fd = -1; }
    owner->diagnostic.cleanup_pending = false;
    return XTC_XIR_PUBLICATION_OK;
}
