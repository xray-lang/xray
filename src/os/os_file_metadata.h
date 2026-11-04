/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os_file_metadata.h - Complete following metadata with an independent link fact
 */
#ifndef XR_OS_FILE_METADATA_H
#define XR_OS_FILE_METADATA_H
#include "os_system.h"

typedef struct XrOsFileMetadata {
    bool present;
    int64_t size, mode;
    int64_t mtime, atime, ctime;
    XrOsOptionalI64 uid, gid;
    bool is_file, is_dir, is_symlink;
} XrOsFileMetadata;

/* Owns one temporary file handle for metadata or controlled mutations. Only
 * one is held at a time, including separate following/link queries. A failed
 * close retains the handle; closing it never replays the original operation. */
typedef struct XrOsFileMetadataOwner { intptr_t handle; } XrOsFileMetadataOwner;

/* Path is an exact UTF-8 span, not necessarily NUL-terminated. Embedded NUL
 * and malformed UTF-8 are protocol errors. Current synchronously borrows
 * policy.context; cleanup never needs current or retains the borrowed context.
 * Owner must start empty. Each acquired handle gets one close attempt before
 * publication. A caller retries retained owners only after execution drains.
 * Ordinary path refusal publishes present=false; typed faults preserve output.
 * Size is bytes, mode is the platform's permission bits masked by 0777, and
 * times are integer Unix-epoch seconds. Windows ctime is creation time, as in
 * its CRT stat convention; uid/gid are absent and mode does not describe ACLs.
 * Metadata follows the target while is_symlink describes the named leaf.
 * On Windows this includes name-surrogate reparse routes such as junctions,
 * so recursive consumers do not follow them into an external tree.
 * The two observations do not claim an atomic snapshot across path changes. */
XR_FUNC XrOsIoStatus xr_os_io_file_metadata(const XrOsIoPolicy *policy,
    const char *path, size_t length, bool (*current)(void *context),
    XrOsFileMetadataOwner *owner, XrOsFileMetadata *output);
XR_FUNC XrOsIoStatus xr_os_io_file_metadata_close(XrOsFileMetadataOwner *owner);
#endif // XR_OS_FILE_METADATA_H
