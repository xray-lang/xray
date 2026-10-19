/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_enum_equal.c - Original enum consumers and owned shared equality
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_nullable.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_assert_compile_owner.h"
#include "xir_runtime_allocations.h"
static const char *const original_cases[]={
    "export enum ProbeError { Bad, Worse }\nexport fn query(flag: bool) -> ProbeError? { if(flag) {return ProbeError.Bad}; return null }\nexport fn probe() -> bool { return query(true) != null }\n",
    "import { query } from \"./helper\"\nexport fn probe() -> bool { return query(true) != null }\n",
    "import { query } from \"./helper\"\nexport fn probe() -> bool { return query(false) == null }\n",
    "export enum ProbeError { Bad, Worse }\nexport fn query(flag: bool) -> ProbeError? { if(flag) {return ProbeError.Bad}; return null }\nexport fn probe() -> bool {\n    var e = query(true)\n    return e != null && e != ProbeError.Worse\n}\n",
    "export enum ProbeError { Bad, Worse }\nexport fn query(flag: bool) -> ProbeError? { if(flag) {return ProbeError.Bad}; return null }\nexport fn probe() -> bool { return query(true) == ProbeError.Bad }\n",
    "export enum ProbeError { Bad, Worse }\nexport fn query(flag: bool) -> ProbeError? { if(flag) {return ProbeError.Bad}; return null }\nclass Holder {\n    kind: ProbeError\n    init() { this.kind = ProbeError.Worse }\n}\nexport fn probe() -> bool { return Holder().kind == ProbeError.Worse }\n",
};
static const char helper_source[]="export enum ProbeError { Bad, Worse }\nexport fn query(flag: bool) -> ProbeError? { if(flag) {return ProbeError.Bad}; return null }\n";
static const char control_source[]="export enum U { A, B }\nexport enum Phantom<T> { A, B }\nfn same<T:Equal>(a:T,b:T)->bool{return a==b}\nexport fn direct()->bool{return U.A==U.A && U.A!=U.B}\nexport fn arrays()->bool{var a:Array<U?>=[U.A,null,U.B];var b:Array<U?>=[U.A,null,U.B];return a==b}\nexport fn relay()->bool{return same(U.A,U.A)}\nexport fn phantom()->bool{return Phantom<i64>.A==Phantom<i64>.A}\nexport fn run()->bool{return direct() && arrays() && relay() && phantom()}\nexport fn escape()->U{return U.A}\nexport fn escapeArray()->Array<U>{return [U.A,U.B]}\nexport fn escapeNullable()->U?{return U.B}\n";
static void write_file(const char *path,const char *text) {
    FILE *f=fopen(path,"wb");CHECK(f);
    size_t n=strlen(text);CHECK(fwrite(text,1,n,f)==n);CHECK(!fclose(f));
}
static void case_path(char out[2048],const char *root,unsigned index,const char *name) {
    int n=snprintf(out,2048,"%s/%u/%s",root,index,name);CHECK(n>0 && n<2048);
}
static void prepare(const char *root) {
    for (unsigned i=0;i<6;++i) {
        char path[2048];case_path(path,root,i,"case.xr");write_file(path,original_cases[i]);
        case_path(path,root,i,"helper.xr");write_file(path,helper_source);
        case_path(path,root,i,"root.xr");
        write_file(path,"import { probe } from \"./case\"\n"
            "export fn bridge()->bool{return probe()}\n"
            "export fn run()->bool{return bridge()}\n");
    }
    char path[2048];case_path(path,root,6,"root.xr");write_file(path,control_source);
}
static uint32_t named(const XrXirModule *m,const char *name,bool root) {
    uint32_t found=UINT32_MAX;
    for (uint32_t i=0;i<m->function_count;++i) {
        if (root && m->declarations->functions[i].module!=m->declarations->root_module) continue;
        if (m->functions[i].name_length==strlen(name) && !memcmp(m->functions[i].name,name,strlen(name))) {
            CHECK(found==UINT32_MAX);found=i;
        }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static XrXirArtifact *compile_case(const char *root,unsigned index,uint8_t identity[32]) {
    char path[2048],directory[2048];case_path(path,root,index,"root.xr");case_path(directory,root,index,"");
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(assert_compile_context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrXirSourceRequest request={session,path,&authority,assert_compile_context,XR_ENUM_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"case%u Source %u: %s\n",index,status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked);
    XrXirCheckedPacket packet={0};XrXirArtifact *read=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(assert_compile_context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    CHECK(xr_xir_compile_specialize(read,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    xr_sha256(packet.bytes,packet.length,identity);xr_xir_compile_checked_packet_free(&packet);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    return lowered;
}
static XrXirValue run_entry(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    if(polled.outcome.status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"entry%u status%u\n",entry,polled.outcome.status);
    CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void vm_case(const char *root,unsigned index) {
    assert_compile_begin();uint8_t identity[32];XrXirArtifact *lowered=compile_case(root,index,identity);
    const XrXirModule *m=xr_xir_compile_artifact_module(lowered);uint32_t run=named(m,"run",true),escape[3]={0};
    uint32_t controls[4]={0};if(index==6){escape[0]=named(m,"escape",true);escape[1]=named(m,"escapeArray",true);escape[2]=named(m,"escapeNullable",true);const char *names[]={"direct","arrays","relay","phantom"};for(unsigned k=0;k<4;++k)controls[k]=named(m,names[k],true);}
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstance *instances[2]={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    for(unsigned i=0;i<2;++i)CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    XrXirValue held[2][3]={{{0}}};
    for(unsigned i=0;i<2;++i){if(index==6)for(unsigned k=0;k<4;++k){XrXirValue control=run_entry(instances[i],controls[k]);CHECK(control.type==XR_XIR_BOOL && control.payload==1);xr_xir_value_drop(&control);printf("enum equality control%u true\n",k);}XrXirValue result=run_entry(instances[i],run);CHECK(result.type==XR_XIR_BOOL && result.payload==1);xr_xir_value_drop(&result);
        if(index==6)for(unsigned h=0;h<3;++h)held[i][h]=run_entry(instances[i],escape[h]);
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
    if(index==6)for(unsigned i=0;i<2;++i){XrXirEnumBorrow b={0};CHECK(xr_xir_enum_borrow(&held[i][0],&b)==XR_XIR_VALUE_OK);
        CHECK(b.name.length==1 && b.name.bytes[0]=='U' && b.member.length==1 && b.member.bytes[0]=='A' && !b.field_count);
        CHECK(xr_xir_value_valid(&held[i][1]));bool some=false;const XrXirValue *payload=NULL;
        CHECK(xr_xir_nullable_view(&held[i][2],&some,&payload) && some && payload);
        CHECK(xr_xir_enum_borrow(payload,&b)==XR_XIR_VALUE_OK && b.member.length==1 && b.member.bytes[0]=='B');
        for(unsigned h=0;h<3;++h)xr_xir_value_drop(&held[i][h]);
    }
    CHECK(!runtime_live && !runtime_bytes);CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes());
    assert_compile_end();printf("enum equality VM case%u true; two Instances, Program-first drop; physical=0/0\n",index);
}
static void generate_case(const char *root,unsigned index) {
    assert_compile_begin();uint8_t identity[32];XrXirArtifact *lowered=compile_case(root,index,identity);
    const XrXirModule *m=xr_xir_compile_artifact_module(lowered);uint32_t entries[]={named(m,"run",true),named(m,"bridge",true),named(m,"probe",false)};
    char prefix[64],path[2048];CHECK(snprintf(prefix,sizeof(prefix),"enum_equal_%u",index)>0);
    XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,prefix,16777216,&c)==XR_XIR_OK);
    case_path(path,root,index,"native.c");FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(c.text,1,c.length,f)==c.length);
    CHECK(fprintf(f,"\nconst uint32_t %s_indices[3]={%uu,%uu,%uu};\n",prefix,entries[0],entries[1],entries[2])>0);CHECK(!fclose(f));
    xr_xir_compile_c_source_free(&c);xr_xir_compile_artifact_free(lowered);CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes());
    assert_compile_end();printf("enum equality actual native C generated case%u\n",index);
}
static void ctfe_cases(const char *root) {
    assert_compile_begin();uint8_t identity[32];XrXirArtifact *lowered=compile_case(root,6,identity);
    const XrXirModule *m=xr_xir_compile_artifact_module(lowered);const char *names[]={"direct","arrays","relay"};
    for(unsigned i=0;i<3;++i){XrXirCtfeRequest request={0};request.artifact=lowered;request.function=named(m,names[i],true);
        request.target=(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};memcpy(request.checked_identity,identity,32);
        request.limits=(XrXirCtfeLimits){XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,1};XrXirValue result={0};
        XrXirCtfeCallRequest calls={request,64};
        XrXirCtfeStatus status=i==2?xr_xir_compile_ctfe_calls(&calls,&result):xr_xir_compile_ctfe_leaf(&request,&result);
        if(status!=XR_XIR_CTFE_OK)fprintf(stderr,"CTFE %s status%u\n",names[i],status);
        CHECK(status==XR_XIR_CTFE_OK && result.type==XR_XIR_BOOL && result.payload==1);xr_xir_value_drop(&result);
        printf("enum equality CTFE %s true on original ledger\n",names[i]);
    }
    xr_xir_compile_artifact_free(lowered);CHECK(!runtime_live && !runtime_bytes);CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes());assert_compile_end();
}
int main(int argc,char **argv) {
    if(argc!=3 || (strcmp(argv[1],"--vm") && strcmp(argv[1],"--generate") && strcmp(argv[1],"--ctfe")))return 1;
    prepare(argv[2]);
    if(!strcmp(argv[1],"--ctfe"))ctfe_cases(argv[2]);
    else for(unsigned i=0;i<6;++i){if(!strcmp(argv[1],"--generate"))generate_case(argv[2],i);else vm_case(argv[2],i);}
    if(!strcmp(argv[1],"--vm"))vm_case(argv[2],6);
    return 0;
}
