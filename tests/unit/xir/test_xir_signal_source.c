/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_signal_source.c - Cold Source bindings and real isolated VM delivery
 */
#include "root_signal_runtime.h"
static uint32_t root_signal_named(const XrXirModule *module,const char *name) {
    for (uint32_t i=0;i<module->function_count;++i)
        if (module->functions[i].name_length==strlen(name) && !memcmp(module->functions[i].name,name,strlen(name))) return i;
    CHECK(false);return UINT32_MAX;
}
static XrXirArtifact *root_signal_code(const XrXirCompileContext *context,uint32_t ids[6]) {
    const char *root=XR_ROOT_SIGNAL_STDLIB;char path[2048];
    CHECK(snprintf(path,sizeof(path),"%s/sys/sys.xr",root)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"sys",root};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={session,path,&authority,context,root,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText text={"sys/sys.xr",root_signal_text,sizeof(root_signal_text)-1};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"signal Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK);xr_compile_session_free(session);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const char *names[]={"parent","guarded","owned","withHandler","__onSignal","read"};
    for (unsigned i=0;i<6;++i) ids[i]=root_signal_named(xr_xir_compile_artifact_module(code),names[i]);
    return code;
}
static void root_signal_emit(const XrXirArtifact *code,const uint32_t ids[6],const char *directory,bool write) {
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(code,"root_signal",16777216,&c)==XR_XIR_OK);
    char file[2048],tail[256];CHECK(snprintf(file,sizeof(file),"%s/root_signal.c",directory)>0);
    int count=snprintf(tail,sizeof(tail),"\nconst uint32_t root_signal_ids[6]={%u,%u,%u,%u,%u,%u};\n",ids[0],ids[1],ids[2],ids[3],ids[4],ids[5]);
    CHECK(count>0 && (size_t)count<sizeof(tail));FILE *out=fopen(file,write?"wb":"rb");CHECK(out);
    if (write) CHECK(fwrite(c.text,1,c.length,out)==c.length && fwrite(tail,1,(size_t)count,out)==(size_t)count);
    else {for(size_t i=0;i<c.length;++i)CHECK(fgetc(out)==(unsigned char)c.text[i]);for(int i=0;i<count;++i)CHECK(fgetc(out)==(unsigned char)tail[i]);CHECK(fgetc(out)==EOF&&!ferror(out));}
    CHECK(!fclose(out));xr_xir_compile_c_source_free(&c);
}
int main(int argc,char **argv) {
    (void)sizeof(root_signal_text);
    bool emit=argc==3 && !strcmp(argv[1],"--emit");
    bool console=argc==2 && !strcmp(argv[1],"--private-console"),detached=argc==2 && !strcmp(argv[1],"--detached");
    CHECK(emit || console || detached || argc==1);
#if defined(XR_OS_WINDOWS)
    if (!emit && !console && !detached) {
        root_signal_isolate(L"--detached",DETACHED_PROCESS);root_signal_isolate(L"--private-console",CREATE_NEW_CONSOLE);
        puts("42");return 0;
    }
    if(console)CHECK(SetConsoleCtrlHandler(root_signal_previous,TRUE));
#endif
    XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;limits.profile_allocations=true;
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    uint32_t ids[6];XrXirArtifact *code=root_signal_code(&context,ids);root_signal_emit(code,ids,emit?argv[2]:XR_ROOT_SIGNAL_GENERATED,emit);
    if (emit) xr_xir_compile_artifact_free(code);
    else {
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK && !code);
        if(detached)root_signal_detached(program,ids);
        else {root_signal_cases(program,ids);root_signal_owner_cases(program,ids,&context);root_signal_stop_cases(program,ids);root_signal_admission_cases(program,ids);}
        xr_xir_compile_program_drop(program);
    }
    CHECK(!runtime_live && !runtime_bytes && xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
    xr_compile_resources_release(context.resources);
#if defined(XR_OS_WINDOWS)
    if(console)CHECK(SetConsoleCtrlHandler(root_signal_previous,FALSE));
#endif
    return 0;
}
