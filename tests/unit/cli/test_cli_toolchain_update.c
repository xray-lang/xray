/* Persistent preference transactions on real files and the original ledger. */
#include "app/cli/xcli_toolchain_update.h"
#include "base/xmalloc.h"
#include "base/xwindows_utf8.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s error=%lu\n",__LINE__,#c,GetLastError());exit(1); } } while(0)
static void *physical_malloc(size_t size) { return xr_malloc(size); }
static void physical_free(void *memory) { xr_free(memory); }
typedef struct Block { void *pointer;size_t size; } Block;
static Block blocks[8192];
static size_t calls, fail_at=SIZE_MAX, live;
static void *observed_malloc(size_t size) {
    if(calls++==fail_at)return NULL;
    void *memory=physical_malloc(size);if(!memory)return NULL;
    for(size_t i=0;i<8192;++i)if(!blocks[i].pointer){blocks[i]=(Block){memory,size};live+=size;return memory;}
    CHECK(false);return NULL;
}
static void observed_free(void *memory) {
    if(!memory)return;
    for(size_t i=0;i<8192;++i)if(blocks[i].pointer==memory){live-=blocks[i].size;blocks[i]=(Block){0};physical_free(memory);return;}
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc observed_malloc
#define xr_free observed_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
#ifdef UPDATE_INJECTED
static unsigned rejected_closes;
static BOOL WINAPI injected_close(HANDLE handle){
    if(rejected_closes){--rejected_closes;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return CloseHandle(handle);
}
#define CloseHandle injected_close
#include "app/cli/xcli_toolchain_update.c"
#undef CloseHandle
#endif
static const XrCompileResourceLimits defaults={UINT64_C(1)<<28,UINT64_C(1)<<26,UINT64_C(1)<<32};
static const char original[]="schema = 1\n[native]\nprovider = \"msvc\"\ncompiler = \"before.exe\"\n[policy]\nallow_fallback = false\n";
static DWORD handles(void){DWORD n=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&n));return n;}
static void winapi_control(void){
    /* Observe process initialization with raw APIs, without warming any owner.
     * Every transaction must then return exactly to its own settled baseline. */
    DWORD cold=handles();
    HANDLE directory=CreateFileW(L".",FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);CHECK(directory!=INVALID_HANDLE_VALUE);
    wchar_t filesystem[16];CHECK(GetVolumeInformationByHandleW(directory,NULL,0,NULL,NULL,NULL,filesystem,16));
    CHECK(CloseHandle(directory));DWORD volume=handles();
    UCHAR nonce[16];CHECK(BCryptGenRandom(NULL,nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);
    DWORD random=handles();CHECK(BCryptGenRandom(NULL,nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);
    DWORD again=handles();CHECK(BCryptGenRandom(NULL,nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);
    DWORD last=handles();CHECK(again==last);
    fprintf(stderr,"raw controls cold=%lu volume-closed=%lu BCrypt=%lu repeated=%lu/%lu\n",cold,volume,random,again,last);
}
static void fixture(const char *path,bool write){
    XrOsIoPolicy policy=xr_os_io_system_policy();wchar_t *wide=NULL;
    CHECK(xr_win_utf8_path_owned(&policy,path,&wide)==XR_OS_IO_OK);
    HANDLE file=CreateFileW(wide,write?GENERIC_WRITE:GENERIC_READ,FILE_SHARE_READ,NULL,
        write?CREATE_ALWAYS:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    policy.free(policy.context,wide);CHECK(file!=INVALID_HANDLE_VALUE);
    DWORD n=0;
    if(write)CHECK(WriteFile(file,original,sizeof(original)-1,&n,NULL)&&n==sizeof(original)-1);
    else{char bytes[sizeof(original)];CHECK(ReadFile(file,bytes,sizeof(bytes),&n,NULL)&&n==sizeof(original)-1);
        CHECK(!memcmp(bytes,original,n));}
    CHECK(CloseHandle(file));
}
static XrCliToolchainUpdateStatus run(const XrCliToolchainUpdateRequest *request,size_t cut,
    const XrCompileResourceLimits *limits,XrCompileResourceStats *stats,bool producer_death){
    char path[4096],target[128],compiler[4096];
    XrCliToolchainUpdateRequest transient=*request;
    if(producer_death){
        CHECK(request->path&&strlen(request->path)<sizeof(path));strcpy(path,request->path);transient.path=path;
        if(request->target){CHECK(strlen(request->target)<sizeof(target));strcpy(target,request->target);transient.target=target;}
        if(request->compiler){CHECK(strlen(request->compiler)<sizeof(compiler));strcpy(compiler,request->compiler);transient.compiler=compiler;}
        request=&transient;
    }
    CHECK(!live);calls=0;fail_at=cut;DWORD baseline=handles();
    XrCompileResources *resources=NULL;XrCompileResourceStatus allocated=xr_compile_resources_new(limits,&resources);
    if(allocated!=XR_COMPILE_RESOURCE_OK){CHECK(!live);return allocated==XR_COMPILE_RESOURCE_BUDGET?
        XR_CLI_TOOLCHAIN_UPDATE_BUDGET:XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY;}
    XrCompileState *state=NULL;allocated=xr_compile_state_new(resources,&state);
    if(allocated!=XR_COMPILE_RESOURCE_OK) {
        xr_compile_resources_release(resources);CHECK(!live);
        return allocated==XR_COMPILE_RESOURCE_BUDGET ? XR_CLI_TOOLCHAIN_UPDATE_BUDGET : XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY;
    }
    XrCliToolchainUpdate *owner=NULL;
    XrCliToolchainUpdateStatus status=xr_cli_toolchain_update_new(state,request,&owner);
    if(status==XR_CLI_TOOLCHAIN_UPDATE_OK){
        XrCliToolchainUpdate *occupied=owner;
        CHECK(xr_cli_toolchain_update_new(state,request,&occupied)==XR_CLI_TOOLCHAIN_UPDATE_INVALID&&occupied==owner);
        if(producer_death){memset(path,0xcc,sizeof(path));memset(target,0xcc,sizeof(target));memset(compiler,0xcc,sizeof(compiler));
            xr_compile_state_release(state);state=NULL;xr_compile_resources_release(resources);resources=NULL;}
        status=xr_cli_toolchain_update_execute(owner);
        const XrCliToolchainUpdateView *view=xr_cli_toolchain_update_view(owner);CHECK(view&&view->path);
        CHECK(view->first_error==status);
        if(status==XR_CLI_TOOLCHAIN_UPDATE_OK)CHECK(xr_cli_toolchain_update_execute(owner)==XR_CLI_TOOLCHAIN_UPDATE_INVALID);
        else CHECK(xr_cli_toolchain_update_execute(owner)==status);
    }
    if(resources){
        CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
        if(status==XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY||status==XR_CLI_TOOLCHAIN_UPDATE_BUDGET){
            XrCompileResourceStats before=*stats,after;
            size_t attempts=calls;
            if(!owner)CHECK(xr_cli_toolchain_update_new(state,request,&owner)==status&&!owner);
            else CHECK(xr_cli_toolchain_update_execute(owner)==status);
            XrCliToolchainUpdate *retry=NULL;XrCliToolchainConfig *snapshot=NULL;char *locator=NULL;
            XrCliToolchainConfigStatus expected=status==XR_CLI_TOOLCHAIN_UPDATE_BUDGET ?
                XR_CLI_TOOLCHAIN_CONFIG_BUDGET : XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY;
            CHECK(xr_cli_toolchain_update_new(state,request,&retry)==status && !retry);
            CHECK(xr_cli_toolchain_config_open(state,request->path,&snapshot)==expected && !snapshot);
            CHECK(xr_cli_toolchain_config_path(state,request->path,&locator)==expected && !locator && calls==attempts);
            CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
            CHECK(before.allocated_bytes==after.allocated_bytes&&before.work==after.work);
        }
    }
    XrCliToolchainUpdateStatus close=XR_CLI_TOOLCHAIN_UPDATE_PENDING;
    for(unsigned i=0;i<64&&close==XR_CLI_TOOLCHAIN_UPDATE_PENDING;++i)close=xr_cli_toolchain_update_close(&owner);
    CHECK(close==XR_CLI_TOOLCHAIN_UPDATE_OK&&!owner);
    xr_compile_state_release(state);xr_compile_resources_release(resources);
    DWORD after_handles=handles();
    if(live||after_handles!=baseline)fprintf(stderr,"release: live=%zu handles=%lu baseline=%lu status=%u\n",live,after_handles,baseline,(unsigned)status);
    CHECK(!live&&after_handles==baseline);return status;
}
static void shared_state_failures(const char *path) {
    for(unsigned family=0;family<6;++family) {
        calls=0;fail_at=SIZE_MAX;DWORD baseline=handles();CHECK(!live);
        XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&defaults,&resources)==XR_COMPILE_RESOURCE_OK);
        XrCompileState *state=NULL;CHECK(xr_compile_state_new(resources,&state)==XR_COMPILE_RESOURCE_OK);
        XrCliToolchainUpdateRequest request={path,"native","after.exe",NULL,XR_TOOLCHAIN_SELECTOR_MSVC,false};
        XrCliToolchainConfig *snapshot=NULL;XrCliToolchainUpdate *transaction=NULL;
        if(family==4)CHECK(xr_cli_toolchain_config_open(state,path,&snapshot)==XR_CLI_TOOLCHAIN_CONFIG_OK);
        if(family==5)CHECK(xr_cli_toolchain_update_new(state,&request,&transaction)==XR_CLI_TOOLCHAIN_UPDATE_OK);
        XrCompileResourceStatus fatal=family==3 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
        if(family==3)CHECK(xr_compile_state_work(state,UINT64_MAX)==fatal);
        else {
            fail_at=calls;
            if(family==0 || family==4) {
                XrCliToolchainConfig *empty=NULL;
                CHECK(xr_cli_toolchain_config_open(state,path,&empty)==XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY && !empty);
            } else if(family==1) {
                XrCliToolchainUpdate *empty=NULL;
                CHECK(xr_cli_toolchain_update_new(state,&request,&empty)==XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY && !empty);
            } else {
                char *empty=NULL;
                CHECK(xr_cli_toolchain_config_path(state,path,&empty)==XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY && !empty);
            }
        }
        CHECK(xr_compile_state_status(state)==fatal);
        XrCliToolchainConfigStatus expected=family==3 ? XR_CLI_TOOLCHAIN_CONFIG_BUDGET : XR_CLI_TOOLCHAIN_CONFIG_OUT_OF_MEMORY;
        XrCliToolchainUpdateStatus updated=family==3 ? XR_CLI_TOOLCHAIN_UPDATE_BUDGET : XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY;
        XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
        size_t attempts=calls;XrCliToolchainConfig *empty=NULL;XrCliToolchainUpdate *retry=NULL;char *locator=NULL;
        CHECK(xr_cli_toolchain_config_open(state,path,&empty)==expected && !empty);
        CHECK(xr_cli_toolchain_config_path(state,path,&locator)==expected && !locator);
        CHECK(xr_cli_toolchain_update_new(state,&request,&retry)==updated && !retry);
        if(transaction)CHECK(xr_cli_toolchain_update_execute(transaction)==updated && !xr_cli_toolchain_update_view(transaction)->published);
        if(snapshot)CHECK(xr_cli_toolchain_config_view(snapshot)->exists);
        CHECK(calls==attempts && xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK &&
            before.work==after.work && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
        fail_at=SIZE_MAX;xr_compile_state_release(state);xr_compile_resources_release(resources);
        xr_cli_toolchain_config_free(snapshot);
        CHECK(xr_cli_toolchain_update_close(&transaction)==XR_CLI_TOOLCHAIN_UPDATE_OK && !transaction);
        CHECK(!live && handles()==baseline);
    }
    puts("Six real shared-state failures: config/update/path constructor OOM and work budget; cross-entry sticky/no work/allocation; retained snapshot/transaction after caller death; physical0/handles baseline");
}
static int test_main(int argc,char **argv){
    CHECK(argc>=3);
    winapi_control();
    if(!strcmp(argv[2],"state")){shared_state_failures(argv[1]);return 0;}
#ifdef UPDATE_INJECTED
    if(!strcmp(argv[2],"cleanup")){
        DWORD baseline=handles();CHECK(!live);calls=0;fail_at=SIZE_MAX;
        XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&defaults,&resources)==XR_COMPILE_RESOURCE_OK);
        XrCompileState *state=NULL;CHECK(xr_compile_state_new(resources,&state)==XR_COMPILE_RESOURCE_OK);
        XrCliToolchainUpdateRequest request={argv[1],"native","retained.exe",NULL,XR_TOOLCHAIN_SELECTOR_MSVC,false};
        XrCliToolchainUpdate *owner=NULL;CHECK(xr_cli_toolchain_update_new(state,&request,&owner)==XR_CLI_TOOLCHAIN_UPDATE_OK);
        CHECK(xr_cli_toolchain_update_execute(owner)==XR_CLI_TOOLCHAIN_UPDATE_OK);
        XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
        xr_compile_state_release(state);xr_compile_resources_release(resources);
        rejected_closes=2;
        for(unsigned i=0;i<2;++i){
            XrCliToolchainUpdate *original_owner=owner;
            CHECK(xr_cli_toolchain_update_close(&owner)==XR_CLI_TOOLCHAIN_UPDATE_PENDING&&owner==original_owner);
            const XrCliToolchainUpdateView *view=xr_cli_toolchain_update_view(owner);
            CHECK(view->published&&!view->removed&&view->first_error==XR_CLI_TOOLCHAIN_UPDATE_IO);
            CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
            CHECK(before.allocated_bytes==after.allocated_bytes&&before.work==after.work);
        }
        CHECK(!rejected_closes&&xr_cli_toolchain_update_close(&owner)==XR_CLI_TOOLCHAIN_UPDATE_OK&&!owner);
        CHECK(!live&&handles()==baseline);puts("pending cleanup retains owner/lock and first error; no fresh work; final physical0/handles baseline");return 0;
    }
#endif
    if(!strcmp(argv[2],"faults")){
        XrCliToolchainUpdateRequest request={argv[1],"native","C:/after/编译器/cl.exe",NULL,XR_TOOLCHAIN_SELECTOR_MSVC,false};
        XrCompileResourceStats stats={0};
        fixture(argv[1],true);CHECK(run(&request,SIZE_MAX,&defaults,&stats,false)==XR_CLI_TOOLCHAIN_UPDATE_OK);
        size_t total=calls;
        for(size_t cut=0;cut<total;++cut){
            fixture(argv[1],true);XrCompileResourceStats unused={0};
            CHECK(run(&request,cut,&defaults,&unused,false)==XR_CLI_TOOLCHAIN_UPDATE_OUT_OF_MEMORY);fixture(argv[1],false);
        }
        const uint64_t bounds[3]={stats.allocated_bytes,stats.peak_bytes,stats.work};
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
            fixture(argv[1],true);XrCompileResourceLimits limits=defaults;uint64_t bound=bounds[axis];CHECK(bound);
            uint64_t selected=delta<0?bound-1:bound+(unsigned)delta;
            if(axis==0)limits.allocated_bytes=selected;if(axis==1)limits.live_bytes=selected;if(axis==2)limits.work=selected;
            XrCompileResourceStats unused={0};XrCliToolchainUpdateStatus status=run(&request,SIZE_MAX,&limits,&unused,false);
            CHECK(status==(delta<0?XR_CLI_TOOLCHAIN_UPDATE_BUDGET:XR_CLI_TOOLCHAIN_UPDATE_OK));
            if(delta<0)fixture(argv[1],false);
        }
        fixture(argv[1],true);CHECK(run(&request,SIZE_MAX,&defaults,&stats,true)==XR_CLI_TOOLCHAIN_UPDATE_OK);
        printf("complete update cuts=%zu axes=3 physical=0 handles=baseline\n",total);return 0;
    }
    CHECK(argc==6);
    bool reset=!strcmp(argv[2],"reset")||!strcmp(argv[2],"reset-all");
    XrCliToolchainUpdateRequest request={argv[1],!strcmp(argv[2],"reset-all")?NULL:argv[3],
        !strcmp(argv[4],"-")?NULL:argv[4],NULL,reset?XR_TOOLCHAIN_SELECTOR_AUTO:XR_TOOLCHAIN_SELECTOR_MSVC,reset};
    XrCompileResourceStats stats={0};
    XrCliToolchainUpdateStatus status=run(&request,SIZE_MAX,&defaults,&stats,true);
    if(status!=atoi(argv[5]))fprintf(stderr,"actual status=%u expected=%s\n",(unsigned)status,argv[5]);
    CHECK(status==atoi(argv[5]));printf("status=%u\n",(unsigned)status);return 0;
}
int wmain(int argc,wchar_t **wide){
    XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);
    int result=test_main(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;
}
