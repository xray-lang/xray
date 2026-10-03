/* Real discovery and focused ownership checks on the sole locator. */
#include "app/toolchain/xtc_xir_local_toolchain.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s error=%lu\n", __LINE__, #c, GetLastError()); exit(1); } } while (0)
#include "base/xmalloc.h"
#include "base/xwindows_utf8.h"
static void *physical_malloc(size_t n) { return xr_malloc(n); }
static void physical_free(void *p) { xr_free(p); }
typedef struct Block { void *p; size_t size; } Block;
static Block blocks[8192];
static size_t attempts, fail_at = SIZE_MAX, live, total, peak;
static uint64_t edges[8192]; static size_t edge_count; static bool record_edges;
static void *observed_malloc(size_t n) {
    if (attempts++ == fail_at) return NULL;
    void *p = physical_malloc(n); if (!p) return NULL;
    for (size_t i = 0; i < 8192; ++i) if (!blocks[i].p) {
        blocks[i] = (Block){p,n}; live += n; total += n; if (live > peak) peak = live; return p;
    }
    CHECK(false); return NULL;
}
static void observed_free(void *p) {
    if (!p) return;
    for (size_t i = 0; i < 8192; ++i) if (blocks[i].p == p) {
        live -= blocks[i].size; blocks[i] = (Block){0}; physical_free(p); return;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc observed_malloc
#define xr_free observed_free
#define xr_compile_resources_work local_real_work
#include "base/xcompile_resources.c"
#undef xr_compile_resources_work
#undef xr_malloc
#undef xr_free
#define xr_malloc physical_malloc
#define xr_free physical_free
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *r, uint64_t n) {
    XrCompileResourceStatus status = local_real_work(r,n);
    if (record_edges && status == XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(r,&stats) == XR_COMPILE_RESOURCE_OK);
        if (!edge_count || edges[edge_count-1] != stats.work) { CHECK(edge_count < 8192); edges[edge_count++] = stats.work; }
    }
    return status;
}
static const XrCompileResourceLimits defaults = {UINT64_C(1)<<30,UINT64_C(256)<<20,UINT64_C(8)<<30};
static void reset(size_t failure) { CHECK(!live); attempts=total=peak=0; fail_at=failure; }
static DWORD handles(void) { DWORD count=0; CHECK(GetProcessHandleCount(GetCurrentProcess(),&count)); return count; }
#ifndef LOCAL_PRODUCTION
static bool absent_temp, fail_temp, fail_compare;
static DWORD injected_error=ERROR_ACCESS_DENIED;
static XrOsIoStatus local_test_environment(const XrOsIoPolicy *p,const char *key,char **out) {
    if(absent_temp && (!strcmp(key,"TEMP") || !strcmp(key,"TMP"))) return XR_OS_IO_NOT_FOUND;
    return xr_os_io_environment_get(p,key,out);
}
static DWORD WINAPI local_test_temp(DWORD n,LPWSTR out) {
    if(fail_temp) { SetLastError(injected_error); return 0; } return GetTempPathW(n,out);
}
static int WINAPI local_test_compare(LPCWCH a,int an,LPCWCH b,int bn,BOOL insensitive) {
    if(fail_compare) { SetLastError(injected_error); return 0; } return CompareStringOrdinal(a,an,b,bn,insensitive);
}
#define xr_os_io_environment_get local_test_environment
#define GetTempPathW local_test_temp
#define CompareStringOrdinal local_test_compare
#include "app/toolchain/xtc_xir_local_toolchain.c"
#undef xr_os_io_environment_get
#undef GetTempPathW
#undef CompareStringOrdinal
static XtcXirLocalToolchain *unit_owner(XrCompileResources **r) {
    reset(SIZE_MAX); *r=NULL; CHECK(xr_compile_resources_new(&defaults,r)==XR_COMPILE_RESOURCE_OK);
    XtcXirLocalToolchain *owner=NULL; CHECK(xr_compile_resources_calloc(*r,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->resources=*r; owner->policy=xr_compile_io_policy(*r); return owner;
}
static void unit_free(XtcXirLocalToolchain *owner,XrCompileResources *r) {
    xr_compile_resources_release(r); xtc_xir_local_toolchain_free(owner); CHECK(!live);
}
static const wchar_t environment[] = L"Unrelated=not retained\r\nVCToolsInstallDir=C:\\VC\\\r\nWindowsSdkDir=C:\\Kit\\\r\n"
    L"WindowsSDKVersion=10.0.22621.0\\\r\nSystemRoot=C:\\Windows\r\n";
static XtcXirLocalStatus parsed(const wchar_t *text, size_t bytes, size_t failure,
    const XrCompileResourceLimits *limits, XrCompileResourceStats *stats) {
    reset(failure); XrCompileResources *resources = NULL;
    XrCompileResourceStatus s = xr_compile_resources_new(limits,&resources);
    if (s != XR_COMPILE_RESOURCE_OK) { CHECK(!live); return local_resource_status(s); }
    XtcXirLocalToolchain *owner = NULL;
    s = xr_compile_resources_calloc(resources,1,sizeof(*owner),(void **)&owner);
    XtcXirLocalStatus status = local_resource_status(s);
    if (owner) {
        owner->resources=resources; owner->policy=xr_compile_io_policy(resources);
        owner->diagnostic.stage=XTC_XIR_LOCAL_ENVIRONMENT;
        const char *values[4]={0}; XrProcessByteBuffer b={(uint8_t *)text,bytes,false};
        bool okay=local_environment(owner,&b,values); status=owner->diagnostic.status;
        if (okay) { CHECK(!strcmp(values[0],"C:\\VC\\") && !strcmp(values[1],"C:\\Kit\\"));
            CHECK(!strcmp(values[2],"10.0.22621.0\\") && !strcmp(values[3],"C:\\Windows")); }
    }
    CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats->allocated_bytes==total && stats->live_bytes==live && stats->peak_bytes==peak);
    xr_compile_resources_release(resources); xtc_xir_local_toolchain_free(owner); CHECK(!live); return status;
}
static void parser_tests(void) {
    XrCompileResourceStats base={0},stats={0}; record_edges=true; edge_count=0;
    CHECK(parsed(environment,sizeof(environment)-2,SIZE_MAX,&defaults,&base)==XTC_XIR_LOCAL_OK);
    record_edges=false; size_t count=attempts, cuts=edge_count;
    for(size_t i=0;i<count;++i) CHECK(parsed(environment,sizeof(environment)-2,i,&defaults,&stats)==XTC_XIR_LOCAL_OUT_OF_MEMORY);
    for(size_t i=0;i<cuts;++i) { XrCompileResourceLimits limit=defaults; limit.work=edges[i]-1;
        CHECK(parsed(environment,sizeof(environment)-2,SIZE_MAX,&limit,&stats)==XTC_XIR_LOCAL_BUDGET); }
    for(unsigned axis=0;axis<3;++axis) for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limit=defaults;
        if(axis==0)limit.allocated_bytes=base.allocated_bytes-minus;
        if(axis==1)limit.live_bytes=base.peak_bytes-minus;
        if(axis==2)limit.work=base.work-minus;
        CHECK(parsed(environment,sizeof(environment)-2,SIZE_MAX,&limit,&stats)==(minus?XTC_XIR_LOCAL_BUDGET:XTC_XIR_LOCAL_OK));
    }
    static const wchar_t duplicate[]=L"SystemRoot=C:\\Windows\r\nsystemroot=C:\\other\r\n";
    CHECK(parsed(duplicate,sizeof(duplicate)-2,SIZE_MAX,&defaults,&stats)==XTC_XIR_LOCAL_INVALID);
    CHECK(parsed(L"SystemRoot=C:\\Windows\r\n",46,SIZE_MAX,&defaults,&stats)==XTC_XIR_LOCAL_UNRESOLVED);
    CHECK(parsed(environment,sizeof(environment)-3,SIZE_MAX,&defaults,&stats)==XTC_XIR_LOCAL_INVALID);
    static const wchar_t bad_utf[]={L'S',L'y',L's',L't',L'e',L'm',L'R',L'o',L'o',L't',L'=',0xd800,L'\r',L'\n'};
    CHECK(parsed(bad_utf,sizeof(bad_utf),SIZE_MAX,&defaults,&stats)==XTC_XIR_LOCAL_INVALID);
    static const wchar_t nul[]={L'S',0,L'='};
    CHECK(parsed(nul,sizeof(nul),SIZE_MAX,&defaults,&stats)==XTC_XIR_LOCAL_INVALID);
    printf("environment %zu real malloc, %zu work boundaries, three-axis exact/minus1, physical zero PASS\n",count,cuts);
}
static void windows_tests(void) {
    for(unsigned api=0;api<2;++api) for(unsigned e=0;e<3;++e) {
        XrCompileResources *r=NULL; XtcXirLocalToolchain *owner=unit_owner(&r);
        injected_error=e==0?ERROR_ACCESS_DENIED:e==1?ERROR_NOT_ENOUGH_MEMORY:ERROR_OUTOFMEMORY;
        absent_temp=fail_temp=api==0; fail_compare=api==1;
        CHECK(!(api==0?local_workspace(owner,NULL):local_same_path(owner,"C:\\a","C:\\A")));
        CHECK(owner->diagnostic.status==(e==0?XTC_XIR_LOCAL_IO:XTC_XIR_LOCAL_OUT_OF_MEMORY));
        CHECK(owner->diagnostic.os_error==injected_error);
        unit_free(owner,r);
    }
    absent_temp=fail_temp=fail_compare=false;
    XrCompileResources *r=NULL; XtcXirLocalToolchain *owner=unit_owner(&r);
    CHECK(local_same_path(owner,"C:\\owned-\xc3\xa9","c:\\OWNED-\xc3\x89"));
    CHECK(!local_same_path(owner,"C:\\different","C:\\other") && owner->diagnostic.status==XTC_XIR_LOCAL_INVALID);
    unit_free(owner,r);
    owner=unit_owner(&r);
    static const wchar_t unicode[]=L"VCToolsInstallDir=C:\\VC\x4e2d\\\r\nWindowsSdkDir=C:\\Kit\\\r\nWindowsSDKVersion=10.0.22621.0\\\r\nSystemRoot=C:\\Windows\r\n";
    XrProcessByteBuffer bytes={(uint8_t *)unicode,sizeof(unicode)-2,false}; const char *values[4]={0};
    CHECK(local_environment(owner,&bytes,values) && !strcmp(values[0],"C:\\VC\xe4\xb8\xad\\"));
    unit_free(owner,r);
    owner=unit_owner(&r);
    XtcXirLocalToolchainRequest request={0}; XtcXirLocalToolchain *out=(XtcXirLocalToolchain *)(uintptr_t)1;
    XtcXirLocalDiagnostic diagnostic; size_t before=attempts;
    CHECK(xtc_xir_local_toolchain_open(r,&request,&out,&diagnostic)==XTC_XIR_LOCAL_INVALID && out==(void *)(uintptr_t)1 && attempts==before);
    unit_free(owner,r);
    puts("new Win32 IO/two OOM errors, Unicode ordinal, UTF16 values and invalid out preservation PASS");
}
int main(void) { DWORD before=handles(); parser_tests(); windows_tests(); CHECK(handles()==before); return 0; }
#else
static size_t environment_units(const wchar_t *p) {
    size_t n=0; while(p[n] || p[n+1]) ++n; return n+2;
}
static void real_once(char **argv) {
    reset(SIZE_MAX); XrCompileResources *r=NULL;
    CHECK(xr_compile_resources_new(&defaults,&r)==XR_COMPILE_RESOURCE_OK);
    char *text[3];
    for(unsigned i=0;i<3;++i) { size_t n=strlen(argv[i+1])+1; text[i]=physical_malloc(n); CHECK(text[i]); memcpy(text[i],argv[i+1],n); }
    XtcXirLocalToolchainRequest request={strcmp(text[1],"-")?text[1]:NULL,
        strcmp(text[0],"-")?text[0]:NULL,strcmp(text[2],"-")?text[2]:NULL,30000,1048576,4194304};
    XtcXirLocalToolchain *owner=NULL; XtcXirLocalDiagnostic diagnostic;
    wchar_t *environment_before=GetEnvironmentStringsW(); CHECK(environment_before);
    size_t units=environment_units(environment_before);
    DWORD initial=handles();
    XtcXirLocalStatus status=xtc_xir_local_toolchain_open(r,&request,&owner,&diagnostic);
    wchar_t *environment_after=GetEnvironmentStringsW(); CHECK(environment_after);
    CHECK(units==environment_units(environment_after) && !memcmp(environment_before,environment_after,units*sizeof(wchar_t)));
    CHECK(FreeEnvironmentStringsW(environment_after) && FreeEnvironmentStringsW(environment_before));
    for(unsigned i=0;i<3;++i) { memset(text[i],0xa5,strlen(text[i])); physical_free(text[i]); }
    CHECK(status==diagnostic.status);
    printf("status=%u stage=%u domain=%u code=%d os=%u exit=%d\n",(unsigned)status,(unsigned)diagnostic.stage,
        (unsigned)diagnostic.domain,diagnostic.code,diagnostic.os_error,diagnostic.child_exit);
    XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(r,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.allocated_bytes==total && stats.live_bytes==live && stats.peak_bytes==peak);
    xr_compile_resources_release(r);
    if(status==XTC_XIR_LOCAL_OK) {
        CHECK(owner && xtc_xir_local_toolchain_resources(owner)==r);
        const XtcXirLocalToolchainView *view=xtc_xir_local_toolchain_view(owner);
        const XrXirRuntimeSdk *sdk=xtc_xir_local_toolchain_sdk(owner);
        CHECK(view->library_count==5 && xr_xir_runtime_sdk_resources(sdk)==r);
        CHECK(xr_xir_runtime_sdk_facts(sdk)->program_abi==28);
        printf("compiler=%s\nlinker=%s\nworkspace=%s\nsdk=%s\n",view->compiler,view->linker,view->workspace_parent,xr_xir_runtime_sdk_root(sdk));
        for(unsigned i=0;i<5;++i) { CHECK(view->libraries[i].path); printf("library%u=%s\n",i,view->libraries[i].path); }
    } else CHECK(!owner);
    xtc_xir_local_toolchain_free(owner); CHECK(!live);
    printf("initial_handles=%lu final_handles=%lu allocations=%zu total=%zu peak=%zu work=%llu physical_heap=0\n",
        initial,handles(),attempts,total,peak,(unsigned long long)stats.work);
    CHECK((int)status==atoi(argv[4]));
}
int wmain(int argc,wchar_t **wide) {
    XrWinPathStatus converted; char **argv=xr_win_utf16_arguments(argc,wide,&converted);
    CHECK(argv && converted==XR_WIN_PATH_OK && argc==5);
    real_once(argv); DWORD baseline=handles();
    real_once(argv); CHECK(handles()==baseline);
    puts("parent environment identical; repeated owner handle delta zero; request+ledger producer destroyed PASS");
    xr_win_utf8_arguments_free(argc,argv); return 0;
}
#endif
