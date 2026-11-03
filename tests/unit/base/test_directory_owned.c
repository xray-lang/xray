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
 if(!pointer)return;DirectoryMemory *memory=context;DirectoryBlock *block=(DirectoryBlock *)pointer-1;
 if(!memory->live||block->bytes>memory->held)abort();--memory->live;memory->held-=block->bytes;xr_free(block);
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
#else
 return directory_unix_boundary()?0:1;
#endif
}
