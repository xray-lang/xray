/* Real native consumption is separate from the deliberately synthetic, owned
 * composition fixture used to enumerate admission's new failures. */
#include "app/toolchain/xtc_xir_native_admission.h"
#include "toolchain/xcompiler_session.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
static bool capture_work;
static uint64_t work_boundaries[32768];
static size_t boundary_count;
#define xr_compile_resources_work admission_raw_work
#include "../native_invocation_owner/invocation_allocator.h"
#undef xr_compile_resources_work
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources, uint64_t units) {
    if (capture_work && boundary_count < sizeof(work_boundaries)/sizeof(*work_boundaries))
        work_boundaries[boundary_count++] = resources->stats.work + units;
    return admission_raw_work(resources, units);
}
#ifdef ADMISSION_INJECTED
static unsigned utf_calls, utf_fail;
static DWORD utf_error;
static int WINAPI test_utf(UINT page,DWORD flags,LPCCH input,int length,LPWSTR output,int count) {
    if (++utf_calls == utf_fail) { SetLastError(utf_error); return 0; }
    return MultiByteToWideChar(page,flags,input,length,output,count);
}
#define MultiByteToWideChar test_utf
#include "toolchain/xr_xir_runtime_sdk.c"
#include "aot/program/xr_xir_native_projection.c"
#include "app/toolchain/xtc_xir_invocation.c"
#include "app/toolchain/xtc_xir_native_operation.c"
static unsigned compare_calls, compare_fail;
static DWORD compare_error;
static int WINAPI test_compare(LPCWCH a,int n,LPCWCH b,int m,BOOL insensitive) {
    if (++compare_calls == compare_fail) { SetLastError(compare_error); return 0; }
    return CompareStringOrdinal(a,n,b,m,insensitive);
}
#define CompareStringOrdinal test_compare
#include "app/toolchain/xtc_xir_native_admission.c"
#undef CompareStringOrdinal
#undef MultiByteToWideChar
#endif
static const XtcXirNativeOperationLimits operation_limits = {{{4*1024*1024,32768,4096},64*1024*1024,4096},30000,4*1024*1024};
static void close_operation(XtcXirNativeOperation **owner) {
    for (unsigned i=0; *owner && i<65536; ++i) {
        XtcXirNativeOperationStatus status=xtc_xir_native_operation_close(owner,1);
        CHECK(status==XTC_XIR_NATIVE_OK || status==XTC_XIR_NATIVE_PENDING);
    }
    CHECK(!*owner);
}
static XrXirNativeProjection *real_projection(XrCompileResources *r,const char *directory,const char *source,const char *stdlib) {
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    XrCompilerSession *session=NULL; CHECK(xr_compile_session_new(r,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrXirSourceProductRequest request={{session,source,&authority,&context,stdlib,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL; CHECK(xr_xir_compile_source_product_build(&request,&product,NULL)==XR_XIR_OK);
    xr_compile_session_free(session);
    XrXirNativeProjectionRequest prepare={product,"admission_source",16777216};
    XrXirNativeProjection *result=NULL; CHECK(xr_compile_native_projection_prepare(&prepare,&result,NULL)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product); return result;
}
static XrXirRuntimeSdk *real_sdk(XrCompileResources *r,const char *root) {
    char path[32768]; CHECK(snprintf(path,sizeof(path),"%s/sdk_manifest.json",root)>0);
    FILE *f=fopen(path,"rb"); CHECK(f && !fseek(f,0,SEEK_END)); long n=ftell(f); CHECK(n>0); rewind(f);
    void *bytes=malloc((size_t)n); CHECK(bytes && fread(bytes,1,(size_t)n,f)==(size_t)n && !fclose(f));
    XrXirRuntimeSdk *sdk=NULL; XrXirRuntimeSdkRequest request={root,bytes,(size_t)n,r};
    CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK); free(bytes); return sdk;
}
static void json_text(FILE *f,const char *s) {
    fputc('"',f); for (;*s;++s) { unsigned char c=(unsigned char)*s;
        if (c=='"' || c=='\\') {fputc('\\',f);fputc(c,f);} else if(c<32) fprintf(f,"\\u%04x",c); else fputc(c,f);
    } fputc('"',f);
}
static void json_hash(FILE *f,const uint8_t *p) { fputc('"',f);for(unsigned i=0;i<32;++i)fprintf(f,"%02x",p[i]);fputc('"',f); }
static void json_provider(FILE *f,const XrXirInvocationProviderImageFacts *p) {
    fprintf(f,"{\"path\":");json_text(f,p->path);fprintf(f,",\"length\":%llu,\"digest\":",(unsigned long long)p->length);json_hash(f,p->digest);
    fprintf(f,",\"version\":[");for(unsigned i=0;i<8;++i)fprintf(f,"%s%u",i?",":"",i<4?p->version.file[i]:p->version.product[i-4]);
    fprintf(f,"],\"file_text\":");json_text(f,p->version.file_text);fputc('}',f);
}
static void facts_dump(const char *path,const XtcXirNativeOperation *op,const XrXirNativeArtifact *artifact) {
    FILE *f=fopen(path,"wb");CHECK(f);
    const XrXirInvocationFacts *facts=xtc_xir_native_operation_facts(op);
    const XrXirInvocationProviderFacts *provider=xtc_xir_native_operation_provider(op);
    const XrXirNativeArtifactView *view=xr_compile_native_artifact_view(artifact);
    fprintf(f,"{\"commands\":[");
    for(unsigned stage=0;stage<3;++stage) {
        const XrProcessView *c=xtc_xir_native_operation_command(op,(XrXirInvocationStage)stage);
        fprintf(f,"%s{\"executable\":",stage?",":"");json_text(f,c->executable);fprintf(f,",\"cwd\":");json_text(f,c->cwd);
        fprintf(f,",\"argv\":[");for(size_t i=0;i<c->argc;++i){if(i)fputc(',',f);json_text(f,c->argv[i]);}
        fprintf(f,"],\"env\":[");for(size_t i=0;i<c->env_count;++i){if(i)fputc(',',f);fputc('[',f);json_text(f,c->env_keys[i]);fputc(',',f);json_text(f,c->env_values[i]);fputc(']',f);}
        fprintf(f,"],\"timeout\":%u,\"limit\":%zu,\"image\":%u,\"completion\":%u}",c->timeout_ms,c->output_limit,(unsigned)c->image_mode,(unsigned)c->completion_policy);
    }
    fprintf(f,"],\"files\":[");for(uint32_t i=0;i<facts->file_count;++i){const XrXirInvocationFile *r=xtc_xir_native_operation_file(op,i);
        fprintf(f,"%s[%u,%u,",i?",":"",(unsigned)r->stage,(unsigned)r->kind);json_text(f,r->path);fprintf(f,",%llu,",(unsigned long long)r->length);json_hash(f,r->digest);fputc(']',f);}
    fprintf(f,"],\"compiler\":");json_provider(f,&provider->compiler);fprintf(f,",\"linker\":");json_provider(f,&provider->linker);
    const XrXirRuntimeSdkFacts *s=&facts->sdk;
    uint32_t words[]={s->schema,s->wire,s->semantic,s->value_abi,s->call_abi,s->program_abi,s->architecture,s->object_format,s->hosted,
        s->c_dialect,s->crt,s->sanitizers,s->allocator,s->assertions,s->build_provider,s->abi_recipe_version,s->closure_recipe_version};
    fprintf(f,",\"sdk\":[");for(unsigned i=0;i<17;++i)fprintf(f,"%s%u",i?",":"",words[i]);fprintf(f,"],\"sdk_id\":");json_hash(f,s->identity);
    fprintf(f,",\"prefix\":");json_text(f,facts->projection.prefix);
    fprintf(f,",\"input_words\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]",view->input.schema_version,view->input.checked_schema,
        view->input.checked_contract,view->input.value_abi,view->input.call_abi,view->input.program_abi,view->input.architecture,
        view->input.entry,view->input.function_count,view->input.module_count);
    const XrFingerprint *hashes[]={&view->input.source_checked_id,&view->input.closed_checked_id,&view->input.lowered_layout_id,
        &view->input.codegen_policy_id,&view->input.generated_digest,&view->input.toolchain.provider_version_id,
        &view->input.toolchain.target_triple_id,&view->input.toolchain.codegen_options_id,&view->input.toolchain.sysroot_id,
        &view->input.toolchain.runtime_sdk_id,&view->input.toolchain.target_profile_id,&view->input.toolchain.id,&view->input.id,
        &view->native_digest,&view->id};
    fprintf(f,",\"hashes\":[");for(unsigned i=0;i<15;++i){if(i)fputc(',',f);json_hash(f,hashes[i]->bytes);}fprintf(f,"]}\n");CHECK(!fclose(f));
}
static void save_artifact(const char *path,const XrXirNativeArtifact *artifact) {
    const XrXirNativeArtifactView *view=xr_compile_native_artifact_view(artifact);FILE *f=fopen(path,"wb");CHECK(f);
    CHECK(fwrite(view->bytes,1,view->size,f)==view->size && !fclose(f));
}
#ifdef ADMISSION_INJECTED
#include "admission_fixture.inc.h"
#endif
static int run_test(int argc,char **argv) {
#ifdef ADMISSION_INJECTED
    if(argc==5 && !strcmp(argv[1],"unit")) return admission_unit(argv[2],argv[3],argv[4]);
#endif
    CHECK(argc==21 && !strcmp(argv[1],"--seal"));
    const XrCompileResourceLimits limits={64*1024*1024,8*1024*1024,128000000};
    XrCompileResources *r=sdk_ledger(&limits);
    XrXirNativeProjection *projection=real_projection(r,argv[2],argv[3],argv[4]);
    XrXirRuntimeSdk *sdk=real_sdk(r,argv[5]);
    XrXirInvocationLibrary libraries[5];for(unsigned i=0;i<5;++i)libraries[i]=(XrXirInvocationLibrary){argv[9+i],i==4?XR_XIR_INVOCATION_SYSTEM:XR_XIR_INVOCATION_CRT};
    XtcXirNativeOperationRequest request={projection,sdk,argv[6],argv[7],{argv[14],argv[15],argv[16],argv[17],argv[18]},libraries,5,NULL,NULL};
    XtcXirWorkspaceRequest ws={argv[8],{32767,32,65536}};XtcXirNativeOperation *op=NULL;
    CHECK(xtc_xir_native_operation_new(r,&ws,&operation_limits,&op)==XTC_XIR_NATIVE_OK);
    CHECK(!xtc_xir_native_operation_command(op,XR_XIR_INVOCATION_GENERATED));
    XrXirNativeArtifact *artifact=NULL;XtcXirNativeAdmissionDiagnostic d;
    CHECK(xtc_xir_native_admit(op,projection,sdk,UINT64_MAX,&artifact,&d)==XTC_XIR_ADMISSION_INVALID && !artifact);
    XrCompileResourceStats entry;
    CHECK(xr_compile_resources_stats(r,&entry)==XR_COMPILE_RESOURCE_OK);
    printf("whole operation entry allocated=%llu live=%llu peak=%llu work=%llu allocations=%llu\n",
        (unsigned long long)entry.allocated_bytes,(unsigned long long)entry.live_bytes,
        (unsigned long long)entry.peak_bytes,(unsigned long long)entry.work,
        (unsigned long long)entry.allocation_count);fflush(stdout);
    XtcXirNativeOperationStatus status=xtc_xir_native_operation_run(op,&request);
    const XtcXirNativeOperationDiagnostic *od=xtc_xir_native_operation_diagnostic(op);
    printf("operation status=%u stage=%u pass=%u code=%d exit=%d inner_domain=%u\n",(unsigned)status,(unsigned)od->invocation.stage,
        (unsigned)od->invocation.pass,od->invocation.code,od->invocation.exit_code,(unsigned)od->invocation.domain);fflush(stdout);
    if(status!=XTC_XIR_NATIVE_OK) {
        XrCompileResourceStats failed;
        CHECK(xr_compile_resources_stats(r,&failed)==XR_COMPILE_RESOURCE_OK && !artifact);
        printf("whole failed domain=%u os=%u allocated=%llu live=%llu peak=%llu work=%llu allocations=%llu limits=%llu/%llu/%llu\n",
            (unsigned)od->domain,od->os_error,(unsigned long long)failed.allocated_bytes,
            (unsigned long long)failed.live_bytes,(unsigned long long)failed.peak_bytes,
            (unsigned long long)failed.work,(unsigned long long)failed.allocation_count,
            (unsigned long long)limits.allocated_bytes,(unsigned long long)limits.live_bytes,
            (unsigned long long)limits.work);fflush(stdout);
        close_operation(&op);xr_compile_native_projection_owner_free(projection);
        xr_xir_runtime_sdk_free(sdk);xr_compile_resources_release(r);
        CHECK(!runtime_live && !runtime_bytes);
        puts("failed seal: no artifact published; physical=0");return 1;
    }
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(r,&before)==XR_COMPILE_RESOURCE_OK);
    XtcXirNativeAdmissionStatus admitted=xtc_xir_native_admit(op,projection,sdk,64*1024*1024,&artifact,&d);
    printf("admission status=%u domain=%u code=%d stage=%u\n",(unsigned)admitted,(unsigned)d.domain,d.code,(unsigned)d.stage);fflush(stdout);
    CHECK(admitted==XTC_XIR_ADMISSION_OK && artifact);CHECK(xr_compile_resources_stats(r,&after)==XR_COMPILE_RESOURCE_OK);
    facts_dump(argv[20],op,artifact);
    XrXirNativeInput expected=xr_compile_native_artifact_view(artifact)->input;
    xr_compile_native_projection_owner_free(projection);xr_xir_runtime_sdk_free(sdk);xr_compile_resources_release(r);
    close_operation(&op);
    CHECK(xr_compile_native_artifact_verify(artifact,&expected,64*1024*1024)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(r,&after)==XR_COMPILE_RESOURCE_OK);
    printf("whole seal allocated=%llu live=%llu peak=%llu work=%llu allocations=%llu limits=%llu/%llu/%llu\n",
        (unsigned long long)after.allocated_bytes,(unsigned long long)after.live_bytes,
        (unsigned long long)after.peak_bytes,(unsigned long long)after.work,
        (unsigned long long)after.allocation_count,(unsigned long long)limits.allocated_bytes,
        (unsigned long long)limits.live_bytes,(unsigned long long)limits.work);
    save_artifact(argv[19],artifact);xr_compile_native_artifact_free(artifact);CHECK(!runtime_live && !runtime_bytes);
    printf("real six calls + admission + producer death + close1 + artifact lifetime PASS; new allocations=%llu bytes=%llu work=%llu physical=0\n",
        (unsigned long long)(after.allocation_count-before.allocation_count),(unsigned long long)(after.allocated_bytes-before.allocated_bytes),
        (unsigned long long)(after.work-before.work));return 0;
}
typedef struct AdmissionThread {int argc;char **argv;int result;} AdmissionThread;
static DWORD WINAPI admission_thread(void *p){AdmissionThread *t=p;t->result=run_test(t->argc,t->argv);return 0;}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--control-child"))return 0;
    for(unsigned i=0;i<2;++i){UCHAR data[16];CHECK(BCryptGenRandom(NULL,data,sizeof(data),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);}
    wchar_t self[32768],command[32768];CHECK(GetModuleFileNameW(NULL,self,32768));
    for(unsigned i=0;i<2;++i){CHECK(swprintf(command,32768,L"\"%ls\" --control-child",self)>0);STARTUPINFOW s={0};s.cb=sizeof(s);PROCESS_INFORMATION p={0};
        CHECK(CreateProcessW(self,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&s,&p));CHECK(WaitForSingleObject(p.hProcess,30000)==WAIT_OBJECT_0);CHECK(CloseHandle(p.hThread)&&CloseHandle(p.hProcess));}
    /* The raw first CreatePipe retains a NamedPipe root handle on this host.
     * Establish that OS baseline visibly, without invoking an Xray owner. */
    DWORD pipe_before,pipe_first=0,pipe_last=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&pipe_before));
    for(unsigned i=0;i<2;++i){HANDLE read,write;SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
        CHECK(CreatePipe(&read,&write,&security,0));CHECK(CloseHandle(read)&&CloseHandle(write));
        CHECK(GetProcessHandleCount(GetCurrentProcess(),&pipe_last));if(!i)pipe_first=pipe_last;}
    printf("raw CreatePipe controls before=%lu first=%lu repeated=%lu\n",pipe_before,pipe_first,pipe_last);
    CHECK(pipe_first==pipe_last);
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));AdmissionThread t={argc,argv,1};
    HANDLE h=CreateThread(NULL,0,admission_thread,&t,0,NULL);CHECK(h && WaitForSingleObject(h,INFINITE)==WAIT_OBJECT_0 && CloseHandle(h));
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));printf("thread-exit handles=%lu/%lu\n",before,after);CHECK(before==after);return t.result;
}
