/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Actual local preparation owns ready inputs and preserves failure cleanup. */
#include "base/xmalloc.h"
#include "app/toolchain/xtc_process.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s win=%lu\n",__LINE__,#c,GetLastError());exit(1);}}while(0)
typedef struct Physical {void *pointer;size_t bytes;} Physical;
static Physical physical[8192];
static size_t attempts,blocks,physical_bytes,fail_at=SIZE_MAX;
static void *ready_malloc(size_t bytes) {
    if(attempts++==fail_at)return NULL;
    void *p=xr_malloc(bytes);if(!p)return NULL;
    for(unsigned i=0;i<8192;++i)if(!physical[i].pointer){physical[i]=(Physical){p,bytes};++blocks;physical_bytes+=bytes;return p;}
    CHECK(false);return NULL;
}
static void ready_free(void *p) {
    if(!p)return;
    for(unsigned i=0;i<8192;++i)if(physical[i].pointer==p){physical_bytes-=physical[i].bytes;physical[i]=(Physical){0};--blocks;xr_free(p);return;}
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc ready_malloc
#define xr_free ready_free
#include "base/xcompile_resources.c"
static bool inject_io,inject_copy,copy_fired;
static size_t copied_field_size;
static unsigned vswhere_calls,activation_calls;
static XrCompileResourceStatus ready_alloc(XrCompileResources *resources,size_t bytes,void **output) {
    if(inject_copy&&bytes==copied_field_size){inject_copy=false;copy_fired=true;fail_at=attempts;}
    XrCompileResourceStatus status=xr_compile_resources_alloc(resources,bytes,output);fail_at=SIZE_MAX;return status;
}
static int WINAPI ready_compare(LPCWCH a,int na,LPCWCH b,int nb,BOOL ignore) {
    if(inject_io){inject_io=false;SetLastError(ERROR_ACCESS_DENIED);return 0;}
    return CompareStringOrdinal(a,na,b,nb,ignore);
}
static bool ready_basename(const char *path,const char *name) {
    const char *p=path;for(const char *at=path;*at;++at)if(*at=='/'||*at=='\\')p=at+1;
    return _stricmp(p,name)==0;
}
static bool arm_after_vswhere;
static XrProcessStatus ready_run(const XrToolchainProcess *process,XrProcessCancelled cancel,
    void *context,XrProcessResult *output) {
    XrProcessView view={0};CHECK(xtc_process_view(process,&view)==XTC_PROCESS_OK);
    bool vs=ready_basename(view.executable,"vswhere.exe");
    if(vs)++vswhere_calls;
    if(ready_basename(view.executable,"cmd.exe"))++activation_calls;
    XrProcessStatus status=xtc_process_run(process,cancel,context,output);
    if(vs&&arm_after_vswhere&&status==XTC_PROCESS_OK){arm_after_vswhere=false;inject_copy=true;}
    return status;
}
#define xr_compile_resources_alloc ready_alloc
#define CompareStringOrdinal ready_compare
#define xtc_process_run ready_run
#include "app/toolchain/xtc_xir_local_toolchain.c"
#undef xr_compile_resources_alloc
#undef CompareStringOrdinal
#undef xtc_process_run
static const char *selected(const XrProcessView *view,const char *key) {
    for(size_t i=0;i<view->env_count;++i)if(local_environment_key(view->env_keys[i],key))return view->env_values[i];
    return NULL;
}
static size_t environment_units(const wchar_t *p) {size_t n=0;while(p[n]||p[n+1])++n;return n+2;}
static XtcXirLocalToolchain *unit_owner(XrCompileResources *resources) {
    XtcXirLocalToolchain *owner=NULL;CHECK(xr_compile_resources_calloc(resources,1,sizeof(*owner),(void **)&owner)==XR_COMPILE_RESOURCE_OK);
    owner->resources=resources;owner->policy=xr_compile_io_policy(resources);owner->diagnostic.stage=XTC_XIR_LOCAL_ENVIRONMENT;return owner;
}
static void trial(const char *self,unsigned mode) {
    XrCompileResourceLimits ordinary={XR_COMPILE_MEMORY_ORDINARY,0,true};XrCompileResources *snapshot_resources=NULL,*resources=NULL;
    CHECK(xr_compile_resources_new(&ordinary,&snapshot_resources)==XR_COMPILE_RESOURCE_OK);
    XrProcessSpec spec;xtc_process_spec_init(&spec,self,30000);spec.environment_source=XTC_PROCESS_ENV_SNAPSHOT;
    XrToolchainProcess *snapshot=NULL;CHECK(xtc_process_prepare(snapshot_resources,&spec,&snapshot)==XTC_PROCESS_OK);
    XrProcessView view={0};CHECK(xtc_process_view(snapshot,&view)==XTC_PROCESS_OK);
    const char *installation=selected(&view,"VSINSTALLDIR");CHECK(installation&&selected(&view,"WindowsSdkDir"));
    char expected[4][1024];const char *keys[]={"VCToolsInstallDir","WindowsSdkDir","WindowsSDKVersion","SystemRoot"};
    for(unsigned k=0;k<4;++k){const char *v=selected(&view,keys[k]);CHECK(v&&strlen(v)<sizeof(expected[k]));strcpy(expected[k],v);}
    XrCompileResourceLimits limits=ordinary;if(mode==5){limits.memory_mode=XR_COMPILE_MEMORY_FINITE;limits.memory_limit=UINT64_C(1)<<20;}
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats frame={0};CHECK(xr_compile_resources_stats(resources,&frame)==XR_COMPILE_RESOURCE_OK);
    XtcXirLocalToolchain *owner=unit_owner(resources);void *filler=NULL;
    const char *values[4]={0};bool ready=true;
    const char **modified=NULL;
    if(mode==1||mode==2) {
        modified=ready_malloc(view.env_count*sizeof(*modified));CHECK(modified);
        memcpy(modified,view.env_values,view.env_count*sizeof(*modified));
        for(size_t i=0;i<view.env_count;++i) {
            if(mode==1&&local_environment_key(view.env_keys[i],"VCToolsInstallDir"))modified[i]="";
            if(mode==2&&local_environment_key(view.env_keys[i],"VSCMD_ARG_TGT_ARCH"))modified[i]="x86";
        }
        view.env_values=modified;
    }
    if(mode==3)inject_io=true;
    if(mode==4){copied_field_size=strlen(expected[1])+1;inject_copy=true;copy_fired=false;}
    if(mode==5) {
        uint64_t low=1,high=limits.memory_limit;
        while(low<high){uint64_t middle=low+(high-low+1)/2;if(xr_compile_resources_admit(resources,(size_t)middle)==XR_COMPILE_RESOURCE_OK)low=middle;else high=middle-1;}
        CHECK(xr_compile_resources_alloc(resources,(size_t)low,&filler)==XR_COMPILE_RESOURCE_OK);
    }
    bool okay=local_ready_environment(owner,&view,installation,values,&ready);
    if(mode==0){CHECK(okay&&ready);}
    else if(mode==1||mode==2){CHECK(okay&&!ready&&owner->diagnostic.status==XTC_XIR_LOCAL_OK);}
    else {CHECK(!okay&&!ready);CHECK(owner->diagnostic.status==(mode==3?XTC_XIR_LOCAL_IO:mode==4?XTC_XIR_LOCAL_OUT_OF_MEMORY:XTC_XIR_LOCAL_MEMORY_LIMIT));}
    if(mode==3)CHECK(owner->diagnostic.os_error==ERROR_ACCESS_DENIED);
    if(mode==4)CHECK(copy_fired&&values[0]);
    ready_free(modified);xtc_process_free(snapshot);xr_compile_resources_release(snapshot_resources);
    if(mode==0) {
        for(unsigned k=0;k<4;++k)CHECK(values[k]&&!strcmp(values[k],expected[k]));
        CHECK(local_paths(owner,values,NULL));CHECK(owner->view.library_count==5);
    }
    xr_compile_resources_free(filler);xtc_xir_local_toolchain_free(owner);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==frame.held_bytes);
    xr_compile_resources_release(resources);CHECK(!blocks&&!physical_bytes);
    printf("ready trial%u owned snapshot death, eligibility status and physical rollback PASS\n",mode);
}
static void opened(int argc,char **argv) {
    CHECK(argc==8); /* --open sdk compiler workspace expected status vswhere cmd */
    if(!strcmp(argv[4],"copy")){arm_after_vswhere=true;const char *v=getenv("WindowsSdkDir");CHECK(v);copied_field_size=strlen(v)+1;}
    if(!strcmp(argv[4],"io"))inject_io=true;
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XtcXirLocalToolchainRequest request={strcmp(argv[3],"-")?argv[3]:NULL,argv[2],NULL,30000,1048576,4194304};
    XtcXirLocalToolchain *owner=NULL;XtcXirLocalDiagnostic diagnostic={0};
    XtcXirLocalStatus status=xtc_xir_local_toolchain_open(resources,&request,&owner,&diagnostic);
    printf("actual status=%u stage=%u os=%lu vswhere=%u activation=%u\n",(unsigned)status,(unsigned)diagnostic.stage,(unsigned long)diagnostic.os_error,vswhere_calls,activation_calls);fflush(stdout);
    CHECK(status==(XtcXirLocalStatus)atoi(argv[5])&&status==diagnostic.status);
    CHECK(vswhere_calls==(unsigned)atoi(argv[6])&&activation_calls==(unsigned)atoi(argv[7]));
    if(status==XTC_XIR_LOCAL_CHILD_FAILED)CHECK(diagnostic.domain==XTC_XIR_LOCAL_PROCESS&&diagnostic.child_exit==1&&diagnostic.os_error==0);
    if(!strcmp(argv[4],"copy"))CHECK(copy_fired);
    xr_compile_resources_release(resources);
    if(status==XTC_XIR_LOCAL_OK){CHECK(owner);const XtcXirLocalToolchainView *v=xtc_xir_local_toolchain_view(owner);CHECK(v&&v->library_count==5);printf("compiler=%s\nlinker=%s\nsdk=%s\n",v->compiler,v->linker,xr_xir_runtime_sdk_root(xtc_xir_local_toolchain_sdk(owner)));}
    else CHECK(!owner);
    xtc_xir_local_toolchain_free(owner);CHECK(!blocks&&!physical_bytes);
    printf("status=%u vswhere=%u activation=%u physical0 PASS\n",(unsigned)status,vswhere_calls,activation_calls);
}
int main(int argc,char **argv) {
    LPWCH before=GetEnvironmentStringsW();CHECK(before);size_t units=environment_units(before);
    if(argc>1&&!strcmp(argv[1],"--open"))opened(argc,argv);
    else {CHECK(argc==1);for(unsigned mode=0;mode<6;++mode)trial(argv[0],mode);}
    LPWCH after=GetEnvironmentStringsW();CHECK(after&&environment_units(after)==units&&!memcmp(before,after,units*sizeof(*before)));
    CHECK(FreeEnvironmentStringsW(after)&&FreeEnvironmentStringsW(before));
    puts("Same C-process parent environment unchanged; actual local ready receiver PASS");return 0;
}
