/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_links_unix.c - Native link text and canonical existing POSIX paths
 *
 * KEY CONCEPT:
 *   Capacity growth never publishes a truncated readlink result. Realpath
 *   uses a proven native PATH_MAX buffer, never an implicit libc allocation.
 */
#if !defined(_XOPEN_SOURCE) && !defined(_GNU_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#include "../os_file_links.h"
#include "../file_metadata_path.inc.h"
#include <stdlib.h>
#include <unistd.h>

static bool file_links_current(const XrOsIoPolicy *policy,bool (*current)(void *),XrIoContext *io) {
    return !current || current(policy->context) || io_status(io,XR_OS_IO_CANCELLED);
}
static XrOsIoStatus file_links_error(int error) {
    if (error==ENOENT || error==ENOTDIR || error==EACCES || error==EPERM || error==ELOOP ||
        error==ENAMETOOLONG || error==EINVAL || error==EEXIST || error==EROFS || error==ENOSPC)
        return XR_OS_IO_OK;
#if defined(EDQUOT)
    if (error==EDQUOT) return XR_OS_IO_OK;
#endif
    return error==EOVERFLOW ? XR_OS_IO_OVERFLOW : io_errno_status(error);
}
static void *file_links_allocate(XrIoContext *io,size_t bytes) {
    void *data=io_alloc(io,bytes);
    if (!data && io->status==XR_OS_IO_OK) io_status(io,XR_OS_IO_BAD_ARGUMENT);
    return data;
}
XR_FUNC XrOsIoStatus xr_os_io_path_symlink(const XrOsIoPolicy *policy,
    XrOsPathSpan source,XrOsPathSpan link,bool (*current)(void *),bool *output) {
    if (!io_policy_valid(policy) || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io={policy,XR_OS_IO_OK};char *target=NULL,*name=NULL;bool changed=false;
    io_status(&io,metadata_path_owned(policy,source.text,source.length,current,&target));
    if (io.status==XR_OS_IO_OK) io_status(&io,metadata_path_owned(policy,link.text,link.length,current,&name));
    if (io.status==XR_OS_IO_OK && file_links_current(policy,current,&io)) {
        changed=symlink(target,name)==0;
        if (!changed) io_status(&io,file_links_error(errno));
    }
    io_free(&io,name);io_free(&io,target);
    if (io.status==XR_OS_IO_OK) (void)file_links_current(policy,current,&io);
    if (io.status==XR_OS_IO_OK) *output=changed;
    return io.status;
}
static bool file_links_query_valid(const XrOsIoPolicy *policy,XrOsFileMetadataOwner *owner,
    const XrOsFileLinkText *output) {
    return io_policy_valid(policy) && owner && !owner->handle && output && !output->text && !output->length;
}
XR_FUNC XrOsIoStatus xr_os_io_path_readlink(const XrOsIoPolicy *policy,
    XrOsPathSpan path,bool (*current)(void *),XrOsFileMetadataOwner *owner,XrOsFileLinkText *output) {
    if (!file_links_query_valid(policy,owner,output)) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io={policy,XR_OS_IO_OK};char *name=NULL,*text=NULL;size_t length=0,capacity=256;
    io_status(&io,metadata_path_owned(policy,path.text,path.length,current,&name));
    while (io.status==XR_OS_IO_OK && file_links_current(policy,current,&io)) {
        char *raw=file_links_allocate(&io,capacity);
        if (!raw) break;
        ssize_t read=readlink(name,raw,capacity);int error=errno;
        if (read<0) io_status(&io,file_links_error(error));
        else if ((size_t)read<capacity) {
            length=(size_t)read;
            io_status(&io,metadata_path_owned(policy,raw,length,current,&text));
        }
        io_free(&io,raw);
        if (read<0 || (size_t)read<capacity || io.status!=XR_OS_IO_OK) break;
        if (capacity>(size_t)INT_MAX/2) {io_status(&io,XR_OS_IO_OVERFLOW);break;}
        capacity*=2;
    }
    io_free(&io,name);
    if (io.status==XR_OS_IO_OK) (void)file_links_current(policy,current,&io);
    if (io.status==XR_OS_IO_OK) *output=(XrOsFileLinkText){text,text?length:0};
    else io_free(&io,text);
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_path_realpath(const XrOsIoPolicy *policy,
    XrOsPathSpan path,bool (*current)(void *),XrOsFileMetadataOwner *owner,XrOsFileLinkText *output) {
    if (!file_links_query_valid(policy,owner,output)) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io={policy,XR_OS_IO_OK};char *name=NULL,*text=NULL;size_t length=0;
    io_status(&io,metadata_path_owned(policy,path.text,path.length,current,&name));
#if defined(PATH_MAX) && PATH_MAX > 0 && (defined(XR_OS_LINUX) || defined(XR_OS_MACOS))
    char *raw=NULL;
    if (io.status==XR_OS_IO_OK) raw=file_links_allocate(&io,(size_t)PATH_MAX);
    if (io.status==XR_OS_IO_OK && file_links_current(policy,current,&io)) {
        /* Linux and Darwin supply the nonNULL realpath buffer contract with
         * their native PATH_MAX, including its terminator. Failed contents
         * are unspecified and are never scanned or published. */
        char *resolved=realpath(name,raw);
        if (!resolved) io_status(&io,file_links_error(errno));
        else if (resolved!=raw) io_status(&io,XR_OS_IO_BAD_ARGUMENT);
        else {
            while (length<(size_t)PATH_MAX && raw[length]) {
                if (!(length&4095u) && !file_links_current(policy,current,&io)) break;
                ++length;
            }
            if (!length || length==(size_t)PATH_MAX) io_status(&io,XR_OS_IO_BAD_ARGUMENT);
            if (io.status==XR_OS_IO_OK) io_status(&io,metadata_path_owned(policy,raw,length,current,&text));
        }
    }
    io_free(&io,raw);
#else
    /* An absent native bound does not authorize a guessed path buffer. */
    io_status(&io,XR_OS_IO_UNSUPPORTED);
#endif
    io_free(&io,name);
    if (io.status==XR_OS_IO_OK) (void)file_links_current(policy,current,&io);
    if (io.status==XR_OS_IO_OK) *output=(XrOsFileLinkText){text,text?length:0};
    else io_free(&io,text);
    return io.status;
}
