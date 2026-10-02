/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_runtime_sdk_windows.inc.c - Locked regular files and directory ancestry
 */
#define SDK_WIDE_CAPACITY 32768u
static bool sdk_windows_error(XrXirRuntimeSdk *sdk,DWORD error) {
    XrXirRuntimeSdkStatus status=error==ERROR_NOT_ENOUGH_MEMORY || error==ERROR_OUTOFMEMORY ?
        XR_XIR_SDK_OUT_OF_MEMORY : error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND ?
        XR_XIR_SDK_UNRESOLVED : XR_XIR_SDK_IO;
    return sdk_fail(sdk,status);
}
static bool sdk_windows_encoding_error(XrXirRuntimeSdk *sdk,DWORD error) {
    return error==ERROR_NO_UNICODE_TRANSLATION || error==ERROR_INVALID_PARAMETER ?
        sdk_fail(sdk,XR_XIR_SDK_INVALID) : sdk_windows_error(sdk,error);
}
static wchar_t *sdk_wide(XrXirRuntimeSdk *sdk,const char *text) {
    size_t length=0;
    while (length<SDK_WIDE_CAPACITY*4 && text[length]) ++length;
    if (length>=SDK_WIDE_CAPACITY*4 || !sdk_work(sdk,length)) {
        sdk_fail(sdk,XR_XIR_SDK_BUDGET);return NULL;
    }
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,(int)length,NULL,0);
    if (count<=0) {sdk_windows_encoding_error(sdk,GetLastError());return NULL;}
    if ((uint32_t)count>=SDK_WIDE_CAPACITY) {sdk_fail(sdk,XR_XIR_SDK_BUDGET);return NULL;}
    wchar_t *wide=sdk_allocate(sdk,((size_t)count+1)*sizeof(wchar_t));
    if (!wide) return NULL;
    if (MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,(int)length,wide,count)!=count) {
        sdk_windows_encoding_error(sdk,GetLastError());return NULL;
    }
    return wide;
}
static const char *sdk_utf8(XrXirRuntimeSdk *sdk,const wchar_t *path) {
    size_t count=wcslen(path);
    if (!sdk_work(sdk,count)) return NULL;
    int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,(int)count,NULL,0,NULL,NULL);
    if (!length) {sdk_windows_encoding_error(sdk,GetLastError());return NULL;}
    char *text=sdk_allocate(sdk,(size_t)length+1);
    if (!text) return NULL;
    if (WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,(int)count,text,length,NULL,NULL)!=length) {
        sdk_windows_encoding_error(sdk,GetLastError());return NULL;
    }
    for (int i=0;i<length;++i) if (text[i]=='\\') text[i]='/';
    return text;
}
static bool sdk_handle_check(XrXirRuntimeSdk *sdk,HANDLE handle,const wchar_t *expected,bool directory) {
    BY_HANDLE_FILE_INFORMATION info={0};
    if (!sdk_work(sdk,1)) return false;
    if (!GetFileInformationByHandle(handle,&info)) return sdk_windows_error(sdk,GetLastError());
    SetLastError(NO_ERROR);DWORD kind=GetFileType(handle);
    if (kind==FILE_TYPE_UNKNOWN && GetLastError()!=NO_ERROR) return sdk_windows_error(sdk,GetLastError());
    if (kind!=FILE_TYPE_DISK || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)!=0)!=directory)
        return sdk_fail(sdk,XR_XIR_SDK_INVALID);
    DWORD count=GetFinalPathNameByHandleW(handle,sdk->scratch,SDK_WIDE_CAPACITY,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if (!count) return sdk_windows_error(sdk,GetLastError());
    if (count>=SDK_WIDE_CAPACITY) return sdk_fail(sdk,XR_XIR_SDK_BUDGET);
    if (!sdk_work(sdk,count)) return false;
    if (expected && CompareStringOrdinal(expected,-1,sdk->scratch,-1,TRUE)!=CSTR_EQUAL)
        return sdk_fail(sdk,XR_XIR_SDK_INVALID);
    return true;
}
static bool sdk_directory_hold(XrXirRuntimeSdk *sdk,const wchar_t *path) {
    size_t length=wcslen(path);
    for (SdkDirectory *directory=sdk->directories;directory;directory=directory->next) {
        if (!sdk_work(sdk,length)) return false;
        if (!wcscmp(path,directory->path)) return true;
    }
    SdkDirectory *directory=sdk_allocate(sdk,sizeof(*directory)+(length+1)*sizeof(wchar_t));
    if (!directory) return false;
    directory->path=(wchar_t *)(directory+1);memcpy(directory->path,path,(length+1)*sizeof(wchar_t));
    HANDLE handle=CreateFileW(path,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ,NULL,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (handle==INVALID_HANDLE_VALUE) return sdk_windows_error(sdk,GetLastError());
    directory->handle=handle;directory->next=sdk->directories;sdk->directories=directory;
    return sdk_handle_check(sdk,handle,path,true);
}
static bool sdk_root_admit(XrXirRuntimeSdk *sdk,const char *root) {
    wchar_t *input=sdk_wide(sdk,root);
    sdk->scratch=sdk_allocate(sdk,SDK_WIDE_CAPACITY*sizeof(wchar_t));
    if (!input || !sdk->scratch) return false;
    SdkDirectory *directory=sdk_allocate(sdk,sizeof(*directory));
    if (!directory) return false;
    HANDLE handle=CreateFileW(input,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ,NULL,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
    if (handle==INVALID_HANDLE_VALUE) return sdk_windows_error(sdk,GetLastError());
    directory->handle=handle;directory->next=sdk->directories;sdk->directories=directory;
    if (!sdk_handle_check(sdk,handle,NULL,true)) return false;
    size_t count=wcslen(sdk->scratch);
    if (count<7 || wcsncmp(sdk->scratch,L"\\\\?\\",4) || sdk->scratch[5]!=':')
        return sdk_fail(sdk,XR_XIR_SDK_UNSUPPORTED);
    sdk->root_wide=sdk_allocate(sdk,(count+1)*sizeof(wchar_t));
    if (!sdk->root_wide) return false;
    memcpy(sdk->root_wide,sdk->scratch,(count+1)*sizeof(wchar_t));directory->path=sdk->root_wide;
    wchar_t *ancestor=sdk_allocate(sdk,(count+1)*sizeof(wchar_t));
    if (!ancestor) return false;
    memcpy(ancestor,sdk->root_wide,(count+1)*sizeof(wchar_t));
    for (size_t i=7;i<count;++i) {
        if (ancestor[i]!=L'\\') continue;
        ancestor[i]=0;bool admitted=sdk_directory_hold(sdk,ancestor);ancestor[i]=L'\\';
        if (!admitted) return false;
    }
    if (!sdk_handle_check(sdk,handle,sdk->root_wide,true)) return false;
    sdk->root=sdk_utf8(sdk,sdk->root_wide+4);return sdk->root!=NULL;
}
static wchar_t *sdk_file_path(XrXirRuntimeSdk *sdk,const char *relative) {
    size_t root_length=wcslen(sdk->root_wide),length=strlen(relative);
    if (root_length+length+2>SDK_WIDE_CAPACITY) {sdk_fail(sdk,XR_XIR_SDK_BUDGET);return NULL;}
    wchar_t *path=sdk_allocate(sdk,(root_length+length+2)*sizeof(wchar_t));
    if (!path || !sdk_work(sdk,length)) return NULL;
    memcpy(path,sdk->root_wide,root_length*sizeof(wchar_t));path[root_length]='\\';
    for (size_t i=0;i<length;++i) {
        unsigned char byte=(unsigned char)relative[i];
        if (byte>=128) {sdk_fail(sdk,XR_XIR_SDK_INVALID);return NULL;}
        path[root_length+i+1]=byte=='/' ? L'\\' : (wchar_t)byte;
    }
    for (size_t i=root_length+1;i<root_length+length+1;++i) {
        if (path[i]!=L'\\') continue;
        path[i]=0;bool admitted=sdk_directory_hold(sdk,path);path[i]=L'\\';
        if (!admitted) return NULL;
    }
    return path;
}
static bool sdk_file_hash(XrXirRuntimeSdk *sdk,XrXirSdkFile *file) {
    LARGE_INTEGER size={0};
    if (!GetFileSizeEx(file->handle,&size)) return sdk_windows_error(sdk,GetLastError());
    if (size.QuadPart<0 || (uint64_t)size.QuadPart!=file->length) return sdk_fail(sdk,XR_XIR_SDK_INVALID);
    XrSHA256Context hash;xr_sha256_init(&hash);uint64_t left=file->length;
    while (left) {
        DWORD count=(DWORD)(left>65536 ? 65536 : left),actual=0;
        if (!sdk_work(sdk,(uint64_t)count*2)) return false;
        if (!ReadFile(file->handle,sdk->scratch,count,&actual,NULL)) return sdk_windows_error(sdk,GetLastError());
        if (actual!=count) return sdk_fail(sdk,XR_XIR_SDK_IO);
        xr_sha256_update(&hash,(const uint8_t *)sdk->scratch,count);left-=count;
    }
    uint8_t digest[32];xr_sha256_final(&hash,digest);
    return !memcmp(digest,file->digest,32) || sdk_fail(sdk,XR_XIR_SDK_INVALID);
}
static bool sdk_files_admit(XrXirRuntimeSdk *sdk,const char *root) {
    if (!sdk_root_admit(sdk,root)) return false;
    for (uint32_t i=0;i<sdk->manifest.file_count;++i) {
        XrXirSdkFile *file=&sdk->manifest.files[i];wchar_t *path=sdk_file_path(sdk,file->path);
        if (!path) return false;
        HANDLE handle=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if (handle==INVALID_HANDLE_VALUE) return sdk_windows_error(sdk,GetLastError());
        file->handle=handle;
        if (!sdk_handle_check(sdk,handle,path,false) || !sdk_file_hash(sdk,file)) return false;
        file->absolute_path=sdk_utf8(sdk,path+4);
        if (!file->absolute_path) return false;
    }
    return true;
}
static void sdk_files_close(XrXirRuntimeSdk *sdk) {
    for (uint32_t i=0;i<sdk->manifest.file_count;++i)
        if (sdk->manifest.files[i].handle)
            XR_CHECK(CloseHandle(sdk->manifest.files[i].handle),"Owned SDK file handle could not be closed");
    for (SdkDirectory *directory=sdk->directories;directory;directory=directory->next)
        if (directory->handle) XR_CHECK(CloseHandle(directory->handle),"Owned SDK directory handle could not be closed");
}
