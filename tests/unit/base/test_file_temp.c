/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_file_temp.c - Exclusive creation and retained rollback responsibility
 *
 * KEY CONCEPT:
 *   Fault injection changes a real OS boundary, never the expected object.
 *   Prepared names remain private until explicit successful commitment.
 */
#define main metadata_fixture_main
#include "test_file_metadata.c"
#undef main
#include "os/os_file_temp.h"
#include "os/os_random.h"
static char *temp_prepared;
static char temp_candidate[65536];
static unsigned temp_prepare_mode;
enum { TEMP_PREPARE_NORMAL,TEMP_PREPARE_OOM,TEMP_PREPARE_CANCEL };
static void temp_drop_prepared(void) {
    if(temp_prepared)test_free(&metadata_memory,temp_prepared);
    temp_prepared=NULL;
}
static XrOsIoStatus temp_prepare(void *context,const char *text,size_t length) {
    if(context!=&metadata_memory || length>=sizeof(temp_candidate))return XR_OS_IO_BAD_ARGUMENT;
    memcpy(temp_candidate,text,length);temp_candidate[length]=0;temp_drop_prepared();
    if(temp_prepare_mode==TEMP_PREPARE_OOM)metadata_memory.fail_at=metadata_memory.attempts;
    void *memory=NULL;XrOsIoStatus status=test_allocate(context,length+1,&memory);
    metadata_memory.fail_at=SIZE_MAX;
    if(status!=XR_OS_IO_OK)return status;
    memcpy(memory,text,length+1);temp_prepared=memory;
    if(temp_prepare_mode==TEMP_PREPARE_CANCEL)metadata_memory.cancelled=true;
    return XR_OS_IO_OK;
}
#if defined(XR_OS_WINDOWS)
#include <winternl.h>
typedef NTSTATUS (NTAPI *TempActualCreate)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,
    PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
static TempActualCreate temp_actual_create;
static unsigned temp_entropy_mode,temp_entropy_calls;
static HANDLE temp_parent,temp_child,temp_fail_close;
static bool temp_cancel_created,temp_fail_delete,temp_missing_api;
static wchar_t temp_long_root_path[32768];
static bool temp_long_root_created;
static NTSTATUS NTAPI temp_observed_create(PHANDLE handle,ACCESS_MASK access,POBJECT_ATTRIBUTES attributes,
    PIO_STATUS_BLOCK status,PLARGE_INTEGER allocation,ULONG flags,ULONG share,ULONG disposition,
    ULONG options,PVOID ea,ULONG ea_length) {
    NTSTATUS result=temp_actual_create(handle,access,attributes,status,allocation,flags,share,disposition,options,ea,ea_length);
    if(result>=0 && *handle && status->Information==FILE_CREATED) {
        temp_parent=attributes->RootDirectory;temp_child=*handle;
        if(temp_cancel_created)metadata_memory.cancelled=true;
    }
    return result;
}
static FARPROC WINAPI temp_lookup(HMODULE module,LPCSTR name) {
    FARPROC actual=GetProcAddress(module,name);
    if(strcmp(name,"NtCreateFile"))return actual;
    if(temp_missing_api || !actual)return NULL;
    memcpy(&temp_actual_create,&actual,sizeof(actual));
    TempActualCreate observed=temp_observed_create;FARPROC output=NULL;
    _Static_assert(sizeof(observed)==sizeof(output),"Windows function address size");
    memcpy(&output,&observed,sizeof(output));return output;
}
static BOOL WINAPI temp_close(HANDLE handle) {
    if(handle==temp_fail_close){temp_fail_close=NULL;SetLastError(ERROR_INVALID_HANDLE);return FALSE;}
    return CloseHandle(handle);
}
static BOOL WINAPI temp_disposition(HANDLE handle,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID data,DWORD length) {
    if(temp_fail_delete && kind==FileDispositionInfo){temp_fail_delete=false;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return SetFileInformationByHandle(handle,kind,data,length);
}
static void temp_entropy(unsigned char *bytes,size_t length) {
    if(temp_entropy_mode==2 || (temp_entropy_mode==1 && temp_entropy_calls++==0))memset(bytes,0,length);
    else xr_random_bytes(bytes,length);
}
#define GetProcAddress temp_lookup
#define CloseHandle temp_close
#define SetFileInformationByHandle temp_disposition
#define xr_random_bytes temp_entropy
#include "os/win/file_temp_win.c"
#undef xr_random_bytes
#undef SetFileInformationByHandle
#undef CloseHandle
#undef GetProcAddress

static int temp_locator(const char *text,wchar_t *output) {
    wchar_t raw[32768];CHECK(xr_win_utf8_to_utf16(text,strlen(text),raw,32768));
    for(size_t n=0;raw[n];++n)if(raw[n]==L'/')raw[n]=L'\\';
    int length=swprintf(output,32768,L"\\\\?\\%ls",raw);CHECK(length>0&&length<32768);return 1;
}
static bool temp_exists(const char *name) {
    wchar_t locator[32768];if(!temp_locator(name,locator))return false;
    return GetFileAttributesW(locator)!=INVALID_FILE_ATTRIBUTES;
}
static int temp_create_case(const char *root,bool directory,XrOsFileTempOwner *owner,bool *created) {
    XrOsIoPolicy policy=test_policy();
    XrOsFileTempRequest request={&policy, {root,strlen(root)}, &metadata_memory,test_current,temp_prepare};
    XrOsIoStatus status=directory?xr_os_io_temp_directory(&request,owner,created):xr_os_io_temp_file(&request,owner,created);
    CHECK(status==XR_OS_IO_OK&&*created&&owner->state&&temp_prepared&&temp_exists(temp_prepared));
    DWORD flags=0;CHECK(GetHandleInformation(temp_child,&flags)&&!(flags&HANDLE_FLAG_INHERIT));
    CHECK(GetHandleInformation(temp_parent,&flags)&&!(flags&HANDLE_FLAG_INHERIT));return 1;
}
static int temp_visible(const char *root,bool directory) {
    XrOsIoPolicy policy=test_policy();XrOsFileTempOwner owner={0};bool created=false;
    CHECK(temp_create_case(root,directory,&owner,&created));wchar_t name[32768],other[32768];
    CHECK(temp_locator(temp_prepared,name));
    WIN32_FILE_ATTRIBUTE_DATA info;CHECK(GetFileAttributesExW(name,GetFileExInfoStandard,&info));
    CHECK(((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)==directory);
    if(!directory)CHECK(!info.nFileSizeHigh&&!info.nFileSizeLow);
    CHECK(swprintf(other,32768,L"%ls-moved",name)>0&&!MoveFileExW(name,other,0));
    CHECK(temp_exists(temp_prepared));
    CHECK(xr_os_io_temp_finish(&policy,&owner,true)==XR_OS_IO_OK&&!owner.state&&temp_exists(temp_prepared));
    DWORD flags=0;CHECK(!GetHandleInformation(temp_child,&flags));
    CHECK(directory?RemoveDirectoryW(name):DeleteFileW(name));temp_drop_prepared();
    CHECK(!metadata_memory.live&&!metadata_memory.held);
    CHECK(temp_create_case(root,directory,&owner,&created));
    CHECK(xr_os_io_temp_finish(&policy,&owner,false)==XR_OS_IO_OK&&!owner.state&&!temp_exists(temp_prepared));
    temp_drop_prepared();CHECK(!metadata_memory.live&&!metadata_memory.held);return 1;
}
static int temp_faults(const char *root,bool directory) {
    XrOsIoPolicy policy=test_policy();XrOsFileTempOwner owner={0};bool created=true;
    XrOsFileTempRequest request={&policy,{root,strlen(root)},&metadata_memory,test_current,temp_prepare};
    for(unsigned mode=TEMP_PREPARE_OOM;mode<=TEMP_PREPARE_CANCEL;++mode) {
        temp_prepare_mode=mode;created=true;
        XrOsIoStatus status=directory?xr_os_io_temp_directory(&request,&owner,&created):xr_os_io_temp_file(&request,&owner,&created);
        CHECK(status==(mode==TEMP_PREPARE_OOM?XR_OS_IO_OUT_OF_MEMORY:XR_OS_IO_CANCELLED)&&created&&!owner.state&&!temp_exists(temp_candidate));
        temp_drop_prepared();reset_memory();
    }
    temp_prepare_mode=TEMP_PREPARE_NORMAL;temp_cancel_created=true;created=false;
    XrOsIoStatus status=directory?xr_os_io_temp_directory(&request,&owner,&created):xr_os_io_temp_file(&request,&owner,&created);
    temp_cancel_created=false;
    CHECK(status==XR_OS_IO_CANCELLED&&!created&&!owner.state&&!temp_exists(temp_candidate));temp_drop_prepared();reset_memory();
    for(unsigned fault=0;fault<2;++fault) {
        CHECK(temp_create_case(root,directory,&owner,&created));temp_fail_close=fault?temp_child:temp_parent;
        CHECK(xr_os_io_temp_finish(&policy,&owner,true)==XR_OS_IO_IO&&owner.state&&temp_exists(temp_prepared));
        DWORD flags=0;CHECK(GetHandleInformation(temp_child,&flags));
        CHECK(xr_os_io_temp_finish(&policy,&owner,false)==XR_OS_IO_OK&&!owner.state&&!temp_exists(temp_prepared));temp_drop_prepared();
    }
    CHECK(temp_create_case(root,directory,&owner,&created));temp_fail_delete=true;
    CHECK(xr_os_io_temp_finish(&policy,&owner,false)==XR_OS_IO_IO&&owner.state&&temp_exists(temp_prepared));
    MetadataMemory foreign={0};XrOsIoPolicy wrong={&foreign,test_allocate,test_free};
    CHECK(xr_os_io_temp_finish(&wrong,&owner,false)==XR_OS_IO_BAD_ARGUMENT&&owner.state);
    CHECK(xr_os_io_temp_finish(&policy,&owner,false)==XR_OS_IO_OK&&!owner.state&&!temp_exists(temp_prepared));temp_drop_prepared();
    temp_missing_api=true;created=true;
    CHECK((directory?xr_os_io_temp_directory(&request,&owner,&created):xr_os_io_temp_file(&request,&owner,&created))==XR_OS_IO_UNSUPPORTED&&created&&!owner.state);
    temp_missing_api=false;CHECK(!metadata_memory.live&&!metadata_memory.held);return 1;
}
static int temp_collisions(const char *root,bool directory) {
    char collision[65536];CHECK(snprintf(collision,sizeof(collision),"%s/xr-00000000000000000000000000000000",root)>0);
    wchar_t name[32768];CHECK(temp_locator(collision,name));
    static const uint8_t bytes[]={42,0,255};CHECK(write_marker(name,bytes,sizeof(bytes)));
    XrOsIoPolicy policy=test_policy();XrOsFileTempOwner owner={0};bool created=true;
    XrOsFileTempRequest request={&policy,{root,strlen(root)},&metadata_memory,test_current,temp_prepare};
    temp_entropy_mode=2;
    CHECK((directory?xr_os_io_temp_directory(&request,&owner,&created):xr_os_io_temp_file(&request,&owner,&created))==XR_OS_IO_OK&&!created&&!owner.state&&temp_exists(collision));
    temp_drop_prepared();temp_entropy_mode=1;temp_entropy_calls=0;
    CHECK(temp_create_case(root,directory,&owner,&created)&&strcmp(temp_prepared,collision));
    CHECK(xr_os_io_temp_finish(&policy,&owner,false)==XR_OS_IO_OK&&!owner.state);temp_drop_prepared();temp_entropy_mode=0;
    HANDLE file=CreateFileW(name,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);CHECK(file!=INVALID_HANDLE_VALUE);
    uint8_t actual[3];DWORD read=0;CHECK(ReadFile(file,actual,sizeof(actual),&read,NULL)&&read==sizeof(bytes)&&!memcmp(actual,bytes,sizeof(bytes)));
    CHECK(CloseHandle(file)&&DeleteFileW(name)&&!metadata_memory.live&&!metadata_memory.held);
    CHECK(CreateDirectoryW(name,NULL));wchar_t protected_name[32768];
    CHECK(swprintf(protected_name,32768,L"%ls\\keep",name)>0&&write_marker(protected_name,bytes,sizeof(bytes)));
    temp_entropy_mode=2;created=true;
    CHECK((directory?xr_os_io_temp_directory(&request,&owner,&created):xr_os_io_temp_file(&request,&owner,&created))==XR_OS_IO_OK&&!created&&!owner.state);
    CHECK(GetFileAttributesW(protected_name)!=INVALID_FILE_ATTRIBUTES);temp_drop_prepared();temp_entropy_mode=0;
    CHECK(DeleteFileW(protected_name)&&RemoveDirectoryW(name)&&!metadata_memory.live&&!metadata_memory.held);return 1;
}
static int temp_long_positive(void) {
    enum { TEMP_LONG_PREFIX_UNITS=250,TEMP_LONG_SUFFIX_UNITS=5,TEMP_COMPONENT_MAX_UNITS=255 };
    _Static_assert(TEMP_LONG_PREFIX_UNITS+TEMP_LONG_SUFFIX_UNITS<=TEMP_COMPONENT_MAX_UNITS,
        "long temporary root component must fit the filesystem limit");
    wchar_t leaf[TEMP_LONG_PREFIX_UNITS+TEMP_LONG_SUFFIX_UNITS+1];char root[65536];
    for(size_t n=0;n<TEMP_LONG_PREFIX_UNITS;++n)leaf[n]=L'l';
    memcpy(leaf+TEMP_LONG_PREFIX_UNITS,L"-\x4e2d-\xd83d\xde00",(TEMP_LONG_SUFFIX_UNITS+1)*sizeof(wchar_t));
    CHECK(wcslen(leaf)==TEMP_LONG_PREFIX_UNITS+TEMP_LONG_SUFFIX_UNITS);
    CHECK(wide_join(temp_long_root_path,paths.child,leaf)&&wcslen(temp_long_root_path)>MAX_PATH);
    CHECK(directory_operation(temp_long_root_path,true));temp_long_root_created=true;
    CHECK(xr_win_utf16_to_utf8(temp_long_root_path,wcslen(temp_long_root_path),root,sizeof(root)));
    fprintf(stderr,"file temp actual long root UTF16=%zu\n",wcslen(temp_long_root_path));
    return temp_visible(root,false)&&temp_visible(root,true);
}
static int temp_drive_relative(void) {
    wchar_t previous[32768], absolute[32768], raw[32768];
    DWORD count=GetCurrentDirectoryW(32768,previous);
    CHECK(count && count<32768 && SetCurrentDirectoryW(paths.child));
    char drive[3]={(char)paths.child[0],':',0};
    XrOsIoPolicy policy=test_policy();
    for(unsigned directory=0;directory<2;++directory) {
        XrOsFileTempOwner owner={0};bool created=false;
        XrOsFileTempRequest request={&policy,{drive,2},&metadata_memory,test_current,temp_prepare};
        XrOsIoStatus status=directory?xr_os_io_temp_directory(&request,&owner,&created):
            xr_os_io_temp_file(&request,&owner,&created);
        CHECK(status==XR_OS_IO_OK && created && owner.state && temp_prepared);
        CHECK(!strncmp(temp_prepared,drive,2) && temp_prepared[2]=='x');
        CHECK(xr_win_utf8_to_utf16(temp_prepared,strlen(temp_prepared),raw,32768));
        count=GetFullPathNameW(raw,32768,absolute,NULL);
        CHECK(count && count<32768 && !wcsncmp(absolute,paths.child,wcslen(paths.child)));
        count=GetFinalPathNameByHandleW(temp_child,raw,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        CHECK(count && count<32768);
        const wchar_t *actual=!wcsncmp(raw,L"\\\\?\\",4)?raw+4:raw;
        const wchar_t *expected=!wcsncmp(absolute,L"\\\\?\\",4)?absolute+4:absolute;
        CHECK(CompareStringOrdinal(actual,-1,expected,-1,TRUE)==CSTR_EQUAL);
        CHECK(xr_os_io_temp_finish(&policy,&owner,false)==XR_OS_IO_OK && !owner.state);
        CHECK(GetFileAttributesW(absolute)==INVALID_FILE_ATTRIBUTES);
        temp_drop_prepared();
        CHECK(!metadata_memory.live && !metadata_memory.held);
    }
    CHECK(SetCurrentDirectoryW(previous));
    return 1;
}
int main(void) {
    reset_memory();int passed=setup_paths();char root[65536];
    if(passed)passed=xr_win_utf16_to_utf8(paths.child,wcslen(paths.child),root,sizeof(root));
    for(unsigned directory=0;directory<2&&passed;++directory)
        passed=temp_visible(root,directory!=0)&&temp_faults(root,directory!=0)&&temp_collisions(root,directory!=0);
    if(passed)passed=temp_long_positive()&&temp_drive_relative();
    if(temp_long_root_created) {
        bool removed=directory_operation(temp_long_root_path,false)!=0;
        if(removed)temp_long_root_created=false;
        if(!removed)passed=0;
    }
    temp_drop_prepared();cleanup_paths();
    if(metadata_memory.live||metadata_memory.held||kernel_handles)return 1;
    if(passed)puts("file temp: real exclusive file/directory creation, prepared results and retained cleanup");
    return passed?0:1;
}
#else
#include "os/unix/file_temp_unix.c"
int main(void) {
    reset_memory();XrOsIoPolicy policy=test_policy();XrOsFileTempOwner owner={0};bool created=true;
    XrOsFileTempRequest request={&policy,{"/tmp",4},&metadata_memory,test_current,temp_prepare};
    int passed=xr_os_io_temp_file(&request,&owner,&created)==XR_OS_IO_UNSUPPORTED&&created&&!owner.state&&
        xr_os_io_temp_directory(&request,&owner,&created)==XR_OS_IO_UNSUPPORTED&&created&&!owner.state&&
        xr_os_io_temp_finish(&policy,&owner,false)==XR_OS_IO_OK&&!temp_prepared&&!metadata_memory.live&&!metadata_memory.held;
    if(passed)puts("file temp: Unix unsupported boundary only; no creation qualification");return passed?0:1;
}
#endif
