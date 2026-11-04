/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_temp_cases.h - Prepared filesystem results and committed returns
 *
 * KEY CONCEPT:
 *   Provider fault injection preserves real unpublished owners. The separate
 *   host Call probe tests ownership transfer without granting native authority.
 */
#ifndef HOST_OS_IO_TEMP_CASES_H
#define HOST_OS_IO_TEMP_CASES_H
enum { HOST_MOVE_OLD,HOST_MOVE_COMMIT,HOST_MOVE_FAIL,HOST_MOVE_STACK,
 HOST_MOVE_OOM,HOST_MOVE_LIMIT,HOST_MOVE_CANCEL };
typedef struct HostReturnCommit {
 XrXirValue initial;unsigned mode,commits,releases;XrXirCallStatus observed;
} HostReturnCommit;
static XrXirCallStatus host_return_commit(void *context){
 HostReturnCommit *probe=context;++probe->commits;
 if(probe->mode==HOST_MOVE_FAIL)return XR_XIR_CALL_HOST_ERROR;
 /* A successful commit leaves no allocation opportunity in the return tail. */
 runtime_fail_at=runtime_attempts;return XR_XIR_CALL_READY;
}
static XrXirAction host_return_resume(XrXirCallView *view){
 HostReturnCommit *probe=view->instance;XrXirValue *source=view->state;
 CHECK(view->phase==XR_XIR_CALL_NORMAL&&xr_xir_value_copy(&probe->initial,source)==XR_XIR_VALUE_OK);
 XrXirAction action,before;memset(&action,0,sizeof(action));action.kind=XR_XIR_ACTION_CONTINUE;action.callee=42;
 memcpy(&before,&action,sizeof(action));XrXirValue saved=*source;
 if(probe->mode==HOST_MOVE_STACK){
  XrXirValue local={0};CHECK(xr_xir_value_copy(source,&local)==XR_XIR_VALUE_OK);
  CHECK(xir_call_return_move_commit(view,&local,&action,host_return_commit,probe)==XR_XIR_CALL_BAD_ARGUMENT);
  CHECK(!memcmp(&local,source,sizeof(local))&&!memcmp(&action,&before,sizeof(action))&&!probe->commits);
  xr_xir_value_drop(&local);
 }else if(probe->mode==HOST_MOVE_CANCEL)
  CHECK(xr_xir_call_request_cancel(view->activation)==XR_XIR_CALL_CANCEL_REQUESTED);
 if(probe->mode==HOST_MOVE_OOM)runtime_fail_at=runtime_attempts;
 probe->observed=probe->mode==HOST_MOVE_OLD||probe->mode==HOST_MOVE_STACK?
  xr_xir_call_return_move(view,source,&action):xir_call_return_move_commit(view,source,&action,host_return_commit,probe);
 if(probe->observed==XR_XIR_CALL_READY){
  CHECK(!source->type&&!source->reserved&&!source->payload&&action.kind==XR_XIR_ACTION_RETURN);
  CHECK(!memcmp(&saved,&action.value,sizeof(saved)));return action;
 }
 runtime_fail_at=SIZE_MAX;
 CHECK(!memcmp(source,&saved,sizeof(saved))&&!memcmp(&action,&before,sizeof(action)));
 CHECK(probe->commits==(probe->mode==HOST_MOVE_FAIL?1u:0u));
 return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,probe->observed}};
}
static void host_return_release(XrXirCallView *view,XrXirCallStatus reason){
 HostReturnCommit *probe=view->instance;XrXirValue *source=view->state;(void)reason;++probe->releases;
 if(probe->observed==XR_XIR_CALL_READY)CHECK(!source->type&&!source->payload&&!source->reserved);
 else CHECK(!memcmp(source,&probe->initial,sizeof(*source)));
 xr_xir_value_drop(source);
}
static void host_return_protocol(XrXirInstance *instance,XrXirType type){
 const XrXirTypes *types=instance->program->types;const XrXirTypeNode *outer=xr_xir_type_node(types,type);
 CHECK(outer&&outer->kind==XR_XIR_TYPE_ARRAY);const XrXirTypeNode *inner=xr_xir_type_node(types,outer->element);
 CHECK(inner&&inner->kind==XR_XIR_TYPE_ARRAY);XrXirValueAdmission admission={.arena=instance->program->arena,
  .domain=instance->domain,.work=1000000,.scratch_bytes=1000000};
 XrXirValue text={0},some={0},row={0},initial={0};
 CHECK(xr_xir_string_new(instance->domain,"owned",5,&text)==XR_XIR_VALUE_OK);
 CHECK(xr_xir_nullable_new(inner->element,&text,&admission,&some)==XR_XIR_VALUE_OK);
 XrXirValue fields[]={some,some};CHECK(xr_xir_array_new(outer->element,fields,2,&admission,&row)==XR_XIR_VALUE_OK);
 XrXirValue rows[]={row,row};CHECK(xr_xir_array_new(type,rows,2,&admission,&initial)==XR_XIR_VALUE_OK);
 xr_xir_value_drop(&row);xr_xir_value_drop(&some);xr_xir_value_drop(&text);
 for(unsigned mode=HOST_MOVE_OLD;mode<=HOST_MOVE_CANCEL;++mode){
  HostReturnCommit probe={.initial=initial,.mode=mode};XrXirCallAccounting accounting={0};
  XrXirCallEntry entry={XR_XIR_CALL_ABI_VERSION,NULL,0,type,sizeof(XrXirValue),host_return_resume,host_return_release,NULL,0,0};
  XrXirCallConfig config;CHECK(xr_xir_call_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
  config.entries=&entry;config.entry_count=1;config.instance=&probe;config.accounting=&accounting;
  config.admission=admission;if(mode==HOST_MOVE_LIMIT)config.admission.scratch_bytes=0;
  XrXirCall *call=NULL;CHECK(xr_xir_call_new(&config,0,NULL,0,&call)==XR_XIR_CALL_READY);
  XrXirCallResult result=xr_xir_call_poll_bounded(call,32);runtime_fail_at=SIZE_MAX;
  XrXirCallStatus expected=mode==HOST_MOVE_FAIL?XR_XIR_CALL_HOST_ERROR:mode==HOST_MOVE_OOM?XR_XIR_CALL_OOM:
   mode==HOST_MOVE_LIMIT?XR_XIR_CALL_LIMIT:mode==HOST_MOVE_CANCEL?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_RETURNED;
  CHECK(result.status==expected&&probe.releases==1);
  CHECK(probe.observed==(expected==XR_XIR_CALL_RETURNED?XR_XIR_CALL_READY:expected));
  XrXirValue output={0};CHECK(xr_xir_call_take_result(call,&output)==(expected==XR_XIR_CALL_RETURNED?expected:XR_XIR_CALL_BAD_STATE));
  if(expected==XR_XIR_CALL_RETURNED)CHECK(!memcmp(&output,&initial,sizeof(output)));
  else CHECK(!output.type&&!output.reserved&&!output.payload);
  CHECK(probe.commits==(mode==HOST_MOVE_COMMIT||mode==HOST_MOVE_FAIL?1u:0u));
  xr_xir_value_drop(&output);CHECK(xr_xir_call_free(call)==XR_XIR_CALL_READY&&!accounting.live_bytes&&!accounting.depth&&accounting.allocations==accounting.frees);
 }
 xr_xir_value_drop(&initial);
}
#if defined(XR_OS_WINDOWS)
static void host_temp_text(const XrXirValue *value,char *output,size_t capacity){
 bool present=false;const XrXirValue *text=NULL;const char *bytes=NULL;size_t length=0;
 CHECK(xr_xir_nullable_view(value,&present,&text)&&present&&text&&xr_xir_string_view(text,&bytes,&length));
 CHECK(length&&length<capacity&&!memchr(bytes,0,length));memcpy(output,bytes,length);output[length]=0;
}
static void host_temp_check(const char *path,bool directory){
 wchar_t wide[4096];CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,4096)>0);
 DWORD attributes=GetFileAttributesW(wide);CHECK(attributes!=INVALID_FILE_ATTRIBUTES&&((attributes&FILE_ATTRIBUTE_DIRECTORY)!=0)==directory);
 if(!directory){WIN32_FILE_ATTRIBUTE_DATA data;CHECK(GetFileAttributesExW(wide,GetFileExInfoStandard,&data)&&!data.nFileSizeHigh&&!data.nFileSizeLow);}
}
static void host_temp_remove(const char *path,bool directory){
 wchar_t wide[4096];CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,4096)>0);
 CHECK(directory?RemoveDirectoryW(wide):DeleteFileW(wide));
}
#endif
static void host_io_temp_execute(XrXirProgram *program,const uint32_t ids[4]){
 size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;
 host_return_protocol(instance,program->entries[ids[3]].result);
 XrXirValue root={0};
#if defined(XR_OS_WINDOWS)
 wchar_t wide[1024];DWORD length=GetTempPathW(900,wide);CHECK(length&&length<900);
 CHECK(swprintf_s(wide+length,1024-length,L"xray-temp-product-%lu-\x6d4b\x8bd5",GetCurrentProcessId())>0&&CreateDirectoryW(wide,NULL));
 char directory[4096];CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,directory,sizeof(directory),NULL,NULL)>0);
#else
 const char *directory="/tmp";
#endif
 CHECK(xr_xir_string_new(instance->domain,directory,strlen(directory),&root)==XR_XIR_VALUE_OK);
 for(uint32_t id=XR_NATIVE_CALLABLE_IO_MAKE_TEMP_DIR;id<=XR_NATIVE_CALLABLE_IO_MAKE_TEMP_FILE;++id){
  uint32_t function=UINT32_MAX;for(uint32_t f=0;f<program->entry_count;++f)if(program->declarations->functions[f].native_callable==id)function=f;
  unsigned before=io_temp_creates;CHECK(function!=UINT32_MAX&&xr_xir_instance_start(instance,function,&root,1)==XR_XIR_CALL_BAD_ARGUMENT&&io_temp_creates==before);
 }
#if defined(XR_OS_WINDOWS)
 XrXirValue kept[2]={{0},{0}};char names[2][4096];io_temp_mode=IO_TEMP_NORMAL;
 for(unsigned operation=0;operation<2;++operation){
  unsigned creates=io_temp_creates,keeps=io_temp_keeps;kept[operation]=host_invoke(instance,ids[operation+1],&root,1);
  CHECK(io_temp_creates==creates+1&&io_temp_keeps==keeps+1&&!instance->file_metadata_owners);
  host_temp_text(&kept[operation],names[operation],sizeof(names[operation]));host_temp_check(names[operation],operation==0);
 }
 XrXirValue invalid={0};unsigned provider=io_temp_creates;
 CHECK(xr_xir_string_new(instance->domain,"\xFF",1,&invalid)==XR_XIR_VALUE_BAD_UTF8&&!invalid.type&&!invalid.payload&&io_temp_creates==provider);
 char missing[4096];CHECK(snprintf(missing,sizeof(missing),"%s/absent",directory)>0);
 CHECK(xr_xir_string_new(instance->domain,missing,strlen(missing),&invalid)==XR_XIR_VALUE_OK);
 XrXirValue none=host_invoke(instance,ids[1],&invalid,1);bool present=true;const XrXirValue *payload=NULL;
 CHECK(xr_xir_nullable_view(&none,&present,&payload)&&!present&&!payload&&!instance->file_metadata_owners);
 xr_xir_value_drop(&none);xr_xir_value_drop(&invalid);
 xr_xir_value_drop(&root);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
 for(unsigned operation=0;operation<2;++operation){char retained[4096];host_temp_text(&kept[operation],retained,sizeof(retained));
  CHECK(!strcmp(retained,names[operation]));xr_xir_value_drop(&kept[operation]);host_temp_remove(names[operation],operation==0);
 }
 /* These are provider-injected failures, not reported Windows close failures. */
 for(unsigned operation=0;operation<2;++operation)for(unsigned mode=IO_TEMP_PREPARE_OOM;mode<=IO_TEMP_AFTER_KEEP_OOM;++mode){
  instance=host_instance(program,ids[0]);host_active=instance;io_temp_mode=mode;io_temp_candidate[0]=0;
  CHECK(xr_xir_string_new(instance->domain,directory,strlen(directory),&root)==XR_XIR_VALUE_OK);
  unsigned creates=io_temp_creates,keeps=io_temp_keeps,rollbacks=io_temp_rollbacks;
  if(mode==IO_TEMP_AFTER_KEEP_OOM){
   XrXirValue value=host_invoke(instance,ids[operation+1],&root,1);runtime_fail_at=SIZE_MAX;
   char path[4096];host_temp_text(&value,path,sizeof(path));CHECK(host_io_exists(path)&&io_temp_keeps==keeps+1);
   xr_xir_value_drop(&value);xr_xir_value_drop(&root);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
   host_temp_remove(path,operation==0);continue;
  }
  XrXirCallStatus expected=mode==IO_TEMP_PREPARE_OOM?XR_XIR_CALL_OOM:mode==IO_TEMP_PREPARE_CANCEL?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_HOST_ERROR;
  host_io_failure(instance,ids[operation+1],&root,1,expected);runtime_fail_at=SIZE_MAX;
  CHECK(io_temp_creates==creates+1&&io_temp_candidate[0]);xr_xir_value_drop(&root);
  if(mode==IO_TEMP_KEEP_FAIL){
   CHECK(io_temp_keeps==keeps+1&&instance->file_metadata_owners);io_temp_rollback_failures=1;
   CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_CLOSE_FAILED&&instance->file_metadata_owners);
   CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_HOST_ERROR&&io_temp_rollbacks==rollbacks+2&&io_temp_creates==creates+1&&io_temp_keeps==keeps+1);
  }else{CHECK(io_temp_keeps==keeps);CHECK(xr_xir_instance_free(instance)==(expected==XR_XIR_CALL_CANCELLED?XR_XIR_CALL_READY:expected));}
  host_active=NULL;CHECK(!host_io_exists(io_temp_candidate));
 }
 io_temp_mode=IO_TEMP_NORMAL;CHECK(RemoveDirectoryW(wide));
#else
 host_io_failure(instance,ids[1],&root,1,XR_XIR_CALL_UNSUPPORTED);xr_xir_value_drop(&root);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_UNSUPPORTED);host_active=NULL;
#endif
 CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#endif // HOST_OS_IO_TEMP_CASES_H
