/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_bool_cases.h - Real leaf mutations and failed preparation isolation
 */
#ifndef HOST_OS_IO_BOOL_CASES_H
#define HOST_OS_IO_BOOL_CASES_H
#include <time.h>
#if !defined(XR_OS_WINDOWS)
#include <utime.h>
#endif
#if defined(XR_OS_WINDOWS)
#include <winioctl.h>
static int host_io_junction(const wchar_t *link, const wchar_t *target) {
    struct JunctionData {
        DWORD tag;
        WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        WCHAR path[16384];
    } data = {0};
    size_t target_length = wcslen(target);
    CHECK(target_length + 4 + 1 + target_length + 1 <= 16384);
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    memcpy(data.path, L"\\??\\", 4 * sizeof(WCHAR));
    memcpy(data.path + 4, target, (target_length + 1) * sizeof(WCHAR));
    data.substitute_length = (WORD)((target_length + 4) * sizeof(WCHAR));
    data.print_offset = data.substitute_length + (WORD)sizeof(WCHAR);
    data.print_length = (WORD)(target_length * sizeof(WCHAR));
    memcpy(data.path + target_length + 5, target, (target_length + 1) * sizeof(WCHAR));
    data.length = (WORD)(8 + (target_length * 2 + 6) * sizeof(WCHAR));
    HANDLE handle = CreateFileW(link, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    bool assigned = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        (DWORD)data.length + 8, NULL, 0, &returned, NULL) != 0;
    bool closed = CloseHandle(handle) != 0;
    CHECK(assigned && closed);
    return 1;
}
#endif
static XrXirValue host_io_object(XrXirInstance *instance,uint32_t make,const char *bytes,size_t length){
 XrXirValue text={0};CHECK(xr_xir_string_new(instance->domain,bytes,length,&text)==XR_XIR_VALUE_OK);
 XrXirValue value=host_invoke(instance,make,&text,1);xr_xir_value_drop(&text);return value;
}
static void host_io_bool(XrXirInstance *instance,uint32_t function,const XrXirValue *arguments,uint32_t count,bool expected){
 XrXirValue result=host_invoke(instance,function,arguments,count);CHECK(result.type==XR_XIR_BOOL&&result.payload==expected);xr_xir_value_drop(&result);
}
static bool host_io_exists(const char *path){
 XrOsFileMetadataOwner owner={0};XrOsFileMetadata value={0};XrOsIoPolicy policy={&owner,host_stat_allocate,host_stat_free};
 CHECK(xr_os_io_file_metadata(&policy,path,strlen(path),NULL,&owner,&value)==XR_OS_IO_OK&&!owner.handle);return value.present;
}
static void host_io_write(const char *path){
#if defined(XR_OS_WINDOWS)
 wchar_t wide[4096];CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,4096)>0);
 HANDLE file=CreateFileW(wide,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(file!=INVALID_HANDLE_VALUE);
 DWORD written=0;CHECK(WriteFile(file,"keep",4,&written,NULL)&&written==4&&CloseHandle(file));
#else
 FILE *file=fopen(path,"wb");CHECK(file&&fwrite("keep",1,4,file)==4&&!fclose(file));
#endif
}
static void host_io_old_times(const char *path){
#if defined(XR_OS_WINDOWS)
 wchar_t wide[4096];CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,4096)>0);
 HANDLE handle=CreateFileW(wide,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);CHECK(handle!=INVALID_HANDLE_VALUE);
 uint64_t ticks=UINT64_C(116444736000000000)+UINT64_C(946684800)*UINT64_C(10000000);FILETIME old={(DWORD)ticks,(DWORD)(ticks>>32)};
 CHECK(SetFileTime(handle,NULL,&old,&old)&&CloseHandle(handle));
#else
 struct utimbuf old={946684800,946684800};CHECK(!utime(path,&old));
#endif
}
static void host_io_failure(XrXirInstance *instance,uint32_t function,const XrXirValue *args,uint32_t count,XrXirCallStatus expected){
 CHECK(xr_xir_instance_start(instance,function,args,count)==XR_XIR_CALL_READY);CHECK(host_poll(instance)==expected);XrXirValue result={0};
 CHECK(xr_xir_instance_take_result(instance,&result)==(expected==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_BAD_STATE:expected)&&!result.type&&!result.payload&&!result.reserved);
}
static void host_io_limit(XrXirProgram *program,const uint32_t ids[7],const char *path){
 uint64_t limit=0;
 for(unsigned pass=0;pass<3;++pass){
  XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);if(pass==1)config.work_limit=limit;
  XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);host_active=instance;io_bool_mode=IO_BOOL_NORMAL;
  XrXirValue init=host_invoke(instance,ids[0],NULL,0);CHECK(init.type==XR_XIR_I64&&!init.payload);xr_xir_value_drop(&init);
  XrXirValue object=host_io_object(instance,ids[1],path,strlen(path));unsigned before=io_bool_calls;
  CHECK(xr_xir_instance_start(instance,ids[6],&object,1)==XR_XIR_CALL_READY);XrXirCallStatus status=host_poll(instance);XrXirValue result={0};
  fprintf(stderr,"IO finite pass%u poll%u provider%u current%u failed%u\n",pass,status,io_bool_calls-before,io_bool_currents,io_bool_failed_at);
  CHECK(io_bool_calls==before+1);
  if(pass==1){CHECK(status==XR_XIR_CALL_LIMIT&&io_bool_currents==2&&io_bool_failed_at==2&&!instance->file_metadata_owners);CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_LIMIT&&!result.type&&!result.payload);}
  else{CHECK(status==XR_XIR_CALL_RETURNED&&xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED&&result.type==XR_XIR_BOOL&&result.payload);limit=io_bool_entry_work+1;}
  xr_xir_value_drop(&result);xr_xir_value_drop(&object);CHECK(xr_xir_instance_free(instance)==(pass==1?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));host_active=NULL;
 }
}
static void host_io_bool_execute(XrXirProgram *program,const uint32_t ids[7]){
 size_t live=runtime_live,bytes=runtime_bytes;char root[4096],dir[4096],old[4096],next[4096];
#if defined(XR_OS_WINDOWS)
 wchar_t wide_root[1024];DWORD length=GetTempPathW(900,wide_root);CHECK(length>0&&length<900);
 CHECK(swprintf_s(wide_root+length,1024-length,L"xray-io-%lu-\x6D4B\x8BD5",GetCurrentProcessId())>0&&CreateDirectoryW(wide_root,NULL));
 CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide_root,-1,root,sizeof(root),NULL,NULL)>0);
#else
 CHECK(snprintf(root,sizeof(root),"host-io-%ld-\xE6\xB5\x8B\xE8\xAF\x95",(long)getpid())>0&&mkdir(root,0700)==0);
#endif
 CHECK(snprintf(dir,sizeof(dir),"%s/dir",root)>0&&snprintf(old,sizeof(old),"%s/old",dir)>0&&snprintf(next,sizeof(next),"%s/new-long-name",dir)>0);
 XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;io_bool_mode=IO_BOOL_NORMAL;
 XrXirValue directory=host_io_object(instance,ids[1],dir,strlen(dir));host_io_bool(instance,ids[2],&directory,1,true);host_io_bool(instance,ids[2],&directory,1,true);
 host_io_write(old);XrXirValue args[2]={host_io_object(instance,ids[1],old,strlen(old)),host_io_object(instance,ids[1],next,strlen(next))};
 host_io_bool(instance,ids[2],args,1,false);host_io_bool(instance,ids[3],&directory,1,false);
 host_io_old_times(old);host_io_old_times(dir);
 int64_t before=(int64_t)time(NULL);host_io_bool(instance,ids[6],args,1,true);host_io_bool(instance,ids[6],&directory,1,true);int64_t after=(int64_t)time(NULL);
 XrOsFileMetadataOwner owner={0};XrOsFileMetadata metadata={0};XrOsIoPolicy policy={&owner,host_stat_allocate,host_stat_free};
 CHECK(xr_os_io_file_metadata(&policy,old,strlen(old),NULL,&owner,&metadata)==XR_OS_IO_OK&&!owner.handle&&metadata.size==4&&metadata.mtime>=before-1&&metadata.mtime<=after+1&&metadata.atime>=before-1&&metadata.atime<=after+1);
 CHECK(xr_os_io_file_metadata(&policy,dir,strlen(dir),NULL,&owner,&metadata)==XR_OS_IO_OK&&!owner.handle&&metadata.is_dir&&metadata.mtime>=before-1&&metadata.mtime<=after+1&&metadata.atime>=before-1&&metadata.atime<=after+1);
 host_io_bool(instance,ids[5],args,2,true);CHECK(!host_io_exists(old)&&host_io_exists(next));unsigned allocation_count=io_bool_allocations;CHECK(allocation_count>=2&&!(allocation_count%2));
 XrXirValue reverse[2]={args[1],args[0]};host_io_bool(instance,ids[5],reverse,2,true);CHECK(host_io_exists(old)&&!host_io_exists(next));
 XrXirValue wrong={XR_XIR_I64,0,42},bad[2]={args[0],wrong};unsigned calls=io_bool_calls;
 CHECK(xr_xir_instance_start(instance,ids[5],bad,2)==XR_XIR_CALL_BAD_ARGUMENT&&io_bool_calls==calls);bad[0]=wrong;bad[1]=args[1];
 CHECK(xr_xir_instance_start(instance,ids[5],bad,2)==XR_XIR_CALL_BAD_ARGUMENT&&io_bool_calls==calls);
 XrXirValue invalid={0};CHECK(xr_xir_string_new(instance->domain,"\xFF",1,&invalid)==XR_XIR_VALUE_BAD_UTF8&&!invalid.type&&!invalid.payload&&io_bool_calls==calls);
 xr_xir_value_drop(&args[0]);xr_xir_value_drop(&args[1]);xr_xir_value_drop(&directory);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
 for(unsigned position=0;position<2;++position){
  instance=host_instance(program,ids[0]);host_active=instance;io_bool_mode=IO_BOOL_NORMAL;
  const char nul[]={'.',0,'x'};args[0]=host_io_object(instance,ids[1],old,strlen(old));args[1]=host_io_object(instance,ids[1],next,strlen(next));
  xr_xir_value_drop(args+position);args[position]=host_io_object(instance,ids[1],nul,sizeof(nul));calls=io_bool_calls;
  host_io_failure(instance,ids[5],args,2,XR_XIR_CALL_HOST_ERROR);CHECK(io_bool_calls==calls&&host_io_exists(old)&&!host_io_exists(next));
  xr_xir_value_drop(args);xr_xir_value_drop(args+1);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_HOST_ERROR);host_active=NULL;
 }
 for(unsigned mode=IO_BOOL_OOM;mode<=IO_BOOL_CANCEL_AFTER;++mode){
  instance=host_instance(program,ids[0]);host_active=instance;io_bool_mode=IO_BOOL_NORMAL;args[0]=host_io_object(instance,ids[1],old,strlen(old));args[1]=host_io_object(instance,ids[1],next,strlen(next));
  io_bool_mode=mode;io_bool_fault_at=allocation_count/2;calls=io_bool_calls;
  host_io_failure(instance,ids[5],args,2,mode==IO_BOOL_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_CANCELLED);CHECK(io_bool_calls==calls+1);
  CHECK(host_io_exists(old)==(mode!=IO_BOOL_CANCEL_AFTER)&&host_io_exists(next)==(mode==IO_BOOL_CANCEL_AFTER));
  xr_xir_value_drop(args);xr_xir_value_drop(args+1);CHECK(xr_xir_instance_free(instance)==(mode==IO_BOOL_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));host_active=NULL;
 }
 io_bool_mode=IO_BOOL_NORMAL;instance=host_instance(program,ids[0]);host_active=instance;
 args[0]=host_io_object(instance,ids[1],next,strlen(next));args[1]=host_io_object(instance,ids[1],old,strlen(old));host_io_bool(instance,ids[5],args,2,true);
#if defined(XR_OS_WINDOWS)
 wchar_t readonly[4096];CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,old,-1,readonly,4096)>0&&SetFileAttributesW(readonly,FILE_ATTRIBUTE_READONLY));
#endif
 host_io_bool(instance,ids[4],args+1,1,true);CHECK(!host_io_exists(old));host_io_bool(instance,ids[4],args+1,1,false);
 directory=host_io_object(instance,ids[1],dir,strlen(dir));host_io_bool(instance,ids[3],&directory,1,true);
 xr_xir_value_drop(args);xr_xir_value_drop(args+1);xr_xir_value_drop(&directory);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
 host_io_limit(program,ids,root);
#if defined(XR_OS_WINDOWS)
 wchar_t target[2048],link[2048],keep[2048];char utf8_link[8192];
 CHECK(swprintf_s(target,2048,L"%ls-target",wide_root)>0&&CreateDirectoryW(target,NULL));
 CHECK(swprintf_s(keep,2048,L"%ls/keep.bin",target)>0);HANDLE protected_file=CreateFileW(keep,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(protected_file!=INVALID_HANDLE_VALUE);
 DWORD written=0;CHECK(WriteFile(protected_file,"safe",4,&written,NULL)&&written==4&&CloseHandle(protected_file));
 CHECK(swprintf_s(link,2048,L"%ls/link",wide_root)>0&&CreateDirectoryW(link,NULL)&&host_io_junction(link,target));
 CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,link,-1,utf8_link,sizeof(utf8_link),NULL,NULL)>0);
 instance=host_instance(program,ids[0]);host_active=instance;XrXirValue junction=host_io_object(instance,ids[1],utf8_link,strlen(utf8_link));
 host_io_bool(instance,ids[4],&junction,1,true);CHECK(GetFileAttributesW(link)==INVALID_FILE_ATTRIBUTES&&GetFileAttributesW(keep)!=INVALID_FILE_ATTRIBUTES);
 protected_file=CreateFileW(keep,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(protected_file!=INVALID_HANDLE_VALUE);
 char protected_bytes[4]={0};DWORD read=0;CHECK(ReadFile(protected_file,protected_bytes,4,&read,NULL)&&read==4&&!memcmp(protected_bytes,"safe",4)&&CloseHandle(protected_file));
 xr_xir_value_drop(&junction);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
 CHECK(DeleteFileW(keep)&&RemoveDirectoryW(target));
 instance=host_instance(program,ids[0]);host_active=instance;XrXirValue retained=host_io_object(instance,ids[1],root,strlen(root));
 io_bool_mode=IO_BOOL_RETAIN_CLOSE;unsigned calls_before=io_bool_calls,closes=stat_close_calls;
 host_io_failure(instance,ids[6],&retained,1,XR_XIR_CALL_HOST_ERROR);CHECK(instance->file_metadata_owners&&io_bool_calls==calls_before+1&&stat_close_calls==closes);
 xr_xir_value_drop(&retained);stat_close_failures=1;stat_close_reentry=true;
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_CLOSE_FAILED&&instance->file_metadata_owners);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_HOST_ERROR&&io_bool_calls==calls_before+1&&stat_close_calls==closes+2);host_active=NULL;
 CHECK(RemoveDirectoryW(wide_root));
#else
 CHECK(!rmdir(root));
#endif
 io_bool_mode=IO_BOOL_NORMAL;CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#endif // HOST_OS_IO_BOOL_CASES_H
