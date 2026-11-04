/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * dylib_runtime.h - Actual data symbols, origin retirement and thread errors
 *
 * KEY CONCEPT:
 *   Tokens are used only through Source calls. Real data pointers retain origin
 *   metadata, while unload and close retry retain their actual OS obligations.
 */
#ifndef DYLIB_RUNTIME_H
#define DYLIB_RUNTIME_H
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_native_callable.h"
#include "xir/xxir_pointer.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_dylib_owned.h"
#include "os/os_time.h"
#include "os/os_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#ifdef interface
#undef interface
#endif
#include <io.h>
#include <fcntl.h>
#endif
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Dylib product %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#if defined(XR_OS_WINDOWS)
static XrOsIoStatus dylib_observed_symbol(XrOsDylibOwner *,const XrOsDylibQuery *,XrOsDylibText,uintptr_t *,bool *);
static XrOsIoStatus dylib_observed_close(XrOsDylibOwner *,bool *);
#define xr_os_io_dylib_symbol dylib_observed_symbol
#define xr_os_io_dylib_close dylib_observed_close
#endif
#include "../xir_runtime_allocations.h"
#if defined(XR_OS_WINDOWS)
#undef xr_os_io_dylib_symbol
#undef xr_os_io_dylib_close
#endif
#include "dylib_cases.h"
#if defined(XR_OS_WINDOWS)
static void dylib_trace(const char *phase){
    const char *enabled=getenv("XR_DYLIB_TRACE");if(enabled&&!strcmp(enabled,"1"))fprintf(stderr,"Dylib trace: %s\n",phase);
}
static XrXirInstanceResult dylib_drive(XrXirInstance *instance){
    for(unsigned n=0;n<1000000;++n){
        XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);
        if(event.outcome.status!=XR_XIR_CALL_READY)return event;
    }
    CHECK(false);return (XrXirInstanceResult){0};
}
static XrXirValue dylib_invoke(XrXirInstance *instance,uint32_t entry,const XrXirValue *args,uint32_t count){
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,args,count);
    if(status!=XR_XIR_CALL_READY)fprintf(stderr,"Dylib start entry%u status%u\n",entry,status);
    CHECK(status==XR_XIR_CALL_READY);XrXirInstanceResult event=dylib_drive(instance);
    if(event.outcome.status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"Dylib entry%u status%u\n",entry,event.outcome.status);
    CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static XrXirInstance *dylib_instance(XrXirProgram *program,const uint32_t *ids){
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue value=dylib_invoke(instance,ids[DYLIB_ENTRY],NULL,0);CHECK(value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);return instance;
}
static void dylib_retire(XrXirInstance *instance,XrXirCallStatus expected){
    uint64_t started=xr_time_monotonic_ns();CHECK(started);
    for(;;){
        XrXirCallStatus status=xr_xir_instance_free(instance);
        if(status!=XR_XIR_CALL_CLOSE_FAILED){CHECK(status==expected);return;}
        uint64_t now=xr_time_monotonic_ns();CHECK(now>=started&&now-started<UINT64_C(10000000000));xr_thread_yield();
    }
}
static const XrXirValue *dylib_some(const XrXirValue *optional,bool expected){
    bool present=false;const XrXirValue *value=NULL;CHECK(xr_xir_nullable_view(optional,&present,&value)&&present==expected);
    CHECK(expected?value!=NULL:value==NULL);return value;
}
static XrXirValue dylib_owned_some(XrXirValue *optional){
    XrXirValue value={0};CHECK(xr_xir_value_copy(dylib_some(optional,true),&value)==XR_XIR_VALUE_OK);xr_xir_value_drop(optional);return value;
}
static XrXirValue dylib_string(XrXirInstance *instance,const char *text){
    XrXirValue value={0};CHECK(xr_xir_string_new(instance->domain,text,strlen(text),&value)==XR_XIR_VALUE_OK);return value;
}
static XrXirValue dylib_path(XrXirInstance *instance,const uint32_t *ids,const char *text){
    XrXirValue raw=dylib_string(instance,text),path=dylib_invoke(instance,ids[DYLIB_PATH],&raw,1);xr_xir_value_drop(&raw);return path;
}
static XrXirValue dylib_open(XrXirInstance *instance,const uint32_t *ids,const char *text){
    XrXirValue path=dylib_path(instance,ids,text),optional=dylib_invoke(instance,ids[DYLIB_OPEN],&path,1);
    xr_xir_value_drop(&path);return dylib_owned_some(&optional);
}
static XrXirValue dylib_symbol(XrXirInstance *instance,const uint32_t *ids,const XrXirValue *library,const char *text,bool found){
    XrXirValue name=dylib_string(instance,text),args[]={*library,name},optional=dylib_invoke(instance,ids[DYLIB_SYMBOL],args,2),pointer={0};
    if(found)pointer=dylib_owned_some(&optional);else{dylib_some(&optional,false);xr_xir_value_drop(&optional);}
    xr_xir_value_drop(&name);return pointer;
}
static void dylib_bool(XrXirInstance *instance,uint32_t entry,const XrXirValue *argument,bool expected){
    XrXirValue value=dylib_invoke(instance,entry,argument,1);CHECK(value.type==XR_XIR_BOOL&&value.payload==expected);xr_xir_value_drop(&value);
}
static void dylib_stale(const XrXirValue *pointer){
    uint8_t byte=73;CHECK(xr_xir_pointer_load(pointer,0,&byte)==XR_XIR_VALUE_BAD_ARGUMENT&&byte==73);
}
static void dylib_equal_text(const XrXirValue *a,const XrXirValue *b,bool expected){
    bool equal=false;CHECK(xr_xir_string_equal(a,b,&equal)&&equal==expected);
}
static void dylib_nonempty(const XrXirValue *value,bool expected){
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(value,&bytes,&length)&&bytes&&(length!=0)==expected);
}
typedef struct DylibFixture {wchar_t directory[32768],file[32768];char *path;} DylibFixture;
static unsigned dylib_serial;
static bool dylib_observing;
static unsigned dylib_fail_close,dylib_close_faults,dylib_symbol_calls,dylib_fault_hits;
enum {DYLIB_FAULT_NONE,DYLIB_FAULT_OOM,DYLIB_FAULT_CANCEL};
static unsigned dylib_fault_mode;
static size_t dylib_fault_allocation;
static XrXirInstance *dylib_fault_instance;
static XrOsIoStatus dylib_observed_symbol(XrOsDylibOwner *owner,const XrOsDylibQuery *query,
    XrOsDylibText name,uintptr_t *address,bool *found){
    if(dylib_observing)++dylib_symbol_calls;
    XrOsIoStatus status=xr_os_io_dylib_symbol(owner,query,name,address,found);
    if(dylib_observing&&dylib_fault_mode&&status==XR_OS_IO_OK&&found&&*found){
        unsigned mode=dylib_fault_mode;dylib_fault_mode=DYLIB_FAULT_NONE;++dylib_fault_hits;
        if(mode==DYLIB_FAULT_OOM){dylib_fault_allocation=runtime_attempts;runtime_fail_at=runtime_attempts;}
        else CHECK(dylib_fault_instance&&xr_xir_call_request_cancel(dylib_fault_instance->call)==XR_XIR_CALL_CANCEL_REQUESTED);
    }
    return status;
}
static XrOsIoStatus dylib_observed_close(XrOsDylibOwner *owner,bool *closed){
    if(dylib_observing&&dylib_fail_close){--dylib_fail_close;++dylib_close_faults;return XR_OS_IO_IO;}
    return xr_os_io_dylib_close(owner,closed);
}
static char *dylib_utf8(const wchar_t *wide){
    int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,NULL,0,NULL,NULL);CHECK(length>0);
    char *text=xr_malloc((size_t)length);CHECK(text&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,text,length,NULL,NULL)==length);return text;
}
static void dylib_fixture_open(DylibFixture *fixture){
    wchar_t root[32768],source[32768];DWORD length=GetTempPathW(32768,root);CHECK(length&&length<32768);
    while(length>3&&(root[length-1]==L'\\'||root[length-1]==L'/'))root[--length]=0;
    CHECK(swprintf(fixture->directory,32768,L"%ls\\xray-dylib-%lu-%u-\u4e2d-\u03b2",root,(unsigned long)GetCurrentProcessId(),++dylib_serial)>0);
    CHECK(CreateDirectoryW(fixture->directory,NULL));
    CHECK(swprintf(fixture->file,32768,L"%ls\\data.dll",fixture->directory)>0);
    CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,XR_DYLIB_DATA,-1,source,32768)>0&&CopyFileW(source,fixture->file,TRUE));
    fixture->path=dylib_utf8(fixture->file);CHECK(!GetModuleHandleW(fixture->file));
}
static void dylib_fixture_close(DylibFixture *fixture){
    CHECK(!GetModuleHandleW(fixture->file)&&DeleteFileW(fixture->file)&&RemoveDirectoryW(fixture->directory));xr_free(fixture->path);
}
static void dylib_peek(XrXirInstance *instance,const uint32_t *ids,const XrXirValue *pointer,unsigned char expected){
    XrXirValue byte=dylib_invoke(instance,ids[DYLIB_PEEK],pointer,1);CHECK(byte.type==XR_XIR_I64&&byte.payload==expected);xr_xir_value_drop(&byte);
    uint8_t observed=73;CHECK(xr_xir_pointer_load(pointer,0,&observed)==XR_XIR_VALUE_OK&&observed==expected);
}
static void dylib_basic(XrXirProgram *program,const uint32_t *ids,const DylibFixture *fixture){
    XrXirInstance *instance=dylib_instance(program,ids);XrXirValue library=dylib_open(instance,ids,fixture->path);
    CHECK(GetModuleHandleW(fixture->file));XrXirValue token=dylib_invoke(instance,ids[DYLIB_TOKEN],&library,1);CHECK(token.type==XR_XIR_I64&&token.payload>0);
    XrXirValue pointer=dylib_symbol(instance,ids,&library,"xray_dylib_bytes",true);
    dylib_peek(instance,ids,&pointer,dylib_expected[0]);
    XrXirValue offset=dylib_invoke(instance,ids[DYLIB_OFFSET],&pointer,1),readonly={0};
    dylib_peek(instance,ids,&offset,dylib_expected[1]);XrXirValue last={0};CHECK(xr_xir_pointer_offset(&pointer,2,&last)==XR_XIR_VALUE_OK);
    dylib_peek(instance,ids,&last,dylib_expected[2]);
    CHECK(xr_xir_pointer_readonly(&pointer,xr_xir_value_arena(&pointer),(XrXirType)pointer.type,&readonly)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_pointer_store(&readonly,0,1)==XR_XIR_VALUE_BAD_ARGUMENT);dylib_peek(instance,ids,&readonly,dylib_expected[0]);
    XrXirValue missing=dylib_symbol(instance,ids,&library,"xray_symbol_absent",false);CHECK(!missing.type);
    XrXirValue error=dylib_invoke(instance,ids[DYLIB_ERROR],NULL,0);dylib_nonempty(&error,true);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&error,&bytes,&length)&&length<SIZE_MAX);
    char *saved=xr_malloc(length+1);CHECK(saved);memcpy(saved,bytes,length);saved[length]=0;
    dylib_bool(instance,ids[DYLIB_CLOSE],&library,true);dylib_bool(instance,ids[DYLIB_CLOSE],&library,true);
    dylib_bool(instance,ids[DYLIB_RAW_CLOSE],&token,false);CHECK(!GetModuleHandleW(fixture->file));
    dylib_stale(&pointer);dylib_stale(&readonly);dylib_stale(&offset);dylib_stale(&last);
    xr_xir_value_drop(&token);xr_xir_value_drop(&library);dylib_retire(instance,XR_XIR_CALL_READY);
    const char *retained=NULL;size_t retained_length=0;CHECK(xr_xir_string_view(&error,&retained,&retained_length)&&retained_length==length&&!memcmp(saved,retained,length));
    xr_free(saved);xr_xir_value_drop(&error);xr_xir_value_drop(&pointer);xr_xir_value_drop(&readonly);xr_xir_value_drop(&offset);xr_xir_value_drop(&last);
}
static void dylib_refusals(XrXirProgram *program,const uint32_t *ids,const DylibFixture *fixture){
    XrXirInstance *a=dylib_instance(program,ids),*b=dylib_instance(program,ids);
    XrXirValue first=dylib_open(a,ids,fixture->path),second=dylib_open(b,ids,fixture->path);
    XrXirValue token=dylib_invoke(a,ids[DYLIB_TOKEN],&first,1),other=dylib_invoke(b,ids[DYLIB_TOKEN],&second,1);
    CHECK(token.type==XR_XIR_I64&&other.type==XR_XIR_I64&&token.payload>0&&other.payload>0&&token.payload!=other.payload);
    XrXirValue pointer=dylib_symbol(a,ids,&first,"xray_dylib_bytes",true),live=dylib_symbol(b,ids,&second,"xray_dylib_bytes",true);
    XrXirValue readonly={0},offset=dylib_invoke(a,ids[DYLIB_OFFSET],&pointer,1);
    CHECK(xr_xir_pointer_readonly(&pointer,xr_xir_value_arena(&pointer),(XrXirType)pointer.type,&readonly)==XR_XIR_VALUE_OK);
    dylib_peek(a,ids,&readonly,dylib_expected[0]);dylib_peek(a,ids,&offset,dylib_expected[1]);
    XrXirValue rejected[]={token,{XR_XIR_I64,0,0},{XR_XIR_I64,0,INT64_MAX}},name=dylib_string(b,"xray_dylib_bytes");
    for(unsigned n=0;n<3;++n){
        dylib_bool(b,ids[DYLIB_RAW_CLOSE],rejected+n,false);XrXirValue args[]={rejected[n],name};
        XrXirValue value=dylib_invoke(b,ids[DYLIB_RAW_SYMBOL],args,2);dylib_some(&value,false);xr_xir_value_drop(&value);
    }
    xr_xir_value_drop(&name);xr_xir_value_drop(&first);dylib_retire(a,XR_XIR_CALL_READY);
    /* The second loader owner keeps the image mapped, but cannot resurrect
     * the first Root's stale pointer origin or accept its token. */
    CHECK(GetModuleHandleW(fixture->file));dylib_stale(&pointer);dylib_stale(&readonly);dylib_stale(&offset);dylib_peek(b,ids,&live,dylib_expected[0]);
    dylib_bool(b,ids[DYLIB_RAW_CLOSE],&token,false);dylib_bool(b,ids[DYLIB_CLOSE],&second,true);dylib_stale(&live);
    xr_xir_value_drop(&second);xr_xir_value_drop(&other);dylib_retire(b,XR_XIR_CALL_READY);CHECK(!GetModuleHandleW(fixture->file));
    XrXirInstance *fresh=dylib_instance(program,ids);XrXirValue next=dylib_open(fresh,ids,fixture->path),next_token=dylib_invoke(fresh,ids[DYLIB_TOKEN],&next,1);
    CHECK(next_token.type==XR_XIR_I64&&next_token.payload>0&&next_token.payload!=token.payload);dylib_stale(&pointer);
    dylib_bool(fresh,ids[DYLIB_RAW_CLOSE],&token,false);xr_xir_value_drop(&next_token);xr_xir_value_drop(&next);
    dylib_retire(fresh,XR_XIR_CALL_READY);xr_xir_value_drop(&token);xr_xir_value_drop(&pointer);xr_xir_value_drop(&live);
    xr_xir_value_drop(&readonly);xr_xir_value_drop(&offset);
}
static void dylib_defer(XrXirProgram *program,const uint32_t *ids,const DylibFixture *fixture){
    XrXirInstance *instance=dylib_instance(program,ids);XrXirValue path=dylib_path(instance,ids,fixture->path);
    XrXirValue optional=dylib_invoke(instance,ids[DYLIB_DEFER],&path,1),pointer=dylib_owned_some(&optional);
    CHECK(!GetModuleHandleW(fixture->file));dylib_stale(&pointer);xr_xir_value_drop(&path);dylib_retire(instance,XR_XIR_CALL_READY);xr_xir_value_drop(&pointer);
}
typedef struct DylibThread {XrXirProgram *program;const uint32_t *ids;const DylibFixture *fixture;XrXirValue error;} DylibThread;
static void *dylib_thread_error(void *context){
    DylibThread *request=context;XrXirInstance *instance=dylib_instance(request->program,request->ids);
    XrXirValue initial=dylib_invoke(instance,request->ids[DYLIB_ERROR],NULL,0);dylib_nonempty(&initial,false);xr_xir_value_drop(&initial);
    XrXirValue library=dylib_open(instance,request->ids,request->fixture->path);
    XrXirValue absent=dylib_symbol(instance,request->ids,&library,"xray_symbol_absent",false);CHECK(!absent.type);
    request->error=dylib_invoke(instance,request->ids[DYLIB_ERROR],NULL,0);dylib_nonempty(&request->error,true);
    dylib_bool(instance,request->ids[DYLIB_CLOSE],&library,true);xr_xir_value_drop(&library);dylib_retire(instance,XR_XIR_CALL_READY);return NULL;
}
static void dylib_thread_errors(XrXirProgram *program,const uint32_t *ids,const DylibFixture *fixture){
    XrXirInstance *a=dylib_instance(program,ids),*b=dylib_instance(program,ids);
    size_t length=strlen(fixture->path);CHECK(length<SIZE_MAX-8);char *absent=xr_malloc(length+8);CHECK(absent);memcpy(absent,fixture->path,length);memcpy(absent+length,".absent",8);
    XrXirValue path=dylib_path(a,ids,absent),missing=dylib_invoke(a,ids[DYLIB_OPEN],&path,1);dylib_some(&missing,false);
    xr_free(absent);xr_xir_value_drop(&path);xr_xir_value_drop(&missing);
    XrXirValue first=dylib_invoke(a,ids[DYLIB_ERROR],NULL,0),same=dylib_invoke(b,ids[DYLIB_ERROR],NULL,0);
    dylib_nonempty(&first,true);dylib_equal_text(&first,&same,true);xr_xir_value_drop(&same);
    DylibThread request={program,ids,fixture,{0}};xr_thread_t thread;CHECK(xr_thread_create(&thread,dylib_thread_error,&request));
    void *returned=(void *)(uintptr_t)1;CHECK(xr_thread_join(thread,&returned)==0&&!returned);
    XrXirValue after=dylib_invoke(b,ids[DYLIB_ERROR],NULL,0);dylib_equal_text(&first,&after,true);dylib_equal_text(&first,&request.error,false);
    dylib_retire(a,XR_XIR_CALL_READY);dylib_retire(b,XR_XIR_CALL_READY);
    dylib_nonempty(&request.error,true);dylib_equal_text(&first,&after,true);
    xr_xir_value_drop(&first);xr_xir_value_drop(&after);xr_xir_value_drop(&request.error);CHECK(!GetModuleHandleW(fixture->file));
}
static void dylib_private_host(XrXirProgram *program,const uint32_t *ids,const DylibFixture *fixture){
    XrXirInstance *instance=dylib_instance(program,ids);XrXirValue library=dylib_open(instance,ids,fixture->path);
    XrXirValue path=dylib_path(instance,ids,fixture->path),token=dylib_invoke(instance,ids[DYLIB_TOKEN],&library,1),name=dylib_string(instance,"xray_dylib_bytes");
    for(unsigned n=0;n<4;++n){
        uint32_t f=UINT32_MAX;for(uint32_t p=0;p<program->entry_count;++p)if(program->declarations->functions[p].native_callable==104+n)f=p;
        CHECK(f!=UINT32_MAX);XrXirValue args[]={n?token:path,name};
        CHECK(xr_xir_instance_start(instance,f,n==3?NULL:args,n==3?0u:n==1?2u:1u)==XR_XIR_CALL_BAD_ARGUMENT);
    }
    dylib_bool(instance,ids[DYLIB_CLOSE],&library,true);xr_xir_value_drop(&library);xr_xir_value_drop(&path);xr_xir_value_drop(&token);xr_xir_value_drop(&name);dylib_retire(instance,XR_XIR_CALL_READY);
}
static void dylib_close_failure(XrXirProgram *program,const uint32_t *ids,const DylibFixture *fixture){
    XrXirInstance *instance=dylib_instance(program,ids);XrXirValue library=dylib_open(instance,ids,fixture->path);
    XrXirValue pointer=dylib_symbol(instance,ids,&library,"xray_dylib_bytes",true);dylib_observing=true;dylib_fail_close=1;dylib_close_faults=0;
    CHECK(xr_xir_instance_start(instance,ids[DYLIB_CLOSE],&library,1)==XR_XIR_CALL_READY);
    CHECK(dylib_drive(instance).outcome.status==XR_XIR_CALL_HOST_ERROR);XrXirValue output={0};
    CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_HOST_ERROR&&!output.type&&!output.reserved&&!output.payload);
    CHECK(dylib_close_faults==1&&!dylib_fail_close&&GetModuleHandleW(fixture->file));dylib_stale(&pointer);
    xr_xir_value_drop(&library);dylib_fail_close=1;CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_CLOSE_FAILED);
    CHECK(dylib_close_faults==2&&!dylib_fail_close&&GetModuleHandleW(fixture->file));
    dylib_retire(instance,XR_XIR_CALL_HOST_ERROR);dylib_observing=false;CHECK(!GetModuleHandleW(fixture->file));xr_xir_value_drop(&pointer);
}
static void dylib_materialization_fault(XrXirProgram *program,const uint32_t *ids,const DylibFixture *fixture,bool cancel){
    size_t live=runtime_live,bytes=runtime_bytes;CHECK(runtime_fail_at==SIZE_MAX&&!dylib_observing&&!dylib_fault_mode);
    XrXirInstance *instance=dylib_instance(program,ids);XrXirValue library=dylib_open(instance,ids,fixture->path),name=dylib_string(instance,"xray_dylib_bytes"),args[]={library,name};
    dylib_observing=true;dylib_fault_mode=cancel?DYLIB_FAULT_CANCEL:DYLIB_FAULT_OOM;dylib_fault_instance=instance;dylib_symbol_calls=dylib_fault_hits=0;
    CHECK(xr_xir_instance_start(instance,ids[DYLIB_SYMBOL],args,2)==XR_XIR_CALL_READY);
    XrXirCallStatus terminal=dylib_drive(instance).outcome.status;runtime_fail_at=SIZE_MAX;
    XrXirCallStatus expected=cancel?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_OOM;
    if(terminal!=expected)fprintf(stderr,"Dylib symbol fault cancel%u status%u\n",(unsigned)cancel,terminal);
    CHECK(terminal==expected&&dylib_fault_hits==1&&!dylib_fault_mode);
    if(!cancel)CHECK(runtime_attempts>dylib_fault_allocation);
    XrXirValue output={0};CHECK(xr_xir_instance_take_result(instance,&output)==(cancel?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_OOM)&&!output.type&&!output.reserved&&!output.payload);
    xr_xir_value_drop(&library);xr_xir_value_drop(&name);dylib_retire(instance,cancel?XR_XIR_CALL_READY:XR_XIR_CALL_OOM);
    CHECK(dylib_symbol_calls==1);dylib_observing=false;dylib_fault_instance=NULL;
    CHECK(!GetModuleHandleW(fixture->file)&&runtime_live==live&&runtime_bytes==bytes);
}
static void dylib_execute(XrXirProgram *program,const uint32_t *ids){
    size_t live=runtime_live,bytes=runtime_bytes;DylibFixture fixture;dylib_fixture_open(&fixture);
    dylib_trace("basic enter");dylib_basic(program,ids,&fixture);dylib_trace("basic exit");
    dylib_trace("refusals enter");dylib_refusals(program,ids,&fixture);dylib_trace("refusals exit");
    dylib_trace("defer enter");dylib_defer(program,ids,&fixture);dylib_trace("defer exit");
    dylib_trace("thread_errors enter");dylib_thread_errors(program,ids,&fixture);dylib_trace("thread_errors exit");
    dylib_trace("privateHost enter");dylib_private_host(program,ids,&fixture);dylib_trace("privateHost exit");
    dylib_trace("close_failure enter");dylib_close_failure(program,ids,&fixture);dylib_trace("close_failure exit");
    dylib_trace("symbol_oom enter");dylib_materialization_fault(program,ids,&fixture,false);dylib_trace("symbol_oom exit");
    dylib_trace("symbol_cancel enter");dylib_materialization_fault(program,ids,&fixture,true);dylib_trace("symbol_cancel exit");
    dylib_fixture_close(&fixture);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#else
static void dylib_execute(XrXirProgram *program,const uint32_t *ids){
    (void)program;(void)ids;fprintf(stderr,"Dylib runtime qualification is not implemented on this platform\n");exit(2);
}
#endif
#endif // DYLIB_RUNTIME_H
