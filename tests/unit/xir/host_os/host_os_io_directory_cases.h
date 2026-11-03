/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_directory_cases.h - Opaque cursors, retained names and retirement
 *
 * KEY CONCEPT:
 *   The same Source and generated C enumerate actual directory entries. Adapter
 *   faults are explicit; every successful name comes from the actual OS.
 */
#ifndef HOST_OS_IO_DIRECTORY_CASES_H
#define HOST_OS_IO_DIRECTORY_CASES_H
#if defined(XR_OS_WINDOWS)
static XrXirProgram *host_directory_replica(const XrXirCompileContext *,const XrXirProgram *);
typedef struct HostDirectoryFixture {
 wchar_t root[4096],long_name[128];char path[16384],long_text[1024];
} HostDirectoryFixture;
static void host_directory_fixture(HostDirectoryFixture *fixture,bool create){
 wchar_t leaf[4096];
 if(create){
  wchar_t base[4096];DWORD length=GetTempPathW(3000,base);CHECK(length&&length<3000);
  CHECK(swprintf_s(base+length,4096-length,L"xray-directory-product-%lu-\x6d4b\x8bd5",GetCurrentProcessId())>0);
  CHECK((base[0]==L'\\'&&base[1]==L'\\'?swprintf_s(fixture->root,4096,L"\\\\?\\UNC\\%ls",base+2):
   swprintf_s(fixture->root,4096,L"\\\\?\\%ls",base))>0&&CreateDirectoryW(fixture->root,NULL));
  for(unsigned n=0;n<120;++n)fixture->long_name[n]=L'\x4e2d';fixture->long_name[120]=0;
  for(unsigned n=0;n<2;++n){CHECK(swprintf_s(leaf,4096,L"%ls\\%ls",fixture->root,fixture->long_name)>0&&CreateDirectoryW(leaf,NULL));wcscpy_s(fixture->root,4096,leaf);}
  CHECK(wcslen(fixture->root)>260&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,fixture->root,-1,fixture->path,sizeof(fixture->path),NULL,NULL)>0);
  CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,fixture->long_name,-1,fixture->long_text,sizeof(fixture->long_text),NULL,NULL)>0&&strlen(fixture->long_text)>260);
 }
 for(unsigned n=0;n<81;++n){
  CHECK((n==80?swprintf_s(leaf,4096,L"%ls\\%ls",fixture->root,fixture->long_name):swprintf_s(leaf,4096,L"%ls\\entry-%02u",fixture->root,n))>0);
  if(create){HANDLE file=CreateFileW(leaf,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(file!=INVALID_HANDLE_VALUE&&CloseHandle(file));}
  else CHECK(DeleteFileW(leaf));
 }
 CHECK(swprintf_s(leaf,4096,L"%ls\\nested",fixture->root)>0);
 if(create)CHECK(CreateDirectoryW(leaf,NULL));
 else{
  CHECK(RemoveDirectoryW(leaf));wcscpy_s(leaf,4096,fixture->root);
  for(unsigned n=0;n<3;++n){CHECK(RemoveDirectoryW(leaf));wchar_t *last=wcsrchr(leaf,L'\\');CHECK(last);*last=0;}
 }
}
static XrXirValue host_directory_open_path(XrXirInstance *instance,const uint32_t ids[6],const char *path){
 XrXirValue object=host_io_object(instance,ids[1],path,strlen(path));
 XrXirValue token=host_invoke(instance,ids[2],&object,1);xr_xir_value_drop(&object);
 CHECK(token.type==XR_XIR_I64&&token.payload>0&&token.payload<=INT64_MAX);return token;
}
static bool host_directory_name(const XrXirValue *value,const char **bytes,size_t *length){
 bool present=false;const XrXirValue *text=NULL;CHECK(xr_xir_nullable_view(value,&present,&text));
 if(present)CHECK(text&&xr_xir_string_view(text,bytes,length)&&*length&&!memchr(*bytes,0,*length));
 else CHECK(!text);return present;
}
static void host_directory_names(XrXirProgram *program,const uint32_t ids[6],const HostDirectoryFixture *fixture){
 XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;
 XrXirValue object=host_io_object(instance,ids[1],fixture->path,strlen(fixture->path));
 for(uint32_t id=XR_NATIVE_CALLABLE_IO_DIR_OPEN;id<=XR_NATIVE_CALLABLE_IO_DIR_CLOSE;++id){
  uint32_t function=UINT32_MAX;for(uint32_t f=0;f<program->entry_count;++f)if(program->declarations->functions[f].native_callable==id)function=f;
  unsigned before=directory_calls[id-XR_NATIVE_CALLABLE_IO_DIR_OPEN];XrXirValue argument=id==XR_NATIVE_CALLABLE_IO_DIR_OPEN?object:(XrXirValue){XR_XIR_I64,0,1};
  CHECK(function!=UINT32_MAX&&xr_xir_instance_start(instance,function,&argument,1)==XR_XIR_CALL_BAD_ARGUMENT&&directory_calls[id-XR_NATIVE_CALLABLE_IO_DIR_OPEN]==before);
 }
 XrXirValue token=host_invoke(instance,ids[2],&object,1);xr_xir_value_drop(&object);CHECK(token.type==XR_XIR_I64&&token.payload>0);
 bool seen[82]={false};unsigned count=0;XrXirValue retained={0};char saved[1024]={0};
 for(;;){
  XrXirValue name=host_invoke(instance,ids[3],&token,1);const char *text=NULL;size_t length=0;
  if(!host_directory_name(&name,&text,&length)){xr_xir_value_drop(&name);break;}
  unsigned index=UINT32_MAX;
  if(length==strlen(fixture->long_text)&&!memcmp(text,fixture->long_text,length))index=80;
  else if(length==6&&!memcmp(text,"nested",6))index=81;
  else for(unsigned n=0;n<80;++n){char expected[16];int bytes=snprintf(expected,sizeof(expected),"entry-%02u",n);CHECK(bytes>0);
   if(length==(size_t)bytes&&!memcmp(text,expected,length)){index=n;break;}}
  CHECK(index<82&&!seen[index]);seen[index]=true;++count;
  if(!retained.type){CHECK(length<sizeof(saved));memcpy(saved,text,length);retained=name;name=(XrXirValue){0};}
  xr_xir_value_drop(&name);
 }
 CHECK(count==82);for(unsigned n=0;n<82;++n)CHECK(seen[n]);
 unsigned next=directory_calls[1];XrXirValue end=host_invoke(instance,ids[3],&token,1);const char *text=NULL;size_t length=0;
 CHECK(!host_directory_name(&end,&text,&length)&&directory_calls[1]==next);xr_xir_value_drop(&end);
 host_io_bool(instance,ids[4],&token,1,true);CHECK(!instance->file_metadata_owners);
 /* A ref-bound Path and a legal subclass retain the same canonical authority. */
 XrXirValue path={0};CHECK(xr_xir_string_new(instance->domain,fixture->path,strlen(fixture->path),&path)==XR_XIR_VALUE_OK);
 XrXirValue other=host_invoke(instance,ids[5],&path,1);xr_xir_value_drop(&path);CHECK(other.type==XR_XIR_I64&&other.payload>0&&other.payload!=token.payload);
 host_io_bool(instance,ids[4],&other,1,true);xr_xir_value_drop(&other);
 next=directory_calls[1];host_io_failure(instance,ids[3],&token,1,XR_XIR_CALL_BAD_ARGUMENT);CHECK(directory_calls[1]==next);
 xr_xir_value_drop(&token);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_BAD_ARGUMENT);host_active=NULL;
 CHECK(host_directory_name(&retained,&text,&length)&&length==strlen(saved)&&!memcmp(text,saved,length));xr_xir_value_drop(&retained);
}
static void host_directory_authority(const XrXirCompileContext *context,XrXirProgram *program,const uint32_t ids[6],const char *path){
 XrXirInstance *first=host_instance(program,ids[0]);host_active=first;XrXirValue token=host_directory_open_path(first,ids,path);
 XrXirInstance *second=host_instance(program,ids[0]);host_active=second;XrXirValue other=host_directory_open_path(second,ids,path);
 CHECK(other.payload!=token.payload);unsigned next=directory_calls[1];
 host_io_phase="directory foreign root";
 host_io_failure(second,ids[3],&token,1,XR_XIR_CALL_BAD_ARGUMENT);CHECK(directory_calls[1]==next);
 xr_xir_value_drop(&other);CHECK(xr_xir_instance_free(second)==XR_XIR_CALL_BAD_ARGUMENT);host_active=first;
 XrXirProgram *replica=host_directory_replica(context,program);uint64_t generation=0;
 CHECK(replica!=program&&xr_xir_instance_generation_prepare(first,replica,&generation)==XR_XIR_CALL_READY&&generation);
 XrXirValue initialized=host_invoke(first,ids[0],NULL,0);CHECK(initialized.type==XR_XIR_I64&&!initialized.payload);xr_xir_value_drop(&initialized);
 host_io_phase="directory candidate generation";
 host_io_failure(first,ids[3],&token,1,XR_XIR_CALL_BAD_ARGUMENT);CHECK(directory_calls[1]==next);
 CHECK(xr_xir_instance_generation_abort(first,generation)==XR_XIR_CALL_BAD_ARGUMENT);xr_xir_compile_program_drop(replica);
 XrXirValue name=host_invoke(first,ids[3],&token,1);const char *text=NULL;size_t length=0;CHECK(host_directory_name(&name,&text,&length));xr_xir_value_drop(&name);
 host_io_bool(first,ids[4],&token,1,true);xr_xir_value_drop(&token);CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY);host_active=NULL;
 second=host_instance(program,ids[0]);host_active=second;XrXirValue fake={XR_XIR_I64,0,0};next=directory_calls[1];
 host_io_phase="directory fake token";
 host_io_failure(second,ids[3],&fake,1,XR_XIR_CALL_BAD_ARGUMENT);CHECK(directory_calls[1]==next&&xr_xir_instance_free(second)==XR_XIR_CALL_BAD_ARGUMENT);host_active=NULL;
}
static void host_directory_pending(XrXirProgram *program,const uint32_t ids[6],const char *path){
 /* OOM/cancel follow actual acquisition; the IO adapter fault precedes it. */
 for(unsigned mode=DIR_NEXT_OOM;mode<=DIR_NEXT_ERROR;++mode){
  XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;directory_mode=DIR_NORMAL;
  XrXirValue token=host_directory_open_path(instance,ids,path);unsigned next=directory_calls[1],opens=directory_calls[0];directory_mode=mode;
  XrXirCallStatus expected=mode==DIR_NEXT_OOM?XR_XIR_CALL_OOM:mode==DIR_NEXT_CANCEL?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_HOST_ERROR;
  host_io_failure(instance,ids[3],&token,1,expected);runtime_fail_at=SIZE_MAX;directory_mode=DIR_NORMAL;
  CHECK(directory_calls[1]==next+1&&instance->file_metadata_owners);XirFileMetadataOwner *node=instance->file_metadata_owners;
  CHECK(node->kind==XIR_DIRECTORY_OWNER&&node->directory.state&&node->directory_published);
  if(mode==DIR_NEXT_ERROR)CHECK(!node->pending_name.bytes&&!node->directory_end);
  else CHECK(node->pending_name.bytes&&node->pending_name.length==strlen(directory_observed_name)&&!memcmp(node->pending_name.bytes,directory_observed_name,node->pending_name.length));
  xr_xir_value_drop(&token);
  if(mode==DIR_NEXT_OOM){directory_close_failures=1;CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_CLOSE_FAILED&&node->pending_name.bytes&&node->directory.state);}
  CHECK(xr_xir_instance_free(instance)==(expected==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_READY:expected));host_active=NULL;
  CHECK(directory_calls[1]==next+1&&directory_calls[0]==opens);
 }
}
static void host_directory_close_controls(XrXirProgram *program,const uint32_t ids[6],const char *path){
 XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;XrXirValue token=host_directory_open_path(instance,ids,path);
 unsigned closes=directory_calls[2],next=directory_calls[1],opens=directory_calls[0];directory_close_failures=1;
 host_io_bool(instance,ids[4],&token,1,false);CHECK(instance->file_metadata_owners&&instance->file_metadata_owners->directory_closing);
 host_io_failure(instance,ids[3],&token,1,XR_XIR_CALL_BAD_ARGUMENT);CHECK(directory_calls[1]==next);xr_xir_value_drop(&token);
 directory_close_failures=1;CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_CLOSE_FAILED);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_BAD_ARGUMENT&&directory_calls[2]==closes+3&&directory_calls[0]==opens);host_active=NULL;
 instance=host_instance(program,ids[0]);host_active=instance;token=host_directory_open_path(instance,ids,path);closes=directory_calls[2];directory_mode=DIR_CLOSE_CANCEL;
 host_io_failure(instance,ids[4],&token,1,XR_XIR_CALL_CANCELLED);CHECK(!instance->file_metadata_owners&&directory_calls[2]==closes+1);xr_xir_value_drop(&token);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&directory_calls[2]==closes+1);host_active=NULL;
 instance=host_instance(program,ids[0]);host_active=instance;directory_mode=DIR_OPEN_CANCEL;
 XrXirValue object=host_io_object(instance,ids[1],path,strlen(path));opens=directory_calls[0];closes=directory_calls[2];
 host_io_failure(instance,ids[2],&object,1,XR_XIR_CALL_CANCELLED);directory_mode=DIR_NORMAL;xr_xir_value_drop(&object);
 CHECK(instance->file_metadata_owners&&!instance->file_metadata_owners->directory_published&&instance->file_metadata_owners->directory.state);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&directory_calls[2]==closes+1&&directory_calls[0]==opens+1);host_active=NULL;
}
#endif
static void host_io_directory_execute(const XrXirCompileContext *context,XrXirProgram *program,const uint32_t ids[6]){
 size_t live=runtime_live,bytes=runtime_bytes;directory_mode=DIR_NORMAL;directory_close_failures=0;
#if defined(XR_OS_WINDOWS)
 HostDirectoryFixture fixture={0};host_directory_fixture(&fixture,true);
 host_io_phase="directory names and stale token";host_directory_names(program,ids,&fixture);host_directory_authority(context,program,ids,fixture.path);
 host_io_phase="directory pending fault";host_directory_pending(program,ids,fixture.path);
 host_io_phase="directory close controls";host_directory_close_controls(program,ids,fixture.path);
 XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;char path[16384];
 CHECK(snprintf(path,sizeof(path),"%s/nested",fixture.path)>0);XrXirValue token=host_directory_open_path(instance,ids,path);
 XrXirValue empty=host_invoke(instance,ids[3],&token,1);const char *text=NULL;size_t length=0;CHECK(!host_directory_name(&empty,&text,&length));xr_xir_value_drop(&empty);
 host_io_bool(instance,ids[4],&token,1,true);xr_xir_value_drop(&token);
 const char *leaves[]={"entry-00","absent"};for(unsigned n=0;n<2;++n){CHECK(snprintf(path,sizeof(path),"%s/%s",fixture.path,leaves[n])>0);
  XrXirValue object=host_io_object(instance,ids[1],path,strlen(path));XrXirValue refused=host_invoke(instance,ids[2],&object,1);
  CHECK(refused.type==XR_XIR_I64&&!refused.payload&&!instance->file_metadata_owners);xr_xir_value_drop(&refused);xr_xir_value_drop(&object);}
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;host_directory_fixture(&fixture,false);
#else
 (void)context;XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;
 XrXirValue object=host_io_object(instance,ids[1],".",1);unsigned opens=directory_calls[0];
 host_io_failure(instance,ids[2],&object,1,XR_XIR_CALL_UNSUPPORTED);CHECK(directory_calls[0]==opens+1&&!instance->file_metadata_owners);
 xr_xir_value_drop(&object);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_UNSUPPORTED);host_active=NULL;
#endif
 host_io_phase="existing IO";CHECK(directory_mode==DIR_NORMAL&&!directory_close_failures&&runtime_live==live&&runtime_bytes==bytes);
}
#endif // HOST_OS_IO_DIRECTORY_CASES_H
