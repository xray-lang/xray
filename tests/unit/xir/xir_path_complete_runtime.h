/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_path_complete_runtime.h - Complete Path algorithms with actual cwd reads
 *
 * KEY CONCEPT:
 *   Absolute paths never query the host. Relative paths observe the actual
 *   current directory on each call, with owned failure cleanup and no cache.
 */
#ifndef XIR_PATH_COMPLETE_RUNTIME_H
#define XIR_PATH_COMPLETE_RUNTIME_H
#include "xir/xxir_ctfe.h"
#include "xir/xxir_native_callable.h"
#include "os/xcwd_owned.h"
#include "os/os_time.h"
#include "os/os_thread.h"
#include <wchar.h>
#if !defined(XR_OS_WINDOWS)
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(XR_OS_WINDOWS)
#ifdef interface
#undef interface
#endif
#endif
static XrOsIoStatus path_complete_observe_cwd(const XrOsIoPolicy *,char **);
#define xr_os_io_getcwd_owned path_complete_observe_cwd
#include "xir_runtime_allocations.h"
#undef xr_os_io_getcwd_owned
enum {PATH_COMPLETE_ENTRY,PATH_COMPLETE_RESOLVE,PATH_COMPLETE_PAIR,PATH_COMPLETE_NORMALIZE,
    PATH_COMPLETE_ALGORITHMS,PATH_COMPLETE_CWD,PATH_COMPLETE_COUNT};
enum {PATH_COMPLETE_OS_ENTRY,PATH_COMPLETE_OS_CWD,PATH_COMPLETE_OS_COUNT};
static unsigned path_complete_queries,path_complete_mode,path_complete_hits;
static size_t path_complete_failed_allocation;
static XrXirInstance *path_complete_active;
#if defined(XR_OS_WINDOWS)
static const wchar_t *path_complete_fixture_cwd;
static unsigned path_complete_fixture_hits;
#endif
enum {PATH_COMPLETE_NORMAL,PATH_COMPLETE_OOM,PATH_COMPLETE_CANCEL};
static XrOsIoStatus path_complete_observe_cwd(const XrOsIoPolicy *policy,char **output){
    ++path_complete_queries;
    if(path_complete_mode==PATH_COMPLETE_OOM){
        path_complete_mode=PATH_COMPLETE_NORMAL;++path_complete_hits;
        path_complete_failed_allocation=runtime_attempts;runtime_fail_at=runtime_attempts;
    }
    XrOsIoStatus status=xr_os_io_getcwd_owned(policy,output);
#if defined(XR_OS_WINDOWS)
    if(status==XR_OS_IO_OK&&path_complete_fixture_cwd){
        int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path_complete_fixture_cwd,-1,NULL,0,NULL,NULL);CHECK(bytes>0);
        void *replacement=NULL;status=policy->alloc(policy->context,(size_t)bytes,&replacement);
        if(status==XR_OS_IO_OK)CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path_complete_fixture_cwd,-1,replacement,bytes,NULL,NULL)==bytes);
        policy->free(policy->context,*output);*output=status==XR_OS_IO_OK?replacement:NULL;
        if(status!=XR_OS_IO_OK)policy->free(policy->context,replacement);else ++path_complete_fixture_hits;
    }
#endif
    if(path_complete_mode==PATH_COMPLETE_CANCEL&&status==XR_OS_IO_OK){
        path_complete_mode=PATH_COMPLETE_NORMAL;++path_complete_hits;
        CHECK(path_complete_active&&xr_xir_call_request_cancel(path_complete_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
    }
    return status;
}
static XrXirCallStatus path_complete_poll(XrXirInstance *instance){
    for(unsigned n=0;n<1000000;++n){
        XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,64).outcome.status;
        if(status!=XR_XIR_CALL_READY)return status;
    }
    CHECK(false);return XR_XIR_CALL_BAD_STATE;
}
static XrXirValue path_complete_invoke(XrXirInstance *instance,uint32_t entry,const XrXirValue *args,uint32_t count){
    CHECK(xr_xir_instance_start(instance,entry,args,count)==XR_XIR_CALL_READY);
    XrXirCallStatus status=path_complete_poll(instance);
    if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"complete Path entry%u status%u\n",entry,status);
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static XrXirInstance *path_complete_instance(XrXirProgram *program,const uint32_t *ids){
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue entry=path_complete_invoke(instance,ids[PATH_COMPLETE_ENTRY],NULL,0);
    CHECK(entry.type==XR_XIR_I64&&!entry.payload);xr_xir_value_drop(&entry);return instance;
}
static XrXirValue path_complete_string(XrXirInstance *instance,const char *text){
    XrXirValue value={0};CHECK(xr_xir_string_new(instance->domain,text,strlen(text),&value)==XR_XIR_VALUE_OK);return value;
}
static void path_complete_bytes(const XrXirValue *value,const char *expected){
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length)&&length==strlen(expected)&&!memcmp(bytes,expected,length));
}
static void path_complete_pure(XrXirInstance *instance,const uint32_t *ids){
    unsigned before=path_complete_queries;
    XrXirValue argument=path_complete_string(instance,"/a//b/../c/./"),result=path_complete_invoke(instance,ids[PATH_COMPLETE_NORMALIZE],&argument,1);
    path_complete_bytes(&result,"/a/c");xr_xir_value_drop(&argument);xr_xir_value_drop(&result);
    argument=path_complete_string(instance,"/a/../absolute");result=path_complete_invoke(instance,ids[PATH_COMPLETE_RESOLVE],&argument,1);
    path_complete_bytes(&result,"/absolute");xr_xir_value_drop(&argument);xr_xir_value_drop(&result);
    XrXirValue args[]={path_complete_string(instance,"ignored"),path_complete_string(instance,"/a/../right")};
    result=path_complete_invoke(instance,ids[PATH_COMPLETE_PAIR],args,2);path_complete_bytes(&result,"/right");
    xr_xir_value_drop(&args[0]);xr_xir_value_drop(&args[1]);xr_xir_value_drop(&result);
    result=path_complete_invoke(instance,ids[PATH_COMPLETE_ALGORITHMS],NULL,0);CHECK(result.type==XR_XIR_I64&&result.payload==42);
    xr_xir_value_drop(&result);CHECK(path_complete_queries==before);
}
static unsigned path_complete_serial;
#if defined(XR_OS_WINDOWS)
typedef struct PathCompleteDirectories {wchar_t old[32768],first[32768],second[32768];} PathCompleteDirectories;
static char *path_complete_windows_text(const wchar_t *wide){
    int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,NULL,0,NULL,NULL);CHECK(bytes>0);
    char *text=xr_malloc((size_t)bytes);CHECK(text&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,text,bytes,NULL,NULL)==bytes);return text;
}
static char *path_complete_actual_cwd(void){
    wchar_t wide[32768];DWORD length=GetCurrentDirectoryW(32768,wide);CHECK(length&&length<32768);
    return path_complete_windows_text(wide);
}
static char *path_complete_expected_anchor(void){
    wchar_t wide[32768];DWORD length=GetCurrentDirectoryW(32768,wide);CHECK(length&&length<32768);
    /* This oracle works in OS WCHAR coordinates, independently of the native
     * boundary's UTF-8 scan. The actual temporary directories are Win32 paths. */
    wchar_t *anchor=wide;
    if(length>=3&&wide[1]==L':'){CHECK(wide[2]==L'\\');anchor+=2;}
    else CHECK(length>=5&&wide[0]==L'\\'&&wide[1]==L'\\');
    for(wchar_t *p=anchor;*p;++p)if(*p==L'\\')*p=L'/';
    return path_complete_windows_text(anchor);
}
static void path_complete_directories_open(PathCompleteDirectories *directories){
    wchar_t temp[32768];DWORD old_length=GetCurrentDirectoryW(32768,directories->old),length=GetTempPathW(32768,temp);
    CHECK(old_length&&old_length<32768&&length&&length<32768);
    while(length>3&&(temp[length-1]==L'\\'||temp[length-1]==L'/'))temp[--length]=0;
    unsigned serial=++path_complete_serial;
    CHECK(swprintf(directories->first,32768,L"%ls\\xray-complete-path-%lu-%u-\u4e2d",temp,(unsigned long)GetCurrentProcessId(),serial)>0);
    CHECK(swprintf(directories->second,32768,L"%ls\\xray-complete-path-%lu-%u-\u03b2",temp,(unsigned long)GetCurrentProcessId(),serial)>0);
    CHECK(CreateDirectoryW(directories->first,NULL)&&CreateDirectoryW(directories->second,NULL));
}
static void path_complete_change(const PathCompleteDirectories *directories,bool second){
    CHECK(SetCurrentDirectoryW(second?directories->second:directories->first));
}
static void path_complete_directories_close(PathCompleteDirectories *directories){
    CHECK(SetCurrentDirectoryW(directories->old)&&RemoveDirectoryW(directories->first)&&RemoveDirectoryW(directories->second));
}
#else
typedef struct PathCompleteDirectories {char *old,*first,*second;} PathCompleteDirectories;
static char *path_complete_actual_cwd(void){
    size_t capacity=256;
    for(;;){
        char *text=xr_malloc(capacity);CHECK(text);
        if(getcwd(text,capacity))return text;
        int error=errno;xr_free(text);CHECK(error==ERANGE&&capacity<=SIZE_MAX/2);capacity*=2;
    }
}
static char *path_complete_expected_anchor(void){return path_complete_actual_cwd();}
static void path_complete_directories_open(PathCompleteDirectories *directories){
    directories->old=path_complete_actual_cwd();const char *temporary=getenv("TMPDIR");
    if(!temporary||!*temporary)temporary="/tmp";
    size_t old_length=strlen(directories->old),length=strlen(temporary);CHECK(old_length<=SIZE_MAX-130&&length<=SIZE_MAX-old_length-130);
    size_t capacity=old_length+length+130;char *root=xr_malloc(capacity);CHECK(root);int count;
    if(*temporary=='/')count=snprintf(root,capacity,"%s",temporary);
    else count=snprintf(root,capacity,"%s/%s",directories->old,temporary);
    CHECK(count>0&&(size_t)count<capacity);length=(size_t)count;while(length>1&&root[length-1]=='/')root[--length]=0;
    directories->first=xr_malloc(capacity);directories->second=xr_malloc(capacity);CHECK(directories->first&&directories->second);
    unsigned serial=++path_complete_serial;const char *separator=length==1?"":"/";
    count=snprintf(directories->first,capacity,"%s%sxray-complete-path-%lu-%u-\xe4\xb8\xad",root,separator,(unsigned long)getpid(),serial);
    CHECK(count>0&&(size_t)count<capacity);
    count=snprintf(directories->second,capacity,"%s%sxray-complete-path-%lu-%u-\xce\xb2",root,separator,(unsigned long)getpid(),serial);
    CHECK(count>0&&(size_t)count<capacity);xr_free(root);
    CHECK(mkdir(directories->first,0700)==0&&mkdir(directories->second,0700)==0);
}
static void path_complete_change(const PathCompleteDirectories *directories,bool second){
    CHECK(chdir(second?directories->second:directories->first)==0);
}
static void path_complete_directories_close(PathCompleteDirectories *directories){
    CHECK(chdir(directories->old)==0&&rmdir(directories->first)==0&&rmdir(directories->second)==0);
    xr_free(directories->old);xr_free(directories->first);xr_free(directories->second);
}
#endif
static void path_complete_relative(XrXirInstance *instance,const uint32_t *ids){
    char *cwd=path_complete_expected_anchor();size_t length=strlen(cwd);CHECK(length<=SIZE_MAX-16);
    const char *base=cwd+(length>1&&cwd[0]=='/'&&cwd[1]=='/');size_t base_length=strlen(base);
    const char *inputs[]={"child/../leaf","child\\leaf","C:/x"},*tails[]={"/leaf","/child\\leaf","/C:/x"};
    for(unsigned n=0;n<3;++n){
        const char *tail=tails[n]+(base_length==1&&base[0]=='/');size_t extra=strlen(tail);
        char *expected=xr_malloc(base_length+extra+1);CHECK(expected);memcpy(expected,base,base_length);memcpy(expected+base_length,tail,extra+1);
        unsigned before=path_complete_queries;XrXirValue argument=path_complete_string(instance,inputs[n]);
        XrXirValue value=path_complete_invoke(instance,ids[PATH_COMPLETE_RESOLVE],&argument,1);
        path_complete_bytes(&value,expected);CHECK(path_complete_queries==before+1);xr_xir_value_drop(&value);xr_xir_value_drop(&argument);xr_free(expected);
    }
    unsigned before=path_complete_queries;XrXirValue value=path_complete_invoke(instance,ids[PATH_COMPLETE_CWD],NULL,0);
    path_complete_bytes(&value,cwd);CHECK(path_complete_queries==before+1);xr_xir_value_drop(&value);xr_free(cwd);
}
static void path_complete_raw_os(XrXirProgram *program,const uint32_t *ids,const char *expected){
    XrXirInstance *instance=path_complete_instance(program,ids);unsigned before=path_complete_queries;
    XrXirValue value=path_complete_invoke(instance,ids[PATH_COMPLETE_OS_CWD],NULL,0);
    CHECK(path_complete_queries==before+1&&xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    path_complete_bytes(&value,expected);xr_xir_value_drop(&value);
}
#if defined(XR_OS_WINDOWS)
static void path_complete_provider_cases(XrXirProgram *program,const uint32_t *ids,
    XrXirProgram *raw_program,const uint32_t *raw_ids){
    static const struct {const wchar_t *raw;const char *anchor,*resolved;} valid[]={
        {L"C:\\","/","/leaf"},
        {L"Z:\\\u4e2d\\\u03b2","/\xe4\xb8\xad/\xce\xb2","/\xe4\xb8\xad/\xce\xb2/leaf"},
        {L"\\\\srv-\u4e2d\\share-\u03b2\\dir","//srv-\xe4\xb8\xad/share-\xce\xb2/dir","/srv-\xe4\xb8\xad/share-\xce\xb2/dir/leaf"},
        {L"\\\\server.example.com\\C$","//server.example.com/C$","/server.example.com/C$/leaf"},
        {L"\\\\192.0.2.1\\share","//192.0.2.1/share","/192.0.2.1/share/leaf"},
        {L"\\\\host-name-longer-than-fifteen\\share","//host-name-longer-than-fifteen/share","/host-name-longer-than-fifteen/share/leaf"},
        {L"\\\\server\\share-longer-than-eighty-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456789-abcdefghijklmnopqrstuvwxyz",
            "//server/share-longer-than-eighty-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456789-abcdefghijklmnopqrstuvwxyz",
            "/server/share-longer-than-eighty-abcdefghijklmnopqrstuvwxyz-ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456789-abcdefghijklmnopqrstuvwxyz/leaf"}
    };
    static const wchar_t *const denied[]={L"\\\\?\\C:\\x",L"\\\\.\\PIPE\\x",L"\\\\C:\\share",L"\\\\srv\\C:",
        L"\\\\..\\share",L"\\\\??\\C:\\x",L"\\\\server",L"\\\\server\\",L"\\\\srv\\\\share",L"C:relative",L"relative\\path",L"\\\\srv\\bad\tname"};
    /* These are synthetic provider facts after an actual owned query. They
     * qualify defensive projection, never actual SMB access or host discovery. */
    CHECK(!path_complete_fixture_cwd&&path_complete_mode==PATH_COMPLETE_NORMAL);
    for(unsigned n=0;n<sizeof(valid)/sizeof(*valid);++n){
        size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=path_complete_instance(program,ids);
        path_complete_fixture_cwd=valid[n].raw;path_complete_fixture_hits=0;unsigned before=path_complete_queries;
        XrXirValue value=path_complete_invoke(instance,ids[PATH_COMPLETE_CWD],NULL,0);path_complete_bytes(&value,valid[n].anchor);xr_xir_value_drop(&value);
        XrXirValue argument=path_complete_string(instance,"leaf");value=path_complete_invoke(instance,ids[PATH_COMPLETE_RESOLVE],&argument,1);
        path_complete_bytes(&value,valid[n].resolved);xr_xir_value_drop(&value);xr_xir_value_drop(&argument);
        CHECK(path_complete_queries==before+2&&path_complete_fixture_hits==2&&xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        char *raw=path_complete_windows_text(valid[n].raw);path_complete_raw_os(raw_program,raw_ids,raw);xr_free(raw);
        CHECK(path_complete_fixture_hits==3);path_complete_fixture_cwd=NULL;CHECK(runtime_live==live&&runtime_bytes==bytes);
    }
    for(unsigned n=0;n<sizeof(denied)/sizeof(*denied);++n){
        size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=path_complete_instance(program,ids);
        path_complete_fixture_cwd=denied[n];path_complete_fixture_hits=0;unsigned before=path_complete_queries;
        CHECK(xr_xir_instance_start(instance,ids[PATH_COMPLETE_CWD],NULL,0)==XR_XIR_CALL_READY);
        CHECK(path_complete_poll(instance)==XR_XIR_CALL_HOST_ERROR);XrXirValue value={0};
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_HOST_ERROR&&!value.type&&!value.reserved&&!value.payload);
        CHECK(path_complete_queries==before+1&&path_complete_fixture_hits==1&&xr_xir_instance_free(instance)==XR_XIR_CALL_HOST_ERROR);
        char *raw=path_complete_windows_text(denied[n]);path_complete_raw_os(raw_program,raw_ids,raw);xr_free(raw);
        CHECK(path_complete_fixture_hits==2);path_complete_fixture_cwd=NULL;CHECK(runtime_live==live&&runtime_bytes==bytes);
    }
}
#endif
static void path_complete_fault(XrXirProgram *program,const uint32_t *ids,bool cancel){
    size_t live=runtime_live,bytes=runtime_bytes;CHECK(runtime_fail_at==SIZE_MAX&&path_complete_mode==PATH_COMPLETE_NORMAL);
    XrXirInstance *instance=path_complete_instance(program,ids);XrXirValue argument=path_complete_string(instance,"relative");
    unsigned before=path_complete_queries;path_complete_hits=0;path_complete_active=instance;path_complete_mode=cancel?PATH_COMPLETE_CANCEL:PATH_COMPLETE_OOM;
    CHECK(xr_xir_instance_start(instance,ids[PATH_COMPLETE_RESOLVE],&argument,1)==XR_XIR_CALL_READY);
    XrXirCallStatus status=path_complete_poll(instance);runtime_fail_at=SIZE_MAX;
    if(status!=(cancel?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_OOM))fprintf(stderr,"complete Path fault cancel%u status%u\n",(unsigned)cancel,status);
    CHECK(status==(cancel?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_OOM)&&path_complete_queries==before+1&&path_complete_hits==1&&path_complete_mode==PATH_COMPLETE_NORMAL);
    if(!cancel)CHECK(runtime_attempts>path_complete_failed_allocation);
    XrXirValue output={0};CHECK(xr_xir_instance_take_result(instance,&output)==(cancel?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_OOM)&&!output.type&&!output.reserved&&!output.payload);
    xr_xir_value_drop(&argument);CHECK(xr_xir_instance_free(instance)==(cancel?XR_XIR_CALL_READY:XR_XIR_CALL_OOM));path_complete_active=NULL;
    CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void path_complete_execute(XrXirProgram *program,const uint32_t *ids,XrXirProgram *raw_program,const uint32_t *raw_ids){
    size_t live=runtime_live,bytes=runtime_bytes;PathCompleteDirectories directories;path_complete_directories_open(&directories);
    XrXirInstance *instance=path_complete_instance(program,ids);path_complete_pure(instance,ids);
    path_complete_change(&directories,false);path_complete_relative(instance,ids);
    char *raw=path_complete_actual_cwd();path_complete_raw_os(raw_program,raw_ids,raw);xr_free(raw);
    path_complete_change(&directories,true);path_complete_relative(instance,ids);
    raw=path_complete_actual_cwd();path_complete_raw_os(raw_program,raw_ids,raw);xr_free(raw);
    uint32_t native=UINT32_MAX;for(uint32_t f=0;f<program->entry_count;++f)if(program->declarations->functions[f].native_callable==108)native=f;
    CHECK(native!=UINT32_MAX&&xr_xir_instance_start(instance,native,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
    XrXirValue retained=path_complete_invoke(instance,ids[PATH_COMPLETE_CWD],NULL,0);char *expected=path_complete_expected_anchor();
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);path_complete_bytes(&retained,expected);xr_free(expected);xr_xir_value_drop(&retained);
    path_complete_fault(program,ids,false);path_complete_fault(program,ids,true);
#if defined(XR_OS_WINDOWS)
    path_complete_provider_cases(program,ids,raw_program,raw_ids);
#endif
    path_complete_directories_close(&directories);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#endif // XIR_PATH_COMPLETE_RUNTIME_H
