/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_stat_cases.h - Native metadata snapshots and exact owner failures
 */
#ifndef HOST_OS_STAT_CASES_H
#define HOST_OS_STAT_CASES_H
static XrOsIoStatus host_stat_allocate(void *context,size_t size,void **output){
 (void)context;void *value=xr_malloc(size);if(!value)return XR_OS_IO_OUT_OF_MEMORY;*output=value;return XR_OS_IO_OK;
}
static void host_stat_free(void *context,void *value){(void)context;xr_free(value);}
static void host_stat_fields(const XrXirValue *result,const XrOsFileMetadata *expected,XrXirDomain *domain){
 bool some=false;const XrXirValue *object=NULL;CHECK(xr_xir_nullable_view(result,&some,&object)&&some&&object);
 const XrXirTypeArena *arena=xr_xir_value_arena(object);CHECK(arena);
 CHECK(xir_file_stat_type(xr_xir_compile_type_arena_types(arena),object->type));
 XrXirValueAdmission admission={.domain=domain,.arena=arena,.work=100000,.scratch_bytes=65536};
 const int64_t integers[]={expected->size,expected->mode,expected->mtime,expected->atime,expected->ctime};
 for(uint32_t f=0;f<10;++f){
  XrXirValue value={0};XrXirValueStatus status=xr_xir_class_get(object,f,&admission,&value);
  if(status!=XR_XIR_VALUE_OK)fprintf(stderr,"stat field%u status%u\n",f,status);CHECK(status==XR_XIR_VALUE_OK);
  if(f<5)CHECK(value.type==XR_XIR_I64&&value.payload==integers[f]);
  else if(f<7){
   const XrOsOptionalI64 *identity=f==5?&expected->uid:&expected->gid;const XrXirValue *number=NULL;bool present=false;
   CHECK(xr_xir_nullable_view(&value,&present,&number)&&present==identity->present);
   if(present)CHECK(number&&number->type==XR_XIR_I64&&number->payload==identity->value);
  }else CHECK(value.type==XR_XIR_BOOL&&value.payload==(f==7?expected->is_file:f==8?expected->is_dir:expected->is_symlink));
  xr_xir_value_drop(&value);
 }
}
static XrXirValue host_stat_path(XrXirInstance *instance,uint32_t make,const char *path){
 XrXirValue text={0};CHECK(xr_xir_string_new(instance->domain,path,strlen(path),&text)==XR_XIR_VALUE_OK);
 XrXirValue object=host_invoke(instance,make,&text,1);xr_xir_value_drop(&text);return object;
}
static void host_stat_execute(XrXirProgram *program,const uint32_t ids[4]){
 size_t live=runtime_live,bytes=runtime_bytes;char path[4096];
#if defined(XR_OS_WINDOWS)
 wchar_t wide[1024];DWORD length=GetTempPathW(900,wide);CHECK(length>0&&length<900);
 CHECK(swprintf_s(wide+length,1024-length,L"xray-stat-%lu-\x6D4B\x8BD5.bin",GetCurrentProcessId())>0);
 HANDLE file=CreateFileW(wide,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(file!=INVALID_HANDLE_VALUE);
 DWORD written=0;CHECK(WriteFile(file,"abc",3,&written,NULL)&&written==3&&CloseHandle(file));
 CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,path,sizeof(path),NULL,NULL)>0);
#else
 CHECK(snprintf(path,sizeof(path),"host-stat-%ld-\xE6\xB5\x8B\xE8\xAF\x95.bin",(long)getpid())>0);
 FILE *file=fopen(path,"wb");CHECK(file&&fwrite("abc",1,3,file)==3&&!fclose(file));
#endif
 XrOsFileMetadataOwner owner={0};XrOsIoPolicy policy={&owner,host_stat_allocate,host_stat_free};XrOsFileMetadata reference={0};
 CHECK(xr_os_io_file_metadata(&policy,path,strlen(path),NULL,&owner,&reference)==XR_OS_IO_OK&&!owner.handle&&reference.present&&reference.size==3&&reference.is_file);
 for(unsigned pass=0;pass<3;++pass){
  XrOsFileMetadata expected=reference;
  if(pass){stat_vector_flags=pass==1?2u:4u;expected=(XrOsFileMetadata){true,17,23,101,202,303,{true,0},{true,42},false,pass==1,pass==2};}
  XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;stat_mode=pass?STAT_ZERO_IDS:STAT_NORMAL;
  XrXirValue object=host_stat_path(instance,ids[1],path);
  XrXirValue checked=host_invoke(instance,ids[2],&object,1);CHECK(checked.type==XR_XIR_BOOL&&checked.payload==!pass);xr_xir_value_drop(&checked);
  unsigned before=stat_calls;
  if(!pass){uint32_t native=UINT32_MAX;
   for(uint32_t n=0;n<program->entry_count;++n)if(program->declarations->functions[n].native_callable==XR_NATIVE_CALLABLE_IO_STAT)native=n;
   CHECK(native!=UINT32_MAX&&xr_xir_instance_start(instance,native,&object,1)==XR_XIR_CALL_BAD_ARGUMENT&&stat_calls==before);
  }
  XrXirValue result=host_invoke(instance,ids[3],&object,1);CHECK(stat_calls==before+1);host_stat_fields(&result,&expected,instance->domain);
  bool some=false;const XrXirValue *body=NULL;CHECK(xr_xir_nullable_view(&result,&some,&body)&&some);
  XrXirValueAdmission admission={.domain=instance->domain,.arena=xr_xir_value_arena(body),.work=100000,.scratch_bytes=65536};XrXirValue uid={0};
  CHECK(xr_xir_class_get(body,5,&admission,&uid)==XR_XIR_VALUE_OK);xr_xir_value_drop(&object);
  CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
  CHECK(xr_xir_nullable_view(&result,&some,&body)&&some&&body);
  const XrXirValue *number=NULL;CHECK(xr_xir_nullable_view(&uid,&some,&number));if(pass)CHECK(some&&number&&number->payload==0);
  XrXirValue size={0};CHECK(xr_xir_class_get(body,0,&admission,&size)==XR_XIR_VALUE_OK&&size.payload==expected.size);xr_xir_value_drop(&size);
  xr_xir_value_drop(&uid);xr_xir_value_drop(&result);CHECK(runtime_live==live&&runtime_bytes==bytes);
 }
 char missing[sizeof(path)+16];CHECK(snprintf(missing,sizeof(missing),"%s.missing",path)>0);
 XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;stat_mode=STAT_NORMAL;
 XrXirValue object=host_stat_path(instance,ids[1],missing),result=host_invoke(instance,ids[3],&object,1);
 bool some=true;const XrXirValue *body=NULL;CHECK(xr_xir_nullable_view(&result,&some,&body)&&!some&&!body);
 xr_xir_value_drop(&result);xr_xir_value_drop(&object);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
 for(unsigned mode=STAT_OOM;mode<=STAT_BAD_UTF;++mode){
  instance=host_instance(program,ids[0]);host_active=instance;stat_mode=STAT_NORMAL;object=host_stat_path(instance,ids[1],path);stat_mode=mode;
  CHECK(xr_xir_instance_start(instance,ids[3],&object,1)==XR_XIR_CALL_READY);XrXirCallStatus status=host_poll(instance);
  XrXirCallStatus expected=mode==STAT_OOM?XR_XIR_CALL_OOM:mode==STAT_BAD_UTF?XR_XIR_CALL_HOST_ERROR:XR_XIR_CALL_CANCELLED;
  fprintf(stderr,"stat mode%u status%u expected%u\n",mode,status,expected);CHECK(status==expected);
  result=(XrXirValue){0};CHECK(xr_xir_instance_take_result(instance,&result)==(status==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_BAD_STATE:status)&&!result.type&&!result.payload);
  xr_xir_value_drop(&object);CHECK(xr_xir_instance_free(instance)==(status==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_READY:status));host_active=NULL;
  CHECK(runtime_live==live&&runtime_bytes==bytes);
 }
#if defined(XR_OS_WINDOWS)
 for(unsigned failures=0;failures<2;++failures){
  instance=host_instance(program,ids[0]);host_active=instance;stat_mode=STAT_NORMAL;object=host_stat_path(instance,ids[1],path);stat_mode=STAT_RETAIN_CLOSE;
  unsigned before=stat_calls,closes=stat_close_calls;CHECK(xr_xir_instance_start(instance,ids[3],&object,1)==XR_XIR_CALL_READY);
  CHECK(host_poll(instance)==XR_XIR_CALL_HOST_ERROR&&stat_calls==before+1&&stat_close_calls==closes&&instance->file_metadata_owners);
  result=(XrXirValue){0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_HOST_ERROR&&!result.type&&!result.payload);
  xr_xir_value_drop(&object);stat_close_reentry=true;stat_close_failures=failures;
  if(failures)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_CLOSE_FAILED&&instance->file_metadata_owners);
  CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_HOST_ERROR&&stat_calls==before+1&&stat_close_calls==closes+1+failures);host_active=NULL;
  CHECK(runtime_live==live&&runtime_bytes==bytes);
 }
 CHECK(DeleteFileW(wide));
#else
 CHECK(!remove(path));
#endif
 stat_mode=STAT_NORMAL;CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#endif // HOST_OS_STAT_CASES_H
