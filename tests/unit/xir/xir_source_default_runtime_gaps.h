/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_default_runtime_gaps.h - Ordered defaults and retained owned results
 *
 * KEY CONCEPT:
 *   Receiver and arguments run once; default effects precede constructor bodies.
 */
#ifndef XIR_SOURCE_DEFAULT_RUNTIME_GAPS_H
#define XIR_SOURCE_DEFAULT_RUNTIME_GAPS_H
#include "../test_win_compat.h"
static void default_gap_write(const char *path,const char *text) {
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(text,1,strlen(text),file)==strlen(text) && fclose(file)==0);
}
static XrXirArtifact *default_gap_checked(const char *root,const char *path) {
    static const char source[]=
        "const trace=Atomic(0)\n"
        "fn step(n:i64)->i64{const prior=trace.load();trace.fetchAdd(prior*9+n);return n}\n"
        "struct Receiver{value:i64;read(first:i64,second:i64=step(3),third:i64=step(4))->i64{return trace.load()}}\n"
        "fn receiver()->Receiver{step(1);return Receiver{value:41}}\n"
        "fn text()->string{return \"default\"+\"-owned\"}\n"
        "fn take(value:string=text())->string{return value}\n"
        "fn delayed()->i64{Coro.yield();return 41}\n"
        "final class Box{value:i64;constructor(value:i64=delayed()){this.value=value}}\n"
        "export fn order()->i64{return receiver().read(step(2))}\n"
        "export fn retained()->string{return take()}\n"
        "export fn construct()->i64{return Box().value}\n";
    default_gap_write(path,source);
    XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,path,&authority,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"default gap %u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    XrXirArtifact *owned=result.checked;result.checked=NULL;xr_xir_source_result_free(&result);
    char manifest[8192];CHECK(snprintf(manifest,sizeof(manifest),"%s/xray.toml",root)>0);
    default_gap_write(manifest,"[declarations]\nversion=1\n[[declarations.function]]\nmodule=\"root.xr\"\nname=\"construct\"\nno_suspend=true\n");
    CHECK(xr_xir_source_check(&request,&result,&diagnostic)==XR_XIR_BAD_TYPE);
    CHECK(!result.checked && strstr(diagnostic.message,"declared no_suspend"));
    xr_xir_source_result_free(&result);xr_compiler_session_delete(session);
    CHECK(xr_test_unlink(manifest)==0);default_gap_write(path,"const poisoned=0\n");
    return owned;
}
static void default_gap_execute(XrXirArtifact *checked) {
    XrXirArtifact *specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_specialize(checked,NULL,&specialized,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
    CHECK(xr_xir_artifact_verify(specialized,NULL,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_lower(specialized,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(specialized);
    const XrXirModule *module=xr_xir_artifact_module(lowered);
    const char *names[]={"order","retained","construct"};uint32_t entries[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX};
    for(uint32_t f=0;f<module->function_count;++f)
        for(uint32_t e=0;e<3;++e)
            if(module->functions[f].name_length==strlen(names[e]) &&
                !memcmp(module->functions[f].name,names[e],strlen(names[e])))entries[e]=f;
    CHECK(entries[0]!=UINT32_MAX && entries[1]!=UINT32_MAX && entries[2]!=UINT32_MAX);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
    XrXirValue held[2]={{0},{0}};
    for(uint32_t n=0;n<2;++n){
        XrXirInstanceConfig config=xr_xir_instance_defaults();XrXirInstance *instance=NULL;
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        for(uint32_t e=0;e<3;++e){
            CHECK(xr_xir_instance_start(instance,entries[e],NULL,0)==XR_XIR_CALL_READY);
            XrXirInstanceResult run=xr_xir_instance_poll(instance);uint32_t yields=0;
            while(run.outcome.status==XR_XIR_CALL_SUSPENDED){
                CHECK(e==2 && ++yields==1);
                CHECK(xr_xir_instance_resume(instance,run.epoch,run.outcome.wake)==XR_XIR_CALL_READY);
                run=xr_xir_instance_poll(instance);
            }
            CHECK(run.outcome.status==XR_XIR_CALL_RETURNED && yields==(e==2 ? 1u : 0u));
            XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
            if(e==1)held[n]=value;
            else{CHECK(value.type==XR_XIR_I64 && value.payload==(e==0 ? 1234u : 41u));xr_xir_value_drop(&value);}
        }
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_program_drop(program);
    for(uint32_t n=0;n<2;++n){
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&held[n],&bytes,&length));
        CHECK(length==13 && !memcmp(bytes,"default-owned",13));xr_xir_value_drop(&held[n]);
    }
}
static void source_default_runtime_gaps(void) {
    CHECK(!runtime_live && !runtime_bytes);
    char directory[64]="xir-default-gaps-XXXXXX",root[4096],path[8192];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,root,sizeof(root)));
    CHECK(snprintf(path,sizeof(path),"%s/root.xr",root)>0);
    default_gap_execute(default_gap_checked(root,path));
    CHECK(xr_test_unlink(path)==0 && xr_test_rmdir(directory)==0);
    CHECK(!runtime_live && !runtime_bytes);
}
#endif // XIR_SOURCE_DEFAULT_RUNTIME_GAPS_H
