/* Actual command parsing and the production formatter command. */
#include "app/cli/xcli.h"
#include "base/xwindows_utf8.h"

#if FMT_INJECTED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"fmt fault invariant %d\n",__LINE__);exit(90);}}while(0)
#define xr_compile_resources_new fmt_real_new
#define xr_compile_resources_release fmt_real_release
#include "../build_source_owner/publication_faults.inc.h"
#undef xr_compile_resources_new
#undef xr_compile_resources_release
static XrCompileResourceStats recorded;
static unsigned ledger_count;
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(const XrCompileResourceLimits *limits,XrCompileResources **out) {
    CHECK(limits->allocated_bytes==UINT64_C(1073741824) && limits->live_bytes==UINT64_C(268435456) && limits->work==UINT64_C(8589934592));
    ++ledger_count; XrCompileResourceLimits bound=*limits; const char *value;
    if((value=getenv("XR_FMT_TEST_WORK")))bound.work=strtoull(value,NULL,10);
    if((value=getenv("XR_FMT_TEST_ALLOCATED")))bound.allocated_bytes=strtoull(value,NULL,10);
    if((value=getenv("XR_FMT_TEST_LIVE")))bound.live_bytes=strtoull(value,NULL,10);
    return fmt_real_new(&bound,out);
}
XR_FUNC void xr_compile_resources_release(XrCompileResources *owner) {
    if(owner && owner->stats.allocated_bytes>=recorded.allocated_bytes)recorded=owner->stats;
    fmt_real_release(owner);
}
extern bool fmt_dir_open_failure,fmt_dir_next_failure;
static DWORD winapi_control(void) {
    DWORD cold=0,volume=0,random=0,again=0,last=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&cold));
    HANDLE dir=CreateFileW(L".",FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);CHECK(dir!=INVALID_HANDLE_VALUE);
    wchar_t filesystem[16];CHECK(GetVolumeInformationByHandleW(dir,NULL,0,NULL,NULL,NULL,filesystem,16));CHECK(CloseHandle(dir));
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&volume));
    UCHAR nonce[16];CHECK(BCryptGenRandom(NULL,nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&random));
    CHECK(BCryptGenRandom(NULL,nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&again));
    CHECK(BCryptGenRandom(NULL,nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&last));CHECK(again==last);
    fprintf(stderr,"raw WinAPI controls cold=%lu volume-closed=%lu BCrypt=%lu repeated=%lu/%lu\n",cold,volume,random,again,last);
    return last;
}

static void inject(void){
    const char *value;
    if((value=getenv("XR_FMT_TEST_ALLOC")))fail_allocation=(size_t)strtoull(value,NULL,10);
    if((value=getenv("XR_FMT_TEST_IO")))fail_io=(size_t)strtoull(value,NULL,10);
    if((value=getenv("XR_FMT_TEST_OS_ERROR")))injected_error=(DWORD)strtoul(value,NULL,10);
    if((value=getenv("XR_FMT_TEST_MODE"))){
        if(!strcmp(value,"published-close"))permanent_close=true;
        else if(!strcmp(value,"unpublished-close")){force_flush_failure=true;fail_disposition=true;}
        else if(!strcmp(value,"transient-close"))fail_close=0;
        else if(!strcmp(value,"dir-open"))fmt_dir_open_failure=true;
        else if(!strcmp(value,"dir-next"))fmt_dir_next_failure=true;
    }
}
#endif
XR_FUNC int cmd_fmt(const XrCliInvocation *inv);
int wmain(int argc,wchar_t **wide) {
#if FMT_INJECTED
    DWORD before_handles=winapi_control(),after_handles=0;
    inject();
    record_work=getenv("XR_FMT_TEST_RECORD_WORK")!=NULL;
#endif
    XrWinPathStatus status;
    char **argv=xr_win_utf16_arguments(argc,wide,&status);
    if(!argv)return 4;
    XrCliContext context={.program="xray"};
    XrCliInvocation invocation;
    int result=(int)xr_cli_parse_command(xr_cli_find_command("fmt"),argc-1,argv+1,&context,&invocation);
    if(!result){result=cmd_fmt(&invocation);xr_cli_invocation_free(&invocation);}
    xr_win_utf8_arguments_free(argc,argv);
#if FMT_INJECTED
    CHECK(!live_allocations);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after_handles) && after_handles==before_handles);
    fprintf(stderr,"FMT_HANDLES before=%lu after=%lu\n",before_handles,after_handles);
    fprintf(stderr,"FMT_STATS allocations=%zu io=%zu work=%llu allocated=%llu peak=%llu ledgers=%u live=%zu\n",
        allocation_attempts,io_attempts,(unsigned long long)recorded.work,(unsigned long long)recorded.allocated_bytes,
        (unsigned long long)recorded.peak_bytes,ledger_count,live_allocations);
    if(record_work){fprintf(stderr,"FMT_WORK");for(size_t i=0;i<work_count;++i)fprintf(stderr," %llu",(unsigned long long)work_points[i]);fprintf(stderr,"\n");}
#endif
    return result;
}
