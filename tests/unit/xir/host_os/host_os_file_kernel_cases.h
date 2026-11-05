/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_file_kernel_cases.h - Real owned file bytes and close responsibility
 */
#ifndef HOST_OS_FILE_KERNEL_CASES_H
#define HOST_OS_FILE_KERNEL_CASES_H
#include "os/os_file_owned.h"
#if defined(XR_OS_WINDOWS)
static XrOsIoStatus host_file_memory(void *context,size_t bytes,void **output) {
 CHECK(context==&runtime_live&&output&&!*output);
 void *memory=runtime_malloc(bytes);if(!memory)return XR_OS_IO_OUT_OF_MEMORY;
 *output=memory;return XR_OS_IO_OK;
}
static void host_file_memory_free(void *context,void *memory) {
 CHECK(context==&runtime_live);runtime_free(memory);
}
static bool host_file_current(void *context) {return !*(bool *)context;}
static XrOsFileProgress host_file_terminal(XrOsFileOperation *operation) {
 ULONGLONG end=GetTickCount64()+5000;XrOsFileProgress result={0};
 do{CHECK(xr_os_io_file_poll(operation,&result)==XR_OS_IO_OK);if(result.terminal)return result;Sleep(0);}while(GetTickCount64()<end);
 CHECK(false);return result;
}
static void host_file_operation_done(XrOsFileOperation *operation) {
 bool closed=false;CHECK(xr_os_io_file_operation_close(operation,&closed)==XR_OS_IO_OK&&closed&&!operation->state);
}
static void host_file_opened(const XrOsIoPolicy *memory,const XrOsFileQuery *query,
 const char *path,bool writing,bool append,XrOsFileOwner *owner) {
 XrOsFileOperation operation={0};XrOsFileRequest request={.action=writing?XR_OS_FILE_OPEN_WRITE:XR_OS_FILE_OPEN_READ,
  .path=path,.path_length=strlen(path),.append=append,.standard_stream=-1};
 CHECK(xr_os_io_file_open_begin(memory,query,&request,owner,&operation)==XR_OS_IO_OK);
 XrOsFileProgress result=host_file_terminal(&operation);CHECK(result.status==XR_OS_IO_OK&&result.success);
 bool closed=true;CHECK(xr_os_io_file_close(owner,&closed)==XR_OS_IO_OK&&!closed);
 XrOsFileOwner copied=*owner;closed=true;
 CHECK(xr_os_io_file_close(&copied,&closed)==XR_OS_IO_BAD_ARGUMENT&&closed);
 XrOsFileOperation copied_operation=operation;XrOsFileProgress unchanged={.count=123};
 CHECK(xr_os_io_file_poll(&copied_operation,&unchanged)==XR_OS_IO_BAD_ARGUMENT&&unchanged.count==123);
 host_file_operation_done(&operation);
}
static void host_file_written(const XrOsIoPolicy *memory,const XrOsFileQuery *query,
 XrOsFileOwner *owner,const uint8_t *bytes,size_t length) {
 XrOsFileOperation operation={0};XrOsFileRequest request={.action=XR_OS_FILE_WRITE,
  .standard_stream=-1,.bytes=bytes,.length=length};
 CHECK(xr_os_io_file_begin(memory,query,&request,owner,&operation)==XR_OS_IO_OK);
 XrOsFileProgress result=host_file_terminal(&operation);CHECK(result.status==XR_OS_IO_OK&&result.success&&result.count==length);
 host_file_operation_done(&operation);
}
static void host_file_owner_done(XrOsFileOwner *owner) {
 bool closed=false;CHECK(xr_os_io_file_close(owner,&closed)==XR_OS_IO_OK&&closed&&!owner->state);
 closed=false;CHECK(xr_os_io_file_close(owner,&closed)==XR_OS_IO_OK&&closed);
}
static void host_file_kernel_execute(void) {
 size_t live=runtime_live,bytes=runtime_bytes;bool cancelled=false;
 XrOsIoPolicy memory={&runtime_live,host_file_memory,host_file_memory_free};
 XrOsFileQuery query={&cancelled,host_file_current};
 wchar_t temporary[32768],directory[32768],wide[32768];char path[98304];
 DWORD length=GetTempPathW(32768,temporary);CHECK(length&&length<32768);
 CHECK(swprintf(directory,32768,L"%lsxray-file-%lu-%llu-\x4e2d",temporary,GetCurrentProcessId(),GetTickCount64())>0);
 CHECK(CreateDirectoryW(directory,NULL));CHECK(swprintf(wide,32768,L"%ls\\bytes-\x6587.bin",directory)>0);
 int encoded=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,path,(int)sizeof(path),NULL,NULL);CHECK(encoded>0);
 static const uint8_t expected[]={0x0d,0x0a,0x1a,0x00,0xff,0x42,0x00,0xe4,0xb8,0xad};
 uint8_t input[sizeof(expected)];memcpy(input,expected,sizeof(input));XrOsFileOwner owner={0};
 host_file_opened(&memory,&query,path,true,false,&owner);host_file_written(&memory,&query,&owner,input,sizeof(input));host_file_owner_done(&owner);
 host_file_opened(&memory,&query,path,true,false,&owner);host_file_written(&memory,&query,&owner,input,7);
 XrOsFileOperation operation={0};XrOsFileRequest flush={.action=XR_OS_FILE_FLUSH,.standard_stream=-1};
 CHECK(xr_os_io_file_begin(&memory,&query,&flush,&owner,&operation)==XR_OS_IO_OK);
 XrOsFileProgress result=host_file_terminal(&operation);CHECK(result.status==XR_OS_IO_OK&&result.success);host_file_operation_done(&operation);host_file_owner_done(&owner);
 host_file_opened(&memory,&query,path,true,true,&owner);host_file_written(&memory,&query,&owner,input+7,sizeof(input)-7);host_file_owner_done(&owner);
 CHECK(!memcmp(input,expected,sizeof(input)));
 HANDLE exclusive=CreateFileW(wide,GENERIC_READ,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(exclusive!=INVALID_HANDLE_VALUE);
 uint8_t actual[sizeof(expected)+1]={0};DWORD got=0;CHECK(ReadFile(exclusive,actual,sizeof(actual),&got,NULL)&&got==sizeof(expected)&&!memcmp(actual,expected,sizeof(expected)));CHECK(CloseHandle(exclusive));
 host_file_opened(&memory,&query,path,false,false,&owner);size_t used=0;
 do{
  XrOsFileRequest read={.action=XR_OS_FILE_READ,.standard_stream=-1,.length=4};
  CHECK(xr_os_io_file_begin(&memory,&query,&read,&owner,&operation)==XR_OS_IO_OK);result=host_file_terminal(&operation);
  CHECK(result.status==XR_OS_IO_OK&&result.success&&result.count<=4&&used+result.count<=sizeof(expected));
  if(result.count){CHECK(result.bytes);memcpy(actual+used,result.bytes,result.count);used+=result.count;}
  host_file_operation_done(&operation);
 }while(result.count);
 CHECK(used==sizeof(expected)&&!memcmp(actual,expected,sizeof(expected)));host_file_owner_done(&owner);
 XrOsFileRequest request={.action=XR_OS_FILE_OPEN_READ,.path=path,.path_length=strlen(path),.standard_stream=-1};
 runtime_fail_at=runtime_attempts;CHECK(xr_os_io_file_open_begin(&memory,&query,&request,&owner,&operation)==XR_OS_IO_OUT_OF_MEMORY);
 runtime_fail_at=SIZE_MAX;CHECK(!owner.state&&!operation.state);cancelled=true;
 CHECK(xr_os_io_file_open_begin(&memory,&query,&request,&owner,&operation)==XR_OS_IO_CANCELLED&&!owner.state&&!operation.state);cancelled=false;
 const char invalid[]={(char)0xff};request.path=invalid;request.path_length=sizeof(invalid);
 CHECK(xr_os_io_file_open_begin(&memory,&query,&request,&owner,&operation)==XR_OS_IO_BAD_ARGUMENT);
 host_file_operation_done(&operation);host_file_owner_done(&owner);
 CHECK(DeleteFileW(wide)&&RemoveDirectoryW(directory));CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#else
static void host_file_kernel_execute(void) {
 XrOsFileOwner owner={0};XrOsFileOperation operation={0};XrOsFileProgress progress={.count=123};
 bool closed=false;XrOsFileRequest request={.action=XR_OS_FILE_READ,.standard_stream=0,.length=1};
 CHECK(xr_os_io_file_open_begin(NULL,NULL,&request,&owner,&operation)==XR_OS_IO_UNSUPPORTED);
 CHECK(xr_os_io_file_begin(NULL,NULL,&request,&owner,&operation)==XR_OS_IO_UNSUPPORTED);
 CHECK(xr_os_io_file_poll(&operation,&progress)==XR_OS_IO_UNSUPPORTED&&progress.count==123);
 CHECK(xr_os_io_file_cancel(&operation)==XR_OS_IO_UNSUPPORTED);
 CHECK(xr_os_io_file_operation_close(&operation,&closed)==XR_OS_IO_UNSUPPORTED&&!closed);
 CHECK(xr_os_io_file_close(&owner,&closed)==XR_OS_IO_UNSUPPORTED&&!closed&&!owner.state&&!operation.state);
}
#endif
#endif // HOST_OS_FILE_KERNEL_CASES_H
