/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_path_cases.h - Path identity, OS effects and retained arena boundaries
 */
#ifndef HOST_OS_PATH_CASES_H
#define HOST_OS_PATH_CASES_H
#if defined(XR_OS_WINDOWS)
static void host_path_execute(XrXirProgram *program,const uint32_t ids[5]){
 size_t baseline=runtime_live,bytes=runtime_bytes;wchar_t old[32768],folder[32768],file[32768];char utf8[131072];
 CHECK(GetCurrentDirectoryW(32768,old)>0);DWORD n=GetTempPathW(32768,folder);CHECK(n>0&&n<32000);
 CHECK(swprintf_s(folder+n,32768-n,L"xray-path-\x6D4B\x8BD5-%lu",GetCurrentProcessId())>0);CHECK(CreateDirectoryW(folder,NULL));
 CHECK(swprintf_s(file,32768,L"%ls/\x6587\x4EF6.txt",folder)>0);
 HANDLE handle=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(handle!=INVALID_HANDLE_VALUE&&CloseHandle(handle));
 CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,file,-1,utf8,sizeof(utf8),NULL,NULL)>0);
 XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;host_mode=HOST_NORMAL;
 XrXirValue text={0};CHECK(xr_xir_string_new(instance->domain,utf8,strlen(utf8),&text)==XR_XIR_VALUE_OK);
 XrXirValue object=host_invoke(instance,ids[1],&text,1);xr_xir_value_drop(&text);
 const XrXirTypeArena *owner=xr_xir_value_arena(&object);CHECK(owner);
 XrXirValueAdmission access={.arena=owner,.work=100000,.scratch_bytes=65536};XrXirValue marker={0};
 CHECK(xr_xir_class_get(&object,0,&access,&marker)==XR_XIR_VALUE_OK&&marker.type==XR_XIR_I64&&marker.payload==42);xr_xir_value_drop(&marker);
 XrXirValue args[2]={object,{XR_XIR_I64,0,256}};unsigned before=host_calls;
 XrXirValue result=host_invoke(instance,ids[2],args,2);CHECK(result.type==XR_XIR_BOOL&&result.payload==1&&host_calls==before+1);xr_xir_value_drop(&result);
 CHECK(GetFileAttributesW(file)&FILE_ATTRIBUTE_READONLY);
 args[1].payload=384;result=host_invoke(instance,ids[2],args,2);CHECK(result.type==XR_XIR_BOOL&&result.payload==1);xr_xir_value_drop(&result);
 CHECK(!(GetFileAttributesW(file)&FILE_ATTRIBUTE_READONLY));
 const int64_t invalid[]={-1,INT64_MAX};for(unsigned i=0;i<2;++i){args[1].payload=invalid[i];before=host_calls;result=host_invoke(instance,ids[2],args,2);CHECK(result.type==XR_XIR_BOOL&&!result.payload&&host_calls==before);xr_xir_value_drop(&result);}
 xr_xir_value_drop(&object);
 wchar_t missing[32768];CHECK(swprintf_s(missing,32768,L"%ls/missing",folder)>0);
 CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,missing,-1,utf8,sizeof(utf8),NULL,NULL)>0);
 CHECK(xr_xir_string_new(instance->domain,utf8,strlen(utf8),&text)==XR_XIR_VALUE_OK);object=host_invoke(instance,ids[1],&text,1);xr_xir_value_drop(&text);
 args[0]=object;args[1].payload=384;before=host_calls;result=host_invoke(instance,ids[2],args,2);CHECK(result.type==XR_XIR_BOOL&&!result.payload&&host_calls==before+1);xr_xir_value_drop(&result);
 before=host_calls;result=host_invoke(instance,ids[3],&object,1);CHECK(result.type==XR_XIR_BOOL&&!result.payload&&host_calls==before+1);xr_xir_value_drop(&result);xr_xir_value_drop(&object);
 size_t length=strlen(utf8);CHECK(length+9<sizeof(utf8));memcpy(utf8+length+1,"ignored",8);
 CHECK(xr_xir_string_new(instance->domain,utf8,length+9,&text)==XR_XIR_VALUE_OK);object=host_invoke(instance,ids[1],&text,1);xr_xir_value_drop(&text);before=host_calls;
 args[0]=object;result=host_invoke(instance,ids[2],args,2);CHECK(result.type==XR_XIR_BOOL&&!result.payload);xr_xir_value_drop(&result);
 result=host_invoke(instance,ids[3],&object,1);CHECK(result.type==XR_XIR_BOOL&&!result.payload&&host_calls==before);xr_xir_value_drop(&result);xr_xir_value_drop(&object);
 CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,folder,-1,utf8,sizeof(utf8),NULL,NULL)>0);
 CHECK(xr_xir_string_new(instance->domain,utf8,strlen(utf8),&text)==XR_XIR_VALUE_OK);object=host_invoke(instance,ids[1],&text,1);xr_xir_value_drop(&text);
 result=host_invoke(instance,ids[3],&object,1);CHECK(result.type==XR_XIR_BOOL&&result.payload==1);xr_xir_value_drop(&result);
 wchar_t actual[32768];CHECK(GetCurrentDirectoryW(32768,actual)>0&&!wcscmp(actual,folder));CHECK(SetCurrentDirectoryW(old));xr_xir_value_drop(&object);
 CHECK(xr_xir_string_new(instance->domain,utf8,strlen(utf8),&text)==XR_XIR_VALUE_OK);
 result=host_invoke(instance,ids[4],&text,1);CHECK(result.type==XR_XIR_BOOL&&result.payload==1);xr_xir_value_drop(&result);xr_xir_value_drop(&text);CHECK(SetCurrentDirectoryW(old));
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
 for(unsigned mode=HOST_OOM_WIDE;mode<=HOST_BAD_UTF;++mode){
  instance=host_instance(program,ids[0]);host_active=instance;host_mode=HOST_NORMAL;
  CHECK(xr_xir_string_new(instance->domain,utf8,strlen(utf8),&text)==XR_XIR_VALUE_OK);object=host_invoke(instance,ids[1],&text,1);xr_xir_value_drop(&text);
  host_mode=mode;CHECK(xr_xir_instance_start(instance,ids[3],&object,1)==XR_XIR_CALL_READY);
  XrXirCallStatus status=host_poll(instance),expected=mode==HOST_OOM_WIDE?XR_XIR_CALL_OOM:mode==HOST_BAD_UTF?XR_XIR_CALL_HOST_ERROR:XR_XIR_CALL_CANCELLED;
  fprintf(stderr,"mutation mode%u status%u expected%u\n",mode,status,expected);CHECK(status==expected);
  CHECK(GetCurrentDirectoryW(32768,actual)>0&&!wcscmp(actual,mode==HOST_CANCEL_AFTER?folder:old));CHECK(SetCurrentDirectoryW(old));
  xr_xir_value_drop(&object);XrXirCallStatus freed=xr_xir_instance_free(instance);CHECK(freed==(mode==HOST_OOM_WIDE?XR_XIR_CALL_OOM:mode==HOST_BAD_UTF?XR_XIR_CALL_HOST_ERROR:XR_XIR_CALL_READY));host_active=NULL;
 }
 host_mode=HOST_NORMAL;CHECK(DeleteFileW(file)&&RemoveDirectoryW(folder));CHECK(runtime_live==baseline&&runtime_bytes==bytes);
 fprintf(stderr,"Path raw field1 public subclass real Unicode chmod/chdir; status/cleanup controls complete\n");
}
#else
static XrOsIoStatus host_cwd_allocate(void *context,size_t bytes,void **output){
 CHECK(context&&output&&!*output);*output=xr_malloc(bytes);return *output?XR_OS_IO_OK:XR_OS_IO_OUT_OF_MEMORY;
}
static void host_cwd_free(void *context,void *memory){CHECK(context);xr_free(memory);}
static void host_path_execute(XrXirProgram *program,const uint32_t ids[5]){
 size_t live=runtime_live,bytes=runtime_bytes;int context=0;XrOsIoPolicy policy={&context,host_cwd_allocate,host_cwd_free};char *old=NULL;
 CHECK(xr_os_io_getcwd_owned(&policy,&old)==XR_OS_IO_OK&&old);
 char folder[]="./xray-host-\xE6\xB5\x8B-XXXXXX";CHECK(mkdtemp(folder));char file[128];CHECK(snprintf(file,sizeof(file),"%s/file",folder)>0);FILE *stream=fopen(file,"wb");CHECK(stream&&!fclose(stream));
 XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;host_mode=HOST_NORMAL;XrXirValue text={0};
 CHECK(xr_xir_string_new(instance->domain,file,strlen(file),&text)==XR_XIR_VALUE_OK);XrXirValue object=host_invoke(instance,ids[1],&text,1);xr_xir_value_drop(&text);
 XrXirValue args[]={object,{XR_XIR_I64,0,256}},result=host_invoke(instance,ids[2],args,2);CHECK(result.type==XR_XIR_BOOL&&result.payload==1);xr_xir_value_drop(&result);
 struct stat info;CHECK(!stat(file,&info)&&(info.st_mode&S_IRUSR)&&!(info.st_mode&S_IWUSR));args[1].payload=384;result=host_invoke(instance,ids[2],args,2);CHECK(result.type==XR_XIR_BOOL&&result.payload==1);xr_xir_value_drop(&result);xr_xir_value_drop(&object);
 CHECK(xr_xir_string_new(instance->domain,folder,strlen(folder),&text)==XR_XIR_VALUE_OK);object=host_invoke(instance,ids[1],&text,1);xr_xir_value_drop(&text);result=host_invoke(instance,ids[3],&object,1);CHECK(result.type==XR_XIR_BOOL&&result.payload==1);xr_xir_value_drop(&result);xr_xir_value_drop(&object);
 char *now=NULL;CHECK(xr_os_io_getcwd_owned(&policy,&now)==XR_OS_IO_OK&&strcmp(now,old));host_cwd_free(&context,now);
 bool restored=false;CHECK(xr_os_io_chdir(&policy,old,NULL,&restored)==XR_OS_IO_OK&&restored);
 char missing[128];CHECK(snprintf(missing,sizeof(missing),"%s/missing",folder)>0);
 CHECK(xr_xir_string_new(instance->domain,missing,strlen(missing),&text)==XR_XIR_VALUE_OK);object=host_invoke(instance,ids[1],&text,1);xr_xir_value_drop(&text);result=host_invoke(instance,ids[3],&object,1);CHECK(result.type==XR_XIR_BOOL&&!result.payload);xr_xir_value_drop(&result);xr_xir_value_drop(&object);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;CHECK(!unlink(file)&&!rmdir(folder));host_cwd_free(&context,old);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#endif
static XrXirProgram *host_replica(const XrXirCompileContext *,const XrXirProgram *);
static void host_path_receiving(XrXirCompileContext *context,XrXirProgram **owned,const uint32_t ids[5]){
 XrXirProgram *producer=*owned;XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
 XrXirInstance *root_instance=NULL;CHECK(xr_xir_instance_generation_new(&config,&root_instance)==XR_XIR_CALL_READY);
 uint64_t generation=0;CHECK(xr_xir_instance_generation_prepare(root_instance,producer,&generation)==XR_XIR_CALL_READY);
 host_active=instance_current(root_instance);host_mode=HOST_NORMAL;
 XrXirValue text={0};CHECK(xr_xir_string_new(root_instance->domain,".",1,&text)==XR_XIR_VALUE_OK);XrXirValue object=host_invoke(root_instance,ids[1],&text,1);xr_xir_value_drop(&text);
 CHECK(xr_xir_value_arena(&object)==producer->arena&&object_pointer(&object)->domain==root_instance->domain);
 XrXirProgram *negative=host_replica(context,producer);CHECK(negative->arena!=producer->arena);
 /* This private adapter receives exact parameter coordinates from two full
  * sealed receptions of one immutable Input, before target first use. */
 XrXirProgram *target=host_replica(context,producer);
 CHECK(target->permissions&&producer->permissions&&target->permissions->input&&producer->permissions->input);
 CHECK(!memcmp(target->permissions->packet_identity,producer->permissions->packet_identity,32));
 CHECK(!memcmp(target->permissions->input->artifact->checked_identity,producer->permissions->input->artifact->checked_identity,32));
 CHECK(xir_program_input_verify(context,target->permissions->input)==XR_XIR_OK&&xir_program_input_verify(context,producer->permissions->input)==XR_XIR_OK);
 CHECK(target->entry_count==producer->entry_count&&ids[3]<target->entry_count&&target->entries[ids[3]].parameter_count==1&&producer->entries[ids[3]].parameter_count==1);
 CHECK(atomic_load_explicit(&target->references,memory_order_acquire)==1&&atomic_load_explicit(&target->arena->references,memory_order_acquire)==1&&!target->arena->received_from&&!target->arena->identities);
 XirTypeReceivingRoot root={producer->entries[ids[3]].parameters[0],target->entries[ids[3]].parameters[0]};
 CHECK(xir_compile_type_arena_receive(context,target->arena,producer->arena,&root,1)==XR_XIR_VALUE_OK);
 CHECK(xr_xir_instance_generation_abort(root_instance,generation)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(producer);*owned=NULL;host_active=NULL;
 generation=0;CHECK(xr_xir_instance_generation_prepare(root_instance,negative,&generation)==XR_XIR_CALL_READY);host_active=instance_current(root_instance);unsigned before=host_calls;
 CHECK(!xr_xir_value_argument(&object,negative->arena,negative->entries[ids[3]].parameters[0]));
 CHECK(xr_xir_instance_start(root_instance,ids[3],&object,1)==XR_XIR_CALL_BAD_ARGUMENT&&host_calls==before);
 CHECK(xr_xir_instance_generation_abort(root_instance,generation)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(negative);
 CHECK(xr_xir_value_valid(&object)&&xr_xir_value_arena(&object)!=target->arena&&xr_xir_value_argument(&object,target->arena,target->entries[ids[3]].parameters[0]));
 generation=0;CHECK(xr_xir_instance_generation_prepare(root_instance,target,&generation)==XR_XIR_CALL_READY);host_active=instance_current(root_instance);XrXirValue value=host_invoke(root_instance,ids[3],&object,1);CHECK(value.type==XR_XIR_BOOL&&value.payload==1);xr_xir_value_drop(&value);xr_xir_value_drop(&object);
 CHECK(xr_xir_instance_generation_abort(root_instance,generation)==XR_XIR_CALL_READY);host_active=NULL;xr_xir_compile_program_drop(target);CHECK(xr_xir_instance_free(root_instance)==XR_XIR_CALL_READY);
 fprintf(stderr,"unreceived third Program refused; fresh exact-Input Path receiving; producer candidate Instance/Program retired before same-Domain consumer CALL71\n");
}
#endif // HOST_OS_PATH_CASES_H
