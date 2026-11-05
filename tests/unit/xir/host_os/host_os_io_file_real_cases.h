/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_file_real_cases.h - Public File bytes and independently closed files
 *
 * KEY CONCEPT:
 *   Real public File calls must preserve explicit byte offsets and caller data.
 *   Windows exclusive opens verify closure without inspecting a token registry.
 */
#ifndef HOST_OS_IO_FILE_REAL_CASES_H
#define HOST_OS_IO_FILE_REAL_CASES_H
#include "os/os_time.h"
#include "xir/xxir_view_value_internal.h"
#include "xir/xxir_buffer_storage.h"
static bool host_file_stdio_child;
enum { HOST_REAL_ENTRY,HOST_REAL_PATH,HOST_REAL_ROUNDTRIP,HOST_REAL_DEFER,HOST_REAL_STDIO,
 HOST_REAL_OPEN,HOST_REAL_READ,HOST_REAL_CLOSE,HOST_REAL_FLUSH,HOST_REAL_WRITE,HOST_REAL_WRITE_STR,
 HOST_REAL_LOSSY,HOST_REAL_LOSSY_CHECK,HOST_REAL_CONSTRUCTOR,HOST_REAL_EAGER,HOST_REAL_EMPTY,HOST_REAL_FIELD_PLACES,HOST_REAL_BUFFERED,HOST_REAL_CTOR_HELPER,HOST_REAL_COUNT };
#if defined(XR_OS_WINDOWS)
static const unsigned char host_file_real_input[]={0x41,0x0D,0x0A,0x1A,0,0xFF,0x42};
/* The two-byte UTF-8 prefix makes offset two distinct from a rune index. */
static const unsigned char host_file_real_text[]={0xC3,0xA9,0,0xE4,0xB8,0xAD};
static const unsigned char host_file_real_expected[]={0x0D,0x0A,0x1A,0,0xFF,0x42,0,0xE4,0xB8,0xAD};
typedef struct HostFileRealFixture {char root[4096],path[8192];} HostFileRealFixture;
static void host_file_real_fixture(HostFileRealFixture *fixture,unsigned phase){
 static const unsigned char previous[]="previous contents must be truncated";
 wchar_t root[4096],path[8192];
 if(!phase){
  DWORD length=GetTempPathW(3000,root);CHECK(length&&length<3000);
  CHECK(swprintf_s(root+length,4096-length,L"xray-file-real-%lu-\x4e2d",GetCurrentProcessId())>0&&CreateDirectoryW(root,NULL));
  CHECK(swprintf_s(path,8192,L"%ls\\\x6587\x4ef6.bin",root)>0);
  CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,root,-1,fixture->root,(int)sizeof(fixture->root),NULL,NULL)>0);
  CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,fixture->path,(int)sizeof(fixture->path),NULL,NULL)>0);
 }else{
  CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,fixture->root,-1,root,4096)>0);
  CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,fixture->path,-1,path,8192)>0);
 }
 if(phase==2){CHECK(DeleteFileW(path)&&RemoveDirectoryW(root));return;}
 /* Share mode zero would reject an outstanding File read or write handle. */
 HANDLE file=CreateFileW(path,phase?GENERIC_READ:GENERIC_WRITE,0,NULL,phase?OPEN_EXISTING:CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 CHECK(file!=INVALID_HANDLE_VALUE);DWORD count=0;
 if(!phase)CHECK(WriteFile(file,previous,(DWORD)(sizeof(previous)-1),&count,NULL)&&count==sizeof(previous)-1);
 else{
  unsigned char actual[sizeof(host_file_real_expected)+1]={0};
  CHECK(ReadFile(file,actual,(DWORD)sizeof(actual),&count,NULL)&&count==sizeof(host_file_real_expected)&&
   !memcmp(actual,host_file_real_expected,sizeof(host_file_real_expected)));
 }
 CHECK(CloseHandle(file));
}
#endif
static XrXirInstanceResult host_file_real_drive(XrXirInstance *instance){
 uint64_t beginning=xr_time_monotonic_ns();CHECK(beginning);
 for(;;){
  XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);XrXirCallStatus status=event.outcome.status;
  if(status==XR_XIR_CALL_SUSPENDED){
   XrXirWaitRequest wait={0};CHECK(xr_xir_instance_wait_request(instance,event.epoch,event.outcome.wake,&wait)==XR_XIR_CALL_READY);
   CHECK(wait.kind==XR_XIR_WAIT_YIELD&&xr_xir_instance_resume(instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_READY);xr_thread_yield();
  }else if(status!=XR_XIR_CALL_READY)return event;
  uint64_t now=xr_time_monotonic_ns();CHECK(now&&now>=beginning&&now-beginning<UINT64_C(30000000000));
 }
}
static XrXirValue host_file_real_invoke(XrXirInstance *instance,uint32_t function,const XrXirValue *arguments,uint32_t count){
 CHECK(xr_xir_instance_start(instance,function,arguments,count)==XR_XIR_CALL_READY);
 XrXirCallStatus status=host_file_real_drive(instance).outcome.status;
 if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"real File function%u status%u\n",function,status);
 CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue output={0};CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_RETURNED);return output;
}
static void host_file_lossy_execute(XrXirProgram *program,const uint32_t *ids){
 static const struct {unsigned char bytes[12];size_t length;const char *text;size_t size;int64_t runes;} cases[]={
  {{0},0,"",0,0},
  {{65,0,0xC3,0xA9,0xE4,0xB8,0xAD,0xF0,0x9F,0x98,0x80},11,"A\0\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80",11,5},
  {{0xFF},1,"\xEF\xBF\xBD",3,1},
  {{0xE2,0x28,0xA1},3,"\xEF\xBF\xBD(\xEF\xBF\xBD",7,3},
  {{0xF0,0x9F,0x92},3,"\xEF\xBF\xBD",3,1},
  {{0xF4,0x90,0x80,0x80},4,"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD",12,4},
  {{0xC0,0xAF},2,"\xEF\xBF\xBD\xEF\xBF\xBD",6,2},
  {{0xED,0xA0,0x80},3,"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD",9,3}
 };
 size_t live=runtime_live,physical=runtime_bytes;XrXirInstanceConfig config;XrXirInstance *instance=NULL;
 CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY&&xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 XrXirValueAdmission admission={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
 XrXirValue held={0};
 for(unsigned n=0;n<sizeof(cases)/sizeof(*cases);++n){
  XrXirValue elements[12]={0},input={0};for(size_t b=0;b<cases[n].length;++b)elements[b]=(XrXirValue){XR_XIR_U8,0,cases[n].bytes[b]};
  CHECK(xr_xir_array_new(program->entries[ids[HOST_REAL_LOSSY]].parameters[0],elements,(int64_t)cases[n].length,&admission,&input)==XR_XIR_VALUE_OK);
  XrXirValue output=host_file_real_invoke(instance,ids[HOST_REAL_LOSSY],&input,1);const char *text=NULL;size_t size=0;int64_t runes=-1;
  CHECK(xr_xir_string_view(&output,&text,&size)&&size==cases[n].size&&!memcmp(text,cases[n].text,size)&&
   xr_xir_string_length(&output,&runes)==XR_XIR_VALUE_OK&&runes==cases[n].runes);
  for(size_t b=0;b<cases[n].length;++b){XrXirValue byte={0};XrXirFaultDetail fault={0};
   CHECK(xr_xir_array_get(&input,(int64_t)b,&admission,&byte,&fault)==XR_XIR_VALUE_OK&&byte.payload==cases[n].bytes[b]);xr_xir_value_drop(&byte);}
  xr_xir_value_drop(&input);xr_xir_value_drop(&held);held=output;
 }
 XrXirValue checked=host_file_real_invoke(instance,ids[HOST_REAL_LOSSY_CHECK],NULL,0);CHECK(checked.type==XR_XIR_BOOL&&checked.payload==1);xr_xir_value_drop(&checked);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);const char *bytes=NULL;size_t length=0;
 CHECK(xr_xir_string_view(&held,&bytes,&length)&&length==9&&!memcmp(bytes,cases[7].text,length));xr_xir_value_drop(&held);
 CHECK(runtime_live==live&&runtime_bytes==physical);
}
static void host_file_field_places_execute(XrXirProgram *program,const uint32_t *ids){
 size_t live=runtime_live,physical=runtime_bytes;XrXirInstanceConfig config;XrXirInstance *instance=NULL;
 CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY&&xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 XrXirValueAdmission admission={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
 XrXirValue element={XR_XIR_I64,0,9},input={0};
 CHECK(xr_xir_array_new(program->entries[ids[HOST_REAL_FIELD_PLACES]].parameters[0],&element,1,&admission,&input)==XR_XIR_VALUE_OK);
 XrXirValue result=host_file_real_invoke(instance,ids[HOST_REAL_FIELD_PLACES],&input,1),value={0};XrXirFaultDetail fault={0};int64_t length=-1;
 CHECK(xr_xir_array_len(&input,&admission,&length)==XR_XIR_VALUE_OK&&length==1);
 CHECK(xr_xir_array_get(&input,0,&admission,&value,&fault)==XR_XIR_VALUE_OK&&value.type==XR_XIR_I64&&value.payload==9);xr_xir_value_drop(&value);
 CHECK(xr_xir_array_len(&result,&admission,&length)==XR_XIR_VALUE_OK&&length==2);
 for(int64_t n=0;n<2;++n){CHECK(xr_xir_array_get(&result,n,&admission,&value,&fault)==XR_XIR_VALUE_OK&&value.type==XR_XIR_I64&&value.payload==7+n);xr_xir_value_drop(&value);}
 xr_xir_value_drop(&result);xr_xir_value_drop(&input);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
 CHECK(runtime_live==live&&runtime_bytes==physical);
}
#if defined(XR_OS_WINDOWS)
static void host_file_real_array(const XrXirValue *value,const unsigned char *expected,size_t count,XrXirValueAdmission *admission){
 int64_t length=-1;CHECK(xr_xir_array_len(value,admission,&length)==XR_XIR_VALUE_OK&&length==(int64_t)count);
 for(size_t n=0;n<count;++n){XrXirValue element={0};XrXirFaultDetail fault={0};
  CHECK(xr_xir_array_get(value,(int64_t)n,admission,&element,&fault)==XR_XIR_VALUE_OK&&element.type==XR_XIR_U8&&element.payload==expected[n]);
  xr_xir_value_drop(&element);}
}
static void host_file_view_conflicts(XrXirProgram *program,XrXirInstance *instance,XrXirType array_type){
 size_t live=runtime_live,bytes=runtime_bytes;const XrXirTypes *types=xr_xir_compile_type_arena_types(program->arena);
 XrXirType cell_type=XR_XIR_UNIT,slice_type=XR_XIR_UNIT;
 for(uint32_t n=0;n<types->count;++n){const XrXirTypeNode *node=types->nodes+n;
  if(node->kind==XR_XIR_TYPE_CELL&&node->element==array_type)cell_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n);
  if(node->kind==XR_XIR_TYPE_SLICE&&node->element==XR_XIR_U8)slice_type=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+n);}
 CHECK(cell_type!=XR_XIR_UNIT&&slice_type!=XR_XIR_UNIT);
 XrXirValueAdmission admission={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
 XrXirValue byte={XR_XIR_U8,0,42},array={0},cell={0},view={0};
 CHECK(xr_xir_array_new(array_type,&byte,1,&admission,&array)==XR_XIR_VALUE_OK);
 CHECK(xr_xir_cell_new(instance->domain,program->arena,cell_type,&array,&admission,&cell)==XR_XIR_VALUE_OK);
 XrXirCellAuthority authority={instance,&cell,1};XrXirViewStorageSpec spec={authority,0,1,XR_XIR_TYPE_SLICE};XrXirViewOriginPin pin={0};
 CHECK(xr_xir_view_new(slice_type,&cell,NULL,&spec,&admission,&view)==XR_XIR_VALUE_OK);xr_xir_view_scope_pin_created(&view,&authority,&pin);
 CHECK(xr_xir_cell_write(&cell,&array,&admission)==XR_XIR_VALUE_BAD_ARGUMENT);
 xr_xir_view_scope_close(&pin);xr_xir_value_drop(&view);CHECK(xr_xir_cell_write(&cell,&array,&admission)==XR_XIR_VALUE_OK);
 xr_xir_value_drop(&cell);xr_xir_value_drop(&array);
 XrXirValue storage={0};CHECK(xr_xir_buffer_storage_new(instance->domain,3,0,true,&storage)==XR_XIR_VALUE_OK);
 CHECK(xr_xir_buffer_storage_view(&storage,slice_type,&authority,&admission,&view)==XR_XIR_VALUE_OK);xr_xir_view_scope_pin_created(&view,&authority,&pin);
 bool resized=false;CHECK(xr_xir_buffer_storage_resize(&storage,4,&resized)==XR_XIR_VALUE_BAD_ARGUMENT&&!resized);
 xr_xir_view_scope_close(&pin);xr_xir_value_drop(&view);CHECK(xr_xir_buffer_storage_resize(&storage,4,&resized)==XR_XIR_VALUE_OK&&resized);
 xr_xir_value_drop(&storage);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static XrXirInstance *host_file_real_instance(XrXirProgram *program){
 XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
 XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);return instance;
}
static XrXirValue host_file_real_path(XrXirInstance *instance,const uint32_t *ids,const char *path){
 XrXirValue raw={0};CHECK(xr_xir_string_new(instance->domain,path,strlen(path),&raw)==XR_XIR_VALUE_OK);
 XrXirValue result=host_file_real_invoke(instance,ids[HOST_REAL_PATH],&raw,1);xr_xir_value_drop(&raw);return result;
}
static void host_file_real_none(const XrXirValue *value){
 bool some=true;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(value,&some,&payload)&&!some&&!payload);
}
static void host_file_real_retire(XrXirInstance *instance,XrXirCallStatus expected,const char *scenario,unsigned requested_mode){
 uint64_t start=xr_time_monotonic_ns();CHECK(start);
 for(;;){XrXirCallStatus status=xr_xir_instance_free(instance);
  if(status!=XR_XIR_CALL_CLOSE_FAILED){
   if(status!=expected)fprintf(stderr,"real File retire scenario=%s actual=%u expected=%u requestedMode=%u armedMode=%u observing=%u reads=%u closes=%u faultHits=%u\n",
    scenario,(unsigned)status,(unsigned)expected,requested_mode,file_mode,(unsigned)file_observing,file_read_calls,file_close_calls,file_fault_hits);
   CHECK(status==expected);return;}
  uint64_t now=xr_time_monotonic_ns();CHECK(now&&now>=start&&now-start<UINT64_C(5000000000));xr_thread_yield();}
}
static void host_file_real_authority(XrXirProgram *program,const uint32_t *ids,const char *path){
 size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *a=host_file_real_instance(program),*b=host_file_real_instance(program);
 XrXirValue object=host_file_real_path(a,ids,path),other=host_file_real_path(b,ids,path);
 XrXirValue token=host_file_real_invoke(a,ids[HOST_REAL_OPEN],&object,1);CHECK(token.type==XR_XIR_I64&&token.payload>=3);
 XrXirValue args[]={token,{XR_XIR_I64,0,1}};file_observing=true;unsigned reads=file_read_calls,closes=file_close_calls;
 XrXirValue output=host_file_real_invoke(b,ids[HOST_REAL_READ],args,2);host_file_real_none(&output);xr_xir_value_drop(&output);
 CHECK(file_read_calls==reads);uint32_t native=UINT32_MAX;
 for(uint32_t f=0;f<program->entry_count;++f)if(program->declarations->functions[f].native_callable==111u)native=f;
 CHECK(native!=UINT32_MAX&&xr_xir_instance_start(a,native,&token,1)==XR_XIR_CALL_BAD_ARGUMENT&&file_close_calls==closes);
 output=host_file_real_invoke(a,ids[HOST_REAL_CLOSE],&token,1);CHECK(output.type==XR_XIR_BOOL&&output.payload==1);xr_xir_value_drop(&output);
 output=host_file_real_invoke(a,ids[HOST_REAL_CLOSE],&token,1);CHECK(output.type==XR_XIR_BOOL&&!output.payload);xr_xir_value_drop(&output);
 args[0]=token;output=host_file_real_invoke(a,ids[HOST_REAL_READ],args,2);host_file_real_none(&output);xr_xir_value_drop(&output);
 args[0]=(XrXirValue){XR_XIR_I64,0,INT64_MAX};output=host_file_real_invoke(a,ids[HOST_REAL_READ],args,2);host_file_real_none(&output);xr_xir_value_drop(&output);
 CHECK(file_read_calls==reads&&file_close_calls==closes+1);file_observing=false;
 xr_xir_value_drop(&object);xr_xir_value_drop(&other);xr_xir_value_drop(&token);
 host_file_real_retire(a,XR_XIR_CALL_READY,"authority-owner",FILE_NORMAL);host_file_real_retire(b,XR_XIR_CALL_READY,"authority-foreign",FILE_NORMAL);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void host_file_real_fault(XrXirProgram *program,const uint32_t *ids,const char *path,unsigned mode,bool constructor){
 size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=host_file_real_instance(program);
 XrXirValue object=host_file_real_path(instance,ids,path),token=host_file_real_invoke(instance,ids[HOST_REAL_OPEN],&object,1);
 CHECK(token.type==XR_XIR_I64&&token.payload>=3);file_observing=true;file_mode=mode;file_fault_instance=instance;
 unsigned reads=file_read_calls,closes=file_close_calls,hits=file_fault_hits;file_observed_count=0;
 XrXirValue args[]={token,{XR_XIR_I64,0,4}};bool closing=mode==FILE_CLOSE_ONCE;
 CHECK(xr_xir_instance_start(instance,ids[closing?HOST_REAL_CLOSE:constructor?HOST_REAL_CONSTRUCTOR:HOST_REAL_READ],args,closing||constructor?1u:2u)==XR_XIR_CALL_READY);
 XrXirCallStatus expected=closing?XR_XIR_CALL_HOST_ERROR:mode==FILE_READ_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_CANCELLED;
 XrXirCallStatus status=host_file_real_drive(instance).outcome.status;runtime_fail_at=SIZE_MAX;
 if(status!=expected)fprintf(stderr,"File fault mode%u constructor%u status%u expected%u\n",mode,(unsigned)constructor,status,expected);
 CHECK(status==expected&&file_fault_hits==hits+1&&file_mode==FILE_NORMAL);
 XrXirValue output={0};CHECK(xr_xir_instance_take_result(instance,&output)==(expected==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_BAD_STATE:expected)&&
  !output.type&&!output.reserved&&!output.payload);
 if(!closing){CHECK(file_read_calls==reads+1&&file_observed_count==4&&!memcmp(file_observed_bytes,host_file_real_expected,4));
  if(mode==FILE_READ_OOM)CHECK(runtime_attempts>file_oom_allocation);}
 XrXirCallStatus retired=expected==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_READY:expected;
 xr_xir_value_drop(&object);xr_xir_value_drop(&token);host_file_real_retire(instance,retired,closing?"close-fault":constructor?"constructor-fault":"read-fault",mode);file_fault_instance=NULL;
 CHECK(file_read_calls==reads+(closing?0u:1u)&&file_close_calls==closes+(closing?2u:1u));
 file_observing=false;CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void host_file_constructor_execute(XrXirProgram *program,const uint32_t *ids,const char *path){
 size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=host_file_real_instance(program);
 XrXirValue object=host_file_real_path(instance,ids,path),token=host_file_real_invoke(instance,ids[HOST_REAL_OPEN],&object,1);
 CHECK(token.type==XR_XIR_I64&&token.payload>=3);
 unsigned reads=file_read_calls;file_observing=true;
 CHECK(xr_xir_instance_start(instance,ids[HOST_REAL_CTOR_HELPER],&token,1)==XR_XIR_CALL_BAD_ARGUMENT&&file_read_calls==reads);file_observing=false;
 XrXirValue output=host_file_real_invoke(instance,ids[HOST_REAL_CONSTRUCTOR],&token,1);CHECK(output.type==XR_XIR_I64&&output.payload==13);xr_xir_value_drop(&output);
 output=host_file_real_invoke(instance,ids[HOST_REAL_CLOSE],&token,1);CHECK(output.type==XR_XIR_BOOL&&!output.payload);xr_xir_value_drop(&output);
 output=host_file_real_invoke(instance,ids[HOST_REAL_EAGER],&object,1);CHECK(output.type==XR_XIR_I64&&output.payload==6);xr_xir_value_drop(&output);
 char empty[8192];wchar_t wide[8192];int size=snprintf(empty,sizeof(empty),"%s.empty",path);CHECK(size>0&&(size_t)size<sizeof(empty));
 CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,empty,-1,wide,8192)>0);
 HANDLE file=CreateFileW(wide,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(file!=INVALID_HANDLE_VALUE&&CloseHandle(file));
 XrXirValue empty_path=host_file_real_path(instance,ids,empty);
 output=host_file_real_invoke(instance,ids[HOST_REAL_EMPTY],&empty_path,1);CHECK(output.type==(uint32_t)program->entries[ids[HOST_REAL_EMPTY]].result);
 /* The returned iterator and its Instance remain live. Exclusive access here
  * proves the eager empty read closed the File before construction returned. */
 file=CreateFileW(wide,GENERIC_READ,0,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(file!=INVALID_HANDLE_VALUE);
 unsigned char byte=0;DWORD count=1;CHECK(ReadFile(file,&byte,1,&count,NULL)&&!count&&CloseHandle(file));
 xr_xir_value_drop(&output);xr_xir_value_drop(&empty_path);xr_xir_value_drop(&object);xr_xir_value_drop(&token);
 host_file_real_retire(instance,XR_XIR_CALL_READY,"eager-constructor",FILE_NORMAL);CHECK(DeleteFileW(wide)&&runtime_live==live&&runtime_bytes==bytes);
}
#endif
static void host_file_real_execute(XrXirProgram *program,const uint32_t ids[HOST_REAL_COUNT]){
 host_file_lossy_execute(program,ids);host_file_field_places_execute(program,ids);
#if defined(XR_OS_WINDOWS)
 size_t live=runtime_live,bytes=runtime_bytes;HostFileRealFixture fixture={0};host_file_real_fixture(&fixture,0);
 XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
 XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 XrXirValue initialized=host_file_real_invoke(instance,ids[0],NULL,0);CHECK(initialized.type==XR_XIR_I64&&!initialized.payload);xr_xir_value_drop(&initialized);
 XrXirValue raw={0},input={0},text={0};CHECK(xr_xir_string_new(instance->domain,fixture.path,strlen(fixture.path),&raw)==XR_XIR_VALUE_OK);
 XrXirValue path=host_file_real_invoke(instance,ids[1],&raw,1);xr_xir_value_drop(&raw);
 XrXirValueAdmission admission={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
 XrXirValue elements[sizeof(host_file_real_input)]={0};
 for(unsigned n=0;n<sizeof(host_file_real_input);++n)elements[n]=(XrXirValue){XR_XIR_U8,0,host_file_real_input[n]};
 CHECK(xr_xir_array_new(program->entries[ids[2]].parameters[1],elements,sizeof(host_file_real_input),&admission,&input)==XR_XIR_VALUE_OK);
 CHECK(xr_xir_string_new(instance->domain,(const char *)host_file_real_text,sizeof(host_file_real_text),&text)==XR_XIR_VALUE_OK);
 XrXirValue arguments[]={path,input,text};XrXirValue result=host_file_real_invoke(instance,ids[2],arguments,3);
 host_file_real_array(&result,host_file_real_expected,sizeof(host_file_real_expected),&admission);
 host_file_real_array(&input,host_file_real_input,sizeof(host_file_real_input),&admission);
 const char *actual=NULL;size_t length=0;CHECK(xr_xir_string_view(&text,&actual,&length)&&length==sizeof(host_file_real_text)&&!memcmp(actual,host_file_real_text,length));
 xr_xir_value_drop(&result);host_file_real_fixture(&fixture,1);
 static const unsigned char buffered_expected[]={0x41,0x0D,0x0A,0x1A,0,0xFF,0x42,0x41,0x0D,0x0A,0x1A,0,0xFF,0x42,
  0x41,0,0xE4,0xB8,0xAD,0,0,0,0x41,0,0xE4,0xB8,0xAD};
 XrXirValue buffered_args[]={path,input};result=host_file_real_invoke(instance,ids[HOST_REAL_BUFFERED],buffered_args,2);
 host_file_real_array(&result,buffered_expected,sizeof(buffered_expected),&admission);host_file_real_array(&input,host_file_real_input,sizeof(host_file_real_input),&admission);
 xr_xir_value_drop(&result);host_file_view_conflicts(program,instance,(XrXirType)input.type);host_file_real_fixture(&fixture,1);
 result=host_file_real_invoke(instance,ids[3],&path,1);CHECK(result.type==XR_XIR_I64&&result.payload==13);xr_xir_value_drop(&result);
 host_file_real_fixture(&fixture,1);xr_xir_value_drop(&path);xr_xir_value_drop(&input);xr_xir_value_drop(&text);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
 host_file_real_authority(program,ids,fixture.path);
 host_file_real_fault(program,ids,fixture.path,FILE_READ_OOM,false);host_file_real_fixture(&fixture,1);
 host_file_real_fault(program,ids,fixture.path,FILE_READ_CANCEL,false);host_file_real_fixture(&fixture,1);
 host_file_real_fault(program,ids,fixture.path,FILE_CLOSE_ONCE,false);host_file_real_fixture(&fixture,1);
 host_file_constructor_execute(program,ids,fixture.path);host_file_real_fixture(&fixture,1);
 host_file_real_fault(program,ids,fixture.path,FILE_READ_CANCEL,true);host_file_real_fixture(&fixture,1);
 host_file_real_fault(program,ids,fixture.path,FILE_READ_OOM,true);host_file_real_fixture(&fixture,1);
 host_file_real_fixture(&fixture,2);
#else
 size_t live=runtime_live,bytes=runtime_bytes;XrXirInstanceConfig config;
 CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
 CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 XrXirValue raw={0};CHECK(xr_xir_string_new(instance->domain,"unused",6,&raw)==XR_XIR_VALUE_OK);
 XrXirValue path=host_file_real_invoke(instance,ids[1],&raw,1);xr_xir_value_drop(&raw);
 XrXirValueAdmission admission={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
 XrXirValue input={0},text={0};CHECK(xr_xir_array_new(program->entries[ids[2]].parameters[1],NULL,0,&admission,&input)==XR_XIR_VALUE_OK);
 CHECK(xr_xir_string_new(instance->domain,"",0,&text)==XR_XIR_VALUE_OK);XrXirValue arguments[]={path,input,text};
 CHECK(xr_xir_instance_start(instance,ids[2],arguments,3)==XR_XIR_CALL_READY);XrXirInstanceResult event;
 do{event=xr_xir_instance_poll_bounded(instance,64);}while(event.outcome.status==XR_XIR_CALL_READY);
 CHECK(event.outcome.status==XR_XIR_CALL_UNSUPPORTED);XrXirValue output={0};
 CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_BAD_STATE&&!output.type&&!output.payload);
 xr_xir_value_drop(&path);xr_xir_value_drop(&input);xr_xir_value_drop(&text);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_UNSUPPORTED&&runtime_live==live&&runtime_bytes==bytes);
#endif
}
#if defined(XR_OS_WINDOWS)
static XrXirOutputStatus host_file_stdio_write(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
 (void)context;FILE *file=stream==XR_XIR_STDOUT?stdout:stream==XR_XIR_STDERR?stderr:NULL;
 return file&&fwrite(bytes,1,length,file)==length&&!fflush(file)?XR_XIR_OUTPUT_OK:XR_XIR_OUTPUT_ERROR;
}
#endif
static void host_file_stdio_execute(XrXirProgram *program,const uint32_t ids[HOST_REAL_COUNT]) {
#if defined(XR_OS_WINDOWS)
 size_t live=runtime_live,bytes=runtime_bytes;XrXirInstanceConfig config;
 CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
 XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,host_file_stdio_write,NULL,XR_RESOURCE_DEFAULT_LIMIT};
 config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
 XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 XrXirValue output=host_file_real_invoke(instance,ids[4],NULL,0);
 XrXirValueAdmission admission={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
 host_file_real_array(&output,host_file_real_expected,sizeof(host_file_real_expected),&admission);xr_xir_value_drop(&output);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&runtime_live==live&&runtime_bytes==bytes);
 for(unsigned n=0;n<3;++n)CHECK(_get_osfhandle(n)!=-1);
#else
 (void)program;(void)ids;CHECK(false);
#endif
}
static void host_file_stdio_parent(void) {
#if defined(XR_OS_WINDOWS)
 SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};HANDLE input[2]={0},output[2]={0},error[2]={0};
 CHECK(CreatePipe(input,input+1,&security,0)&&CreatePipe(output,output+1,&security,0)&&CreatePipe(error,error+1,&security,0));
 CHECK(SetHandleInformation(output[0],HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(error[0],HANDLE_FLAG_INHERIT,0));
 DWORD written=0;CHECK(WriteFile(input[1],host_file_real_expected,(DWORD)sizeof(host_file_real_expected),&written,NULL)&&written==sizeof(host_file_real_expected));
 CHECK(CloseHandle(input[1]));input[1]=NULL;
 wchar_t executable[32768],command[32768];DWORD length=GetModuleFileNameW(NULL,executable,32768);CHECK(length&&length<32768);
 CHECK(swprintf(command,32768,L"\"%ls\" --file-stdio-child",executable)>0);
 STARTUPINFOW startup={0};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
 startup.hStdInput=input[0];startup.hStdOutput=output[1];startup.hStdError=error[1];PROCESS_INFORMATION process={0};
 CHECK(CreateProcessW(executable,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process));
 CHECK(CloseHandle(process.hThread)&&CloseHandle(input[0])&&CloseHandle(output[1])&&CloseHandle(error[1]));
 DWORD wait=WaitForSingleObject(process.hProcess,30000);
 if(wait!=WAIT_OBJECT_0){CHECK(TerminateProcess(process.hProcess,1));CHECK(WaitForSingleObject(process.hProcess,30000)==WAIT_OBJECT_0);}
 DWORD code=1;CHECK(GetExitCodeProcess(process.hProcess,&code)&&CloseHandle(process.hProcess));
 unsigned char actual_output[4096],actual_error[4096];DWORD out_count=0,error_count=0;
 BOOL out_ok=ReadFile(output[0],actual_output,sizeof(actual_output),&out_count,NULL);
 if(!out_ok)CHECK(GetLastError()==ERROR_BROKEN_PIPE);
 BOOL error_ok=ReadFile(error[0],actual_error,sizeof(actual_error),&error_count,NULL);
 if(!error_ok)CHECK(GetLastError()==ERROR_BROKEN_PIPE);
 CHECK(CloseHandle(output[0])&&CloseHandle(error[0]));
 unsigned char expected[sizeof(host_file_real_expected)+5];memcpy(expected,"AB\n",3);
 memcpy(expected+3,host_file_real_expected,sizeof(host_file_real_expected));memcpy(expected+3+sizeof(host_file_real_expected),"C\n",2);
 if(wait!=WAIT_OBJECT_0||code||out_count!=sizeof(expected)||error_count!=sizeof(host_file_real_expected)) {
  fprintf(stderr,"File stdio child wait%lu exit%lu stdout%lu stderr%lu\n",wait,code,out_count,error_count);
  if(error_count)CHECK(fwrite(actual_error,1,error_count,stderr)==error_count);
 }
 CHECK(wait==WAIT_OBJECT_0&&!code&&out_count==sizeof(expected)&&!memcmp(actual_output,expected,sizeof(expected)));
 CHECK(error_count==sizeof(host_file_real_expected)&&!memcmp(actual_error,host_file_real_expected,sizeof(host_file_real_expected)));
#endif
}
#endif // HOST_OS_IO_FILE_REAL_CASES_H
