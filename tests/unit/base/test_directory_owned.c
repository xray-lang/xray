/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_directory_owned.c - Real names, cursor retirement and retained text
 *
 * KEY CONCEPT:
 *   Native directory contents are the oracle. Injected errors are distinguished
 *   from real OS results and never replace a successful filesystem operation.
 */
#include "os/os_directory_owned.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"owned directory %d: %s\n",__LINE__,#c);return 0;}}while(0)
typedef union DirectoryBlock {long double alignment;void *pointer;size_t bytes;} DirectoryBlock;
typedef struct DirectoryMemory {size_t attempts,fail_at,live,held,limit;} DirectoryMemory;
typedef struct TestDirectoryQuery {bool active,cancelled;} TestDirectoryQuery;
static DirectoryMemory directory_memory_state;
static XrOsIoStatus test_directory_allocate(void *context,size_t bytes,void **output){
 DirectoryMemory *memory=context;if(!output||*output||bytes>SIZE_MAX-sizeof(DirectoryBlock))return XR_OS_IO_BAD_ARGUMENT;
 if(memory->attempts++==memory->fail_at)return XR_OS_IO_OUT_OF_MEMORY;
 if(memory->held>memory->limit||bytes>memory->limit-memory->held)return XR_OS_IO_MEMORY_LIMIT;
 DirectoryBlock *block=xr_malloc(sizeof(*block)+bytes);if(!block)return XR_OS_IO_OUT_OF_MEMORY;
 block->bytes=bytes;++memory->live;memory->held+=bytes;*output=block+1;return XR_OS_IO_OK;
}
static void test_directory_free(void *context,void *pointer){
 if(!pointer)return;
 DirectoryMemory *memory=context;
 DirectoryBlock *block=(DirectoryBlock *)pointer-1;
 if(!memory->live||block->bytes>memory->held)abort();
 --memory->live;
 memory->held-=block->bytes;
 xr_free(block);
}
static bool test_directory_current(void *context){
 TestDirectoryQuery *query=context;if(!query||!query->active)abort();return !query->cancelled;
}
static XrOsIoPolicy directory_policy(void){return (XrOsIoPolicy){&directory_memory_state,test_directory_allocate,test_directory_free};}
static void directory_reset(void){
 if(directory_memory_state.live||directory_memory_state.held)abort();
 directory_memory_state=(DirectoryMemory){.fail_at=SIZE_MAX,.limit=SIZE_MAX};
}
#if defined(XR_OS_WINDOWS)
#include "base/xwindows_utf8.h"
#include <wchar.h>
static unsigned directory_handles,directory_fault;
static bool directory_more_data,directory_close_failure;
static TestDirectoryQuery *directory_cancel_open;
enum { DIRECTORY_REAL,DIRECTORY_ODD_NAME,DIRECTORY_BAD_OFFSET,DIRECTORY_BAD_UTF,DIRECTORY_IO_ERROR };
static HANDLE WINAPI directory_create(LPCWSTR path,DWORD access,DWORD sharing,LPSECURITY_ATTRIBUTES security,
 DWORD creation,DWORD flags,HANDLE template_file){
 HANDLE handle=CreateFileW(path,access,sharing,security,creation,flags,template_file);
 if(handle!=INVALID_HANDLE_VALUE){++directory_handles;if(directory_cancel_open)directory_cancel_open->cancelled=true;}
 return handle;
}
static BOOL WINAPI directory_close(HANDLE handle){
 /* A provider refusal before CloseHandle leaves the actual handle live. */
 if(directory_close_failure){directory_close_failure=false;SetLastError(ERROR_INVALID_HANDLE);return FALSE;}
 BOOL closed=CloseHandle(handle);if(closed){if(!directory_handles)abort();--directory_handles;}return closed;
}
static BOOL WINAPI directory_information(HANDLE handle,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID output,DWORD bytes){
 if(kind==FileIdBothDirectoryInfo||kind==FileIdBothDirectoryRestartInfo){
  /* More-data is injected before the OS call, preserving its real cursor. */
  if(directory_more_data){directory_more_data=false;SetLastError(ERROR_MORE_DATA);return FALSE;}
  if(directory_fault==DIRECTORY_IO_ERROR){SetLastError(ERROR_SUCCESS);return FALSE;}
 }
 BOOL result=GetFileInformationByHandleEx(handle,kind,output,bytes);
 if(result&&(kind==FileIdBothDirectoryInfo||kind==FileIdBothDirectoryRestartInfo)){
  FILE_ID_BOTH_DIR_INFO *entry=output;
  if(directory_fault==DIRECTORY_ODD_NAME)entry->FileNameLength=1;
  if(directory_fault==DIRECTORY_BAD_OFFSET)entry->NextEntryOffset=4;
  if(directory_fault==DIRECTORY_BAD_UTF){entry->NextEntryOffset=0;entry->FileNameLength=2;entry->FileName[0]=(WCHAR)0xD800;}
 }
 return result;
}
#define CreateFileW directory_create
#define CloseHandle directory_close
#define GetFileInformationByHandleEx directory_information
#include "os/win/directory_owned_win.c"
#undef GetFileInformationByHandleEx
#undef CloseHandle
#undef CreateFileW
static int directory_open_real(const char *path,XrOsDirectoryOwner *owner){
 XrOsIoPolicy policy=directory_policy();TestDirectoryQuery query={true,false};bool opened=false;
 XrOsDirectoryRequest request={&policy,{path,strlen(path)},&query,test_directory_current};
 CHECK(xr_os_io_directory_open(&request,owner,&opened)==XR_OS_IO_OK&&opened&&owner->state);
 /* Subsequent requests and close cannot retain this synchronous context. */
 query.active=false;return 1;
}
static int directory_names(const char *root,const char *long_name){
 XrOsIoPolicy policy=directory_policy();XrOsDirectoryOwner owner={0};CHECK(directory_open_real(root,&owner));
 TestDirectoryQuery query={true,false};bool seen[82]={false},injected=false;unsigned count=0;
 XrOsDirectoryText retained={0};
 for(;;){
  XrOsDirectoryText name={0};XrOsIoStatus status=xr_os_io_directory_next(&owner,&query,test_directory_current,&name);
  if(status==XR_OS_IO_END){CHECK(!name.bytes&&!name.length);break;}
  CHECK(status==XR_OS_IO_OK&&name.bytes&&name.length==strlen(name.bytes));unsigned index=UINT32_MAX;
  if(!strcmp(name.bytes,long_name))index=80;
  else if(!strcmp(name.bytes,"nested"))index=81;
  else{unsigned value=99;char tail=0;CHECK(sscanf_s(name.bytes,"entry-%u%c",&value,&tail,1)==1&&value<80);index=value;}
  CHECK(index<82&&!seen[index]);seen[index]=true;++count;
  if(!retained.bytes)retained=name;else policy.free(policy.context,name.bytes);
  if(!injected){directory_more_data=true;injected=true;}
 }
 CHECK(count==82&&!directory_more_data);for(unsigned n=0;n<82;++n)CHECK(seen[n]);
 XrOsDirectoryText empty={0};CHECK(xr_os_io_directory_next(&owner,&query,test_directory_current,&empty)==XR_OS_IO_END&&!empty.bytes);
 query.active=false;CHECK(xr_os_io_directory_close(&owner)==XR_OS_IO_OK&&!owner.state&&!directory_handles);
 CHECK(retained.bytes&&retained.length==strlen(retained.bytes));policy.free(policy.context,retained.bytes);
 CHECK(!directory_memory_state.live&&!directory_memory_state.held);return 1;
}
static int directory_faults(const char *root){
 XrOsIoPolicy policy=directory_policy();XrOsDirectoryOwner owner={0};TestDirectoryQuery query={true,false};
 XrOsDirectoryRequest request={&policy,{root,strlen(root)},&query,test_directory_current};bool opened=true;
 directory_memory_state.fail_at=directory_memory_state.attempts;
 CHECK(xr_os_io_directory_open(&request,&owner,&opened)==XR_OS_IO_OUT_OF_MEMORY&&opened&&!owner.state&&!directory_handles);
 directory_reset();CHECK(directory_open_real(root,&owner));directory_memory_state.fail_at=directory_memory_state.attempts;
 XrOsDirectoryText output={0};CHECK(xr_os_io_directory_next(&owner,&query,test_directory_current,&output)==XR_OS_IO_OUT_OF_MEMORY&&!output.bytes&&!output.length);
 CHECK(xr_os_io_directory_close(&owner)==XR_OS_IO_OK&&!directory_handles);directory_reset();
 CHECK(directory_open_real(root,&owner));directory_memory_state.limit=directory_memory_state.held;
 CHECK(xr_os_io_directory_next(&owner,&query,test_directory_current,&output)==XR_OS_IO_MEMORY_LIMIT&&!output.bytes&&!output.length);
 CHECK(xr_os_io_directory_close(&owner)==XR_OS_IO_OK&&!directory_handles);directory_reset();
 directory_cancel_open=&query;directory_close_failure=true;opened=true;
 CHECK(xr_os_io_directory_open(&request,&owner,&opened)==XR_OS_IO_CANCELLED&&opened&&owner.state&&directory_handles==1);
 directory_cancel_open=NULL;query.active=false;CHECK(xr_os_io_directory_close(&owner)==XR_OS_IO_OK&&!owner.state&&!directory_handles);directory_reset();
 for(unsigned fault=DIRECTORY_ODD_NAME;fault<=DIRECTORY_IO_ERROR;++fault){
  query=(TestDirectoryQuery){true,false};CHECK(directory_open_real(root,&owner));directory_fault=fault;
  CHECK(xr_os_io_directory_next(&owner,&query,test_directory_current,&output)==(fault==DIRECTORY_IO_ERROR?XR_OS_IO_IO:XR_OS_IO_BAD_ARGUMENT));
  CHECK(!output.bytes&&!output.length);directory_fault=DIRECTORY_REAL;
  CHECK(xr_os_io_directory_close(&owner)==XR_OS_IO_OK&&!directory_handles);directory_reset();
 }
 query=(TestDirectoryQuery){true,false};CHECK(directory_open_real(root,&owner));query.cancelled=true;
 CHECK(xr_os_io_directory_next(&owner,&query,test_directory_current,&output)==XR_OS_IO_CANCELLED&&!output.bytes);
 query.active=false;directory_close_failure=true;
 CHECK(xr_os_io_directory_close(&owner)==XR_OS_IO_IO&&owner.state&&directory_handles==1);
 CHECK(xr_os_io_directory_next(&owner,NULL,NULL,&output)==XR_OS_IO_BAD_ARGUMENT&&!output.bytes);
 CHECK(xr_os_io_directory_close(&owner)==XR_OS_IO_OK&&!owner.state&&!directory_handles);directory_reset();
 query=(TestDirectoryQuery){true,false};CHECK(directory_open_real(root,&owner));
 XrOsDirectoryText poison={(char *)(uintptr_t)1,99};
 CHECK(xr_os_io_directory_next(&owner,&query,test_directory_current,&poison)==XR_OS_IO_BAD_ARGUMENT&&poison.bytes==(char *)(uintptr_t)1&&poison.length==99);
 CHECK(xr_os_io_directory_close(&owner)==XR_OS_IO_OK&&!directory_handles);directory_reset();
 query=(TestDirectoryQuery){true,false};request.path=(XrOsPathSpan){"x\0y",3};opened=true;
 CHECK(xr_os_io_directory_open(&request,&owner,&opened)==XR_OS_IO_BAD_ARGUMENT&&opened&&!owner.state);
 request.path=(XrOsPathSpan){"\xFF",1};CHECK(xr_os_io_directory_open(&request,&owner,&opened)==XR_OS_IO_BAD_ARGUMENT&&opened&&!owner.state);
 CHECK(!directory_memory_state.live&&!directory_memory_state.held);return 1;
}
static int directory_windows(void){
 wchar_t root[4096],base[4096],parent[4096],leaf[4096],long_name[128];DWORD length=GetTempPathW(3000,base);
 CHECK(length&&length<3000);CHECK(swprintf_s(base+length,4096-length,L"xray-directory-%lu-\x6d4b\x8bd5",GetCurrentProcessId())>0);
 CHECK((base[0]==L'\\'&&base[1]==L'\\'?swprintf_s(root,4096,L"\\\\?\\UNC\\%ls",base+2):
  swprintf_s(root,4096,L"\\\\?\\%ls",base))>0&&CreateDirectoryW(root,NULL));wcscpy_s(base,4096,root);
 for(unsigned n=0;n<120;++n)long_name[n]=L'\x4e2d';long_name[120]=0;
 CHECK(swprintf_s(leaf,4096,L"%ls\\%ls",root,long_name)>0&&CreateDirectoryW(leaf,NULL));wcscpy_s(parent,4096,leaf);
 CHECK(swprintf_s(root,4096,L"%ls\\%ls",parent,long_name)>0&&CreateDirectoryW(root,NULL));CHECK(wcslen(root)>260);
 char path[16384],long_text[1024];CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,root,-1,path,sizeof(path),NULL,NULL)>0);
 CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,long_name,-1,long_text,sizeof(long_text),NULL,NULL)>0&&strlen(long_text)>260);
 for(unsigned n=0;n<80;++n){CHECK(swprintf_s(leaf,4096,L"%ls\\entry-%02u",root,n)>0);
  HANDLE file=CreateFileW(leaf,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(file!=INVALID_HANDLE_VALUE&&CloseHandle(file));}
 CHECK(swprintf_s(leaf,4096,L"%ls\\%ls",root,long_name)>0);HANDLE file=CreateFileW(leaf,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 CHECK(file!=INVALID_HANDLE_VALUE&&CloseHandle(file));CHECK(swprintf_s(leaf,4096,L"%ls\\nested",root)>0&&CreateDirectoryW(leaf,NULL));
 CHECK(directory_names(path,long_text)&&directory_faults(path));
 XrOsDirectoryOwner owner={0};XrOsIoPolicy policy=directory_policy();TestDirectoryQuery query={true,false};
 char empty[16384];CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,leaf,-1,empty,sizeof(empty),NULL,NULL)>0);
 CHECK(directory_open_real(empty,&owner));XrOsDirectoryText text={0};CHECK(xr_os_io_directory_next(&owner,&query,test_directory_current,&text)==XR_OS_IO_END&&!text.bytes);
 CHECK(xr_os_io_directory_close(&owner)==XR_OS_IO_OK&&RemoveDirectoryW(leaf));
 CHECK(swprintf_s(leaf,4096,L"%ls\\entry-00",root)>0&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,leaf,-1,empty,sizeof(empty),NULL,NULL)>0);
 bool file_opened=true;XrOsDirectoryRequest regular={&policy,{empty,strlen(empty)},&query,test_directory_current};
 CHECK(xr_os_io_directory_open(&regular,&owner,&file_opened)==XR_OS_IO_OK&&!file_opened&&!owner.state);
 char absent[16384];CHECK(snprintf(absent,sizeof(absent),"%s\\absent",path)>0);
 bool opened=true;XrOsDirectoryRequest missing={&policy,{absent,strlen(absent)},&query,test_directory_current};
 CHECK(xr_os_io_directory_open(&missing,&owner,&opened)==XR_OS_IO_OK&&!opened&&!owner.state);
 for(unsigned n=0;n<80;++n){CHECK(swprintf_s(leaf,4096,L"%ls\\entry-%02u",root,n)>0&&DeleteFileW(leaf));}
 CHECK(swprintf_s(leaf,4096,L"%ls\\%ls",root,long_name)>0&&DeleteFileW(leaf));
 CHECK(RemoveDirectoryW(root)&&RemoveDirectoryW(parent)&&RemoveDirectoryW(base));
 CHECK(!directory_handles&&!directory_memory_state.live&&!directory_memory_state.held);
 puts("Owned directory: real long Unicode names, EOF, retained text and close-only retry; provider faults isolated");return 1;
}
#elif defined(XR_OS_LINUX) || defined(XR_OS_MACOS)
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(XR_OS_LINUX)
#include <dirent.h>
#else
#include <sys/attr.h>
#endif

static unsigned directory_reads;
static unsigned directory_closes;
static bool directory_consumed_error;
static int directory_read_error;
static TestDirectoryQuery *directory_cancel_read;

static int directory_test_close(int descriptor) {
    ++directory_closes;
    int status = close(descriptor);
    if (status == 0 && directory_consumed_error) {
        directory_consumed_error = false;
        errno = EIO;
        return -1;
    }
    return status;
}

#if defined(XR_OS_LINUX)
static ssize_t directory_test_read(int descriptor, void *buffer, size_t bytes) {
    ++directory_reads;
    if (directory_read_error) { errno = directory_read_error; return -1; }
    ssize_t status = getdents64(descriptor, buffer, bytes);
    if (status >= 0 && directory_cancel_read) directory_cancel_read->cancelled = true;
    return status;
}
#define getdents64 directory_test_read
#else
static int directory_test_read(int descriptor, struct attrlist *attributes,
    void *buffer, size_t bytes, uint64_t options) {
    ++directory_reads;
    if (directory_read_error) { errno = directory_read_error; return -1; }
    int status = getattrlistbulk(descriptor, attributes, buffer, bytes, options);
    if (status >= 0 && directory_cancel_read) directory_cancel_read->cancelled = true;
    return status;
}
#define getattrlistbulk directory_test_read
#endif
#define close directory_test_close
#include "os/unix/directory_owned_unix.c"
#undef close
#if defined(XR_OS_LINUX)
#undef getdents64
#else
#undef getattrlistbulk
#endif

enum { UNIX_DIRECTORY_NAMES = 160 };

static void directory_entry_name(char *buffer, size_t capacity, unsigned number) {
    int size = snprintf(buffer, capacity, "entry-%03u-abcdefghijklmnopqrstuvwxyz", number);
    if (size < 0 || (size_t)size >= capacity) abort();
}

static void directory_entry_path(char *buffer, size_t capacity, const char *root, const char *name) {
    int size = snprintf(buffer, capacity, "%s/%s", root, name);
    if (size < 0 || (size_t)size >= capacity) abort();
}

static int directory_open_real(const char *path, XrOsDirectoryOwner *owner) {
    XrOsIoPolicy policy = directory_policy();
    TestDirectoryQuery query = {true, false};
    XrOsDirectoryRequest request = {&policy, {path, strlen(path)}, &query, test_directory_current};
    bool opened = false;
    CHECK(xr_os_io_directory_open(&request, owner, &opened) == XR_OS_IO_OK && opened && owner->state);
    return 1;
}

static int directory_unix_names(const char *path, const char *unicode) {
    XrOsDirectoryOwner owner = {0};
    XrOsIoPolicy policy = directory_policy();
    TestDirectoryQuery query = {true, false};
    CHECK(directory_open_real(path, &owner));

    unsigned before = directory_reads;
    bool seen[UNIX_DIRECTORY_NAMES] = {0};
    XrOsDirectoryText retained = {0};
    size_t read = 0;
    for (;;) {
        XrOsDirectoryText name = {0};
        XrOsIoStatus status = xr_os_io_directory_next(&owner, &query, test_directory_current, &name);
        if (status == XR_OS_IO_END) break;
        CHECK(status == XR_OS_IO_OK && name.bytes && name.length == strlen(name.bytes));
        ++read;
        if (!strcmp(name.bytes, unicode)) {
            CHECK(!retained.bytes);
            retained = name;
        } else {
            unsigned number;
            char expected[64];
            CHECK(sscanf(name.bytes, "entry-%u", &number) == 1 && number < UNIX_DIRECTORY_NAMES);
            directory_entry_name(expected, sizeof(expected), number);
            CHECK(!strcmp(name.bytes, expected) && !seen[number]);
            seen[number] = true;
            policy.free(policy.context, name.bytes);
        }
    }
    CHECK(read == UNIX_DIRECTORY_NAMES + 1 && directory_reads >= before + 3);
    XrOsDirectoryText empty = {0};
    before = directory_reads;
    CHECK(xr_os_io_directory_next(&owner, &query, test_directory_current, &empty) == XR_OS_IO_END &&
        directory_reads == before && !empty.bytes);
    CHECK(xr_os_io_directory_close(&owner) == XR_OS_IO_OK && !owner.state);
    CHECK(retained.bytes && retained.length == strlen(unicode) && !strcmp(retained.bytes, unicode));
    policy.free(policy.context, retained.bytes);
    directory_reset();
    return 1;
}

static int directory_unix_faults(const char *path) {
    XrOsDirectoryOwner owner = {0};
    XrOsDirectoryText name = {0};
    TestDirectoryQuery query = {true, false};
    CHECK(directory_open_real(path, &owner));
    directory_read_error = EIO;
    CHECK(xr_os_io_directory_next(&owner, &query, test_directory_current, &name) == XR_OS_IO_IO &&
        !name.bytes && !name.length);
    directory_read_error = 0;
    unsigned before = directory_reads;
    CHECK(xr_os_io_directory_next(&owner, &query, test_directory_current, &name) == XR_OS_IO_IO &&
        directory_reads == before);
    CHECK(xr_os_io_directory_close(&owner) == XR_OS_IO_OK);
    directory_reset();

    CHECK(directory_open_real(path, &owner));
    directory_memory_state.fail_at = directory_memory_state.attempts + 1;
    CHECK(xr_os_io_directory_next(&owner, &query, test_directory_current, &name) == XR_OS_IO_OUT_OF_MEMORY &&
        !name.bytes && !name.length);
    CHECK(xr_os_io_directory_close(&owner) == XR_OS_IO_OK);
    directory_reset();

    CHECK(directory_open_real(path, &owner));
    directory_memory_state.limit = directory_memory_state.held;
    CHECK(xr_os_io_directory_next(&owner, &query, test_directory_current, &name) == XR_OS_IO_MEMORY_LIMIT &&
        !name.bytes && !name.length);
    CHECK(xr_os_io_directory_close(&owner) == XR_OS_IO_OK);
    directory_reset();

    CHECK(directory_open_real(path, &owner));
    directory_cancel_read = &query;
    CHECK(xr_os_io_directory_next(&owner, &query, test_directory_current, &name) == XR_OS_IO_CANCELLED &&
        !name.bytes && !name.length);
    directory_cancel_read = NULL;
    CHECK(xr_os_io_directory_close(&owner) == XR_OS_IO_OK);
    directory_reset();

    CHECK(directory_open_real(path, &owner));
    int retired = ((DirectoryOwned *)owner.state)->descriptor;
    before = directory_closes;
    directory_consumed_error = true;
    CHECK(xr_os_io_directory_close(&owner) == XR_OS_IO_IO && !owner.state && directory_closes == before + 1);
    int replacement = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(replacement == retired);
    CHECK(xr_os_io_directory_close(&owner) == XR_OS_IO_OK && directory_closes == before + 1);
    CHECK(fcntl(replacement, F_GETFD) >= 0 && close(replacement) == 0);
    directory_reset();
    return 1;
}

static int directory_unix_edges(const char *root) {
    XrOsDirectoryOwner owner = {0};
    XrOsIoPolicy policy = directory_policy();
    TestDirectoryQuery query = {true, false};
    char path[1024];
    directory_entry_path(path, sizeof(path), root, "empty");
    CHECK(mkdir(path, 0700) == 0);
    CHECK(directory_open_real(path, &owner));
    XrOsDirectoryText name = {0};
    CHECK(xr_os_io_directory_next(&owner, &query, test_directory_current, &name) == XR_OS_IO_END &&
        !name.bytes && !name.length);
    CHECK(xr_os_io_directory_close(&owner) == XR_OS_IO_OK && rmdir(path) == 0);
    XrOsDirectoryRequest request = {&policy, {path, strlen(path)}, &query, test_directory_current};
    bool opened = true;
    CHECK(xr_os_io_directory_open(&request, &owner, &opened) == XR_OS_IO_OK && !opened && !owner.state);

    char entry[64];
    directory_entry_name(entry, sizeof(entry), 0);
    directory_entry_path(path, sizeof(path), root, entry);
    request.path = (XrOsPathSpan){path, strlen(path)};
    opened = true;
    CHECK(xr_os_io_directory_open(&request, &owner, &opened) == XR_OS_IO_OK && !opened && !owner.state);
    request.path = (XrOsPathSpan){"x\0y", 3};
    opened = true;
    CHECK(xr_os_io_directory_open(&request, &owner, &opened) == XR_OS_IO_BAD_ARGUMENT && opened && !owner.state);
    directory_reset();

#if defined(XR_OS_LINUX)
    directory_entry_path(path, sizeof(path), root, "removed");
    CHECK(mkdir(path, 0700) == 0 && directory_open_real(path, &owner) && rmdir(path) == 0);
    CHECK(xr_os_io_directory_next(&owner, &query, test_directory_current, &name) == XR_OS_IO_NOT_FOUND &&
        !name.bytes && !name.length);
    CHECK(xr_os_io_directory_close(&owner) == XR_OS_IO_OK);
    directory_reset();

    directory_entry_path(path, sizeof(path), root, "invalid-utf8");
    CHECK(mkdir(path, 0700) == 0);
    char invalid[1024];
    directory_entry_path(invalid, sizeof(invalid), path, "file-\377");
    int file = open(invalid, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(file >= 0 && close(file) == 0 && directory_open_real(path, &owner));
    CHECK(xr_os_io_directory_next(&owner, &query, test_directory_current, &name) == XR_OS_IO_BAD_ARGUMENT &&
        !name.bytes && !name.length);
    CHECK(xr_os_io_directory_close(&owner) == XR_OS_IO_OK && unlink(invalid) == 0 && rmdir(path) == 0);
    directory_reset();
#endif
    return 1;
}

static int directory_unix(void) {
    char root[] = "/tmp/xray-owned-directory-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char path[1024];
    char unicode[226] = {0};
    for (size_t i = 0; i < 75; ++i) memcpy(unicode + i * 3, "\xE4\xB8\xAD", 3);
    directory_entry_path(path, sizeof(path), root, unicode);
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(file >= 0 && close(file) == 0);
    for (unsigned i = 0; i < UNIX_DIRECTORY_NAMES; ++i) {
        char name[64];
        directory_entry_name(name, sizeof(name), i);
        directory_entry_path(path, sizeof(path), root, name);
        file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        CHECK(file >= 0 && close(file) == 0);
    }
    CHECK(directory_unix_names(root, unicode) && directory_unix_faults(root) && directory_unix_edges(root));
    for (unsigned i = 0; i < UNIX_DIRECTORY_NAMES; ++i) {
        char name[64];
        directory_entry_name(name, sizeof(name), i);
        directory_entry_path(path, sizeof(path), root, name);
        CHECK(unlink(path) == 0);
    }
    directory_entry_path(path, sizeof(path), root, unicode);
    CHECK(unlink(path) == 0 && rmdir(root) == 0);
    puts("Owned directory: real Unicode names, multiple batches, EOF, retained text and consumed close errors");
    return 1;
}
#else
#include "os/unix/directory_owned_unix.c"
static int directory_unix_boundary(void){
 XrOsIoPolicy policy=directory_policy();XrOsDirectoryOwner owner={0};TestDirectoryQuery query={true,false};
 XrOsDirectoryRequest request={&policy,{".",1},&query,test_directory_current};bool opened=true;
 CHECK(xr_os_io_directory_open(&request,&owner,&opened)==XR_OS_IO_UNSUPPORTED&&opened&&!owner.state);
 XrOsDirectoryText output={0};CHECK(xr_os_io_directory_next(&owner,&query,test_directory_current,&output)==XR_OS_IO_UNSUPPORTED&&!output.bytes);
 CHECK(xr_os_io_directory_close(&owner)==XR_OS_IO_OK&&!directory_memory_state.attempts);
 puts("Owned directory Unix boundary: unsupported before acquisition");return 1;
}
#endif
int main(void){
 directory_reset();
#if defined(XR_OS_WINDOWS)
 return directory_windows()?0:1;
#elif defined(XR_OS_LINUX) || defined(XR_OS_MACOS)
 return directory_unix()?0:1;
#else
 return directory_unix_boundary()?0:1;
#endif
}
