/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_links_win.c - Wide symbolic links and owned following-handle paths
 *
 * KEY CONCEPT:
 *   Reparse substitute text is independent of the following path query. Every
 *   temporary handle closes before publication or remains in its real owner.
 */
#include "../os_file_links.h"
#include "../file_metadata_path.inc.h"
#include "../../base/xwindows_utf8.h"
#include <winioctl.h>
_Static_assert(sizeof(wchar_t)==2,"Windows reparse names contain UTF-16 units");

static bool file_links_current(const XrOsIoPolicy *policy,bool (*current)(void *),XrIoContext *io) {
    return !current || current(policy->context) || io_status(io,XR_OS_IO_CANCELLED);
}
static void *file_links_allocate(XrIoContext *io,size_t bytes) {
    void *data=io_alloc(io,bytes);
    if (!data && io->status==XR_OS_IO_OK) io_status(io,XR_OS_IO_BAD_ARGUMENT);
    return data;
}
static bool file_links_refused(DWORD error) {
    return xr_win_path_error_refused(error) || error==ERROR_CANT_ACCESS_FILE || error==ERROR_CANT_RESOLVE_FILENAME ||
        error==ERROR_ALREADY_EXISTS || error==ERROR_FILE_EXISTS || error==ERROR_PRIVILEGE_NOT_HELD ||
        error==ERROR_NOT_A_REPARSE_POINT || error==ERROR_DISK_FULL || error==ERROR_HANDLE_DISK_FULL;
}
static XrOsIoStatus file_links_error(DWORD error) {
    return file_links_refused(error) ? XR_OS_IO_OK : io_windows_status(error);
}
static XrOsIoStatus file_links_locator(const XrOsIoPolicy *policy,XrOsPathSpan path,
    bool (*current)(void *),wchar_t **output,bool *refused) {
    char *name=NULL;*refused=false;
    XrOsIoStatus status=metadata_path_owned(policy,path.text,path.length,current,&name);
    if (status!=XR_OS_IO_OK) return status;
    if (path.length>(size_t)32767*3) *refused=true;
    else {
        status=xr_win_utf8_path_owned_classified(policy,name,output,refused);
        if (*refused) status=XR_OS_IO_OK;
        if (status==XR_OS_IO_OK && !*refused && !*output) status=XR_OS_IO_BAD_ARGUMENT;
    }
    policy->free(policy->context,name);
    if (status==XR_OS_IO_OK && current && !current(policy->context)) status=XR_OS_IO_CANCELLED;
    return status;
}
static XrOsIoStatus file_links_wide_text(XrIoContext *io,const wchar_t *wide,size_t units,
    bool (*current)(void *),XrOsFileLinkText *output) {
    if (!wide || !units || units>INT_MAX) return units>INT_MAX?XR_OS_IO_OVERFLOW:XR_OS_IO_BAD_ARGUMENT;
    for (size_t i=0;i<units;++i) {
        if (!(i&4095u) && !file_links_current(io->policy,current,io)) return io->status;
        if (!wide[i]) return XR_OS_IO_BAD_ARGUMENT;
    }
    int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,(int)units,NULL,0,NULL,NULL);
    if (!count) return io_windows_status(GetLastError());
    char *text=file_links_allocate(io,(size_t)count+1);
    if (!text) return io->status;
    if (file_links_current(io->policy,current,io)) {
        if (WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,(int)units,text,count,NULL,NULL)!=count)
            io_status(io,io_windows_status(GetLastError()));
        else text[count]=0;
    }
    if (io->status==XR_OS_IO_OK && file_links_current(io->policy,current,io))
        *output=(XrOsFileLinkText){text,(size_t)count};
    else io_free(io,text);
    return io->status;
}
static bool file_links_drive(const wchar_t *text,size_t units) {
    return units>=3 && ((text[0]>=L'A'&&text[0]<=L'Z') || (text[0]>=L'a'&&text[0]<=L'z')) &&
        text[1]==L':' && (text[2]==L'\\'||text[2]==L'/');
}
static bool file_links_target_directory(XrIoContext *io,const wchar_t *target,const wchar_t *link,
    bool (*current)(void *),bool *directory,bool *refused) {
    size_t count=wcslen(target),base_length=0,prefix=0;const wchar_t *base=link;
    bool relative=target[0]!=L'\\' && target[0]!=L'/' && !(count>=2&&target[1]==L':');
    if (relative) {
        size_t length=wcslen(link);
        if (length>=8 && !wcsncmp(link,L"\\\\?\\UNC\\",8)) {base=link+8;prefix=2;}
        else if (length>=4 && !wcsncmp(link,L"\\\\?\\",4)) {
            if (!file_links_drive(link+4,length-4)) return io_status(io,XR_OS_IO_UNSUPPORTED);
            base=link+4;
        }
        for (size_t i=0;base[i];++i) if (base[i]==L'\\'||base[i]==L'/') base_length=i+1;
    }
    if (count>SIZE_MAX-base_length-prefix-1 || count+base_length+prefix+1>SIZE_MAX/sizeof(wchar_t))
        return io_status(io,XR_OS_IO_OVERFLOW);
    size_t units=prefix+base_length+count;
    wchar_t *query=file_links_allocate(io,(units+1)*sizeof(*query));
    if (!query) return false;
    if (prefix) memcpy(query,L"\\\\",prefix*sizeof(*query));
    if (base_length) memcpy(query+prefix,base,base_length*sizeof(*query));
    memcpy(query+prefix+base_length,target,(count+1)*sizeof(*query));
    XrOsIoStatus status=xr_win_path_locator_owned_classified(io->policy,&query,units,refused);
    if (!*refused) io_status(io,status);
    if (io->status==XR_OS_IO_OK && !*refused && file_links_current(io->policy,current,io)) {
        DWORD attributes=GetFileAttributesW(query);
        if (attributes!=INVALID_FILE_ATTRIBUTES) *directory=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        else {
            DWORD error=GetLastError();
            if (error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND) {
                if (file_links_refused(error)) *refused=true;
                else io_status(io,io_windows_status(error));
            }
        }
    }
    io_free(io,query);return io->status==XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_os_io_path_symlink(const XrOsIoPolicy *policy,
    XrOsPathSpan source,XrOsPathSpan link,bool (*current)(void *),bool *output) {
    if (!io_policy_valid(policy) || !output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io={policy,XR_OS_IO_OK};char *raw=NULL;wchar_t *target=NULL,*name=NULL;
    bool source_refused=false,link_refused=false,directory=false,changed=false;
    io_status(&io,metadata_path_owned(policy,source.text,source.length,current,&raw));
    if (io.status==XR_OS_IO_OK) {
        if (source.length>(size_t)32767*3) source_refused=true;
        else {
            io_status(&io,xr_win_utf8_owned_classified(policy,raw,&target,NULL,&source_refused));
            if (source_refused) io.status=XR_OS_IO_OK;
            else if (io.status==XR_OS_IO_OK && !target) io_status(&io,XR_OS_IO_BAD_ARGUMENT);
        }
    }
    if (io.status==XR_OS_IO_OK) io_status(&io,file_links_locator(policy,link,current,&name,&link_refused));
    if (io.status==XR_OS_IO_OK && !source_refused && !link_refused)
        (void)file_links_target_directory(&io,target,name,current,&directory,&source_refused);
    if (io.status==XR_OS_IO_OK && !source_refused && !link_refused && file_links_current(policy,current,&io)) {
        DWORD flags=SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE | (directory?SYMBOLIC_LINK_FLAG_DIRECTORY:0);
        changed=CreateSymbolicLinkW(name,target,flags)!=0;
        if (!changed) {
            DWORD error=GetLastError();
            /* This API's concrete invalid-parameter refusal is not a UTF
             * decoder error. Older Windows may reject its unprivileged flag. */
            io_status(&io,(error==ERROR_INVALID_PARAMETER || error==ERROR_NOT_SUPPORTED || error==ERROR_INVALID_FUNCTION)?
                XR_OS_IO_OK:file_links_error(error));
        }
    }
    io_free(&io,name);io_free(&io,target);io_free(&io,raw);
    if (io.status==XR_OS_IO_OK) (void)file_links_current(policy,current,&io);
    if (io.status==XR_OS_IO_OK) *output=changed;
    return io.status;
}
static bool file_links_query_valid(const XrOsIoPolicy *policy,XrOsFileMetadataOwner *owner,
    const XrOsFileLinkText *output) {
    return io_policy_valid(policy) && owner && !owner->handle && output && !output->text && !output->length;
}
static XrOsIoStatus file_links_open(const wchar_t *name,DWORD flags,
    XrOsFileMetadataOwner *owner,bool *present) {
    HANDLE handle=CreateFileW(name,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|flags,NULL);
    if (handle==INVALID_HANDLE_VALUE) return file_links_error(GetLastError());
    owner->handle=(intptr_t)handle;*present=true;return XR_OS_IO_OK;
}
static bool file_links_reparse_span(size_t bytes,size_t offset,size_t length) {
    return !(offset%sizeof(wchar_t)) && !(length%sizeof(wchar_t)) && offset<=bytes && length<=bytes-offset;
}
static XrOsIoStatus file_links_reparse(XrIoContext *io,HANDLE handle,
    bool (*current)(void *),XrOsFileLinkText *output) {
    unsigned char *data=file_links_allocate(io,MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    if (!data) return io->status;
    DWORD returned=0;XrOsIoStatus status=XR_OS_IO_OK;
    if (!file_links_current(io->policy,current,io)) status=io->status;
    else if (!DeviceIoControl(handle,FSCTL_GET_REPARSE_POINT,NULL,0,data,MAXIMUM_REPARSE_DATA_BUFFER_SIZE,&returned,NULL))
        status=file_links_error(GetLastError());
    else {
        uint32_t tag=0,flags=0;uint16_t bytes=0,offset=0,length=0,print_offset=0,print_length=0;
        if (returned<8 || returned>MAXIMUM_REPARSE_DATA_BUFFER_SIZE) status=XR_OS_IO_BAD_ARGUMENT;
        else {
            memcpy(&tag,data,4);memcpy(&bytes,data+4,2);
            size_t header=tag==IO_REPARSE_TAG_SYMLINK?12u:tag==IO_REPARSE_TAG_MOUNT_POINT?8u:0u;
            if (!header) status=XR_OS_IO_UNSUPPORTED;
            else if ((size_t)bytes+8!=returned || bytes<header) status=XR_OS_IO_BAD_ARGUMENT;
            else {
                memcpy(&offset,data+8,2);memcpy(&length,data+10,2);
                memcpy(&print_offset,data+12,2);memcpy(&print_length,data+14,2);
                if (tag==IO_REPARSE_TAG_SYMLINK) memcpy(&flags,data+16,4);
                size_t path_bytes=bytes-header;
                if ((flags&~1u) || !length || !file_links_reparse_span(path_bytes,offset,length) ||
                    !file_links_reparse_span(path_bytes,print_offset,print_length)) status=XR_OS_IO_BAD_ARGUMENT;
                else {
                    wchar_t *substitute=(wchar_t *)(data+8+header+offset);size_t units=length/sizeof(wchar_t);
                    if (!(flags&1u)) {
                        if (units>=4 && !wmemcmp(substitute,L"\\??\\",4)) {
                            /* Preserve the actual DOS substitute namespace;
                             * only its NT spelling changes to Win32 syntax. */
                            substitute[1]=L'\\';substitute[2]=L'?';
                        } else if (!file_links_drive(substitute,units) &&
                            !(units>=2&&substitute[0]==L'\\'&&substitute[1]==L'\\')) status=XR_OS_IO_UNSUPPORTED;
                    }
                    if (status==XR_OS_IO_OK) status=file_links_wide_text(io,substitute,units,current,output);
                }
            }
        }
    }
    io_free(io,data);return status;
}
static XrOsIoStatus file_links_final(XrIoContext *io,HANDLE handle,
    bool (*current)(void *),XrOsFileLinkText *output) {
    DWORD capacity=GetFinalPathNameByHandleW(handle,NULL,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if (!capacity) return file_links_error(GetLastError());
    for (;;) {
        if ((uint64_t)capacity>SIZE_MAX/sizeof(wchar_t)) return XR_OS_IO_OVERFLOW;
        if (!file_links_current(io->policy,current,io)) return io->status;
        wchar_t *wide=file_links_allocate(io,(size_t)capacity*sizeof(*wide));
        if (!wide) return io->status;
        DWORD length=GetFinalPathNameByHandleW(handle,wide,capacity,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        if (!length) {XrOsIoStatus status=file_links_error(GetLastError());io_free(io,wide);return status;}
        if (length>=capacity) {
            io_free(io,wide);
            if (capacity==UINT32_MAX) return XR_OS_IO_OVERFLOW;
            DWORD doubled=capacity>UINT32_MAX/2?UINT32_MAX:capacity*2;
            capacity=length>doubled?length:doubled;continue;
        }
        XrOsIoStatus status=wide[length]?XR_OS_IO_BAD_ARGUMENT:file_links_wide_text(io,wide,length,current,output);
        io_free(io,wide);return status;
    }
}
static XrOsIoStatus file_links_query(const XrOsIoPolicy *policy,XrOsPathSpan path,
    bool (*current)(void *),XrOsFileMetadataOwner *owner,XrOsFileLinkText *output,bool following) {
    if (!file_links_query_valid(policy,owner,output)) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io={policy,XR_OS_IO_OK};wchar_t *name=NULL;XrOsFileLinkText result={0};
    bool refused=false,present=false;
    io_status(&io,file_links_locator(policy,path,current,&name,&refused));
    if (io.status==XR_OS_IO_OK && !refused && file_links_current(policy,current,&io))
        io_status(&io,file_links_open(name,following?0:FILE_FLAG_OPEN_REPARSE_POINT,owner,&present));
    if (io.status==XR_OS_IO_OK && present)
        io_status(&io,following?file_links_final(&io,(HANDLE)owner->handle,current,&result):
            file_links_reparse(&io,(HANDLE)owner->handle,current,&result));
    XrOsIoStatus closed=xr_os_io_file_metadata_close(owner);
    io_status(&io,closed);io_free(&io,name);
    if (io.status==XR_OS_IO_OK) (void)file_links_current(policy,current,&io);
    if (io.status==XR_OS_IO_OK) *output=result;
    else io_free(&io,result.text);
    return io.status;
}
XR_FUNC XrOsIoStatus xr_os_io_path_readlink(const XrOsIoPolicy *policy,
    XrOsPathSpan path,bool (*current)(void *),XrOsFileMetadataOwner *owner,XrOsFileLinkText *output) {
    return file_links_query(policy,path,current,owner,output,false);
}
XR_FUNC XrOsIoStatus xr_os_io_path_realpath(const XrOsIoPolicy *policy,
    XrOsPathSpan path,bool (*current)(void *),XrOsFileMetadataOwner *owner,XrOsFileLinkText *output) {
    return file_links_query(policy,path,current,owner,output,true);
}
