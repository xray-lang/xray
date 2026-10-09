/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_go_consumption.c - Root permission and independent GO obligations
 */
#include "base/xmalloc.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);} } while(0)
#include "xir_effect_analysis_owner.h"
#include "xir/xxir_effects.c"
#include "xir_root_effects_builtin.h"
#include "xir_root_effects_oracles.h"
#include "xir_root_go_builtin.h"
#include "xir_root_go_resources.h"
static const char *go_required="GO target requires the current instance root execution";
static const char *go_unresolved="GO target lacks proof for worker execution";

static XrXirArtifact *go_source(const char *file,bool query,XrXirStatus expected,const char *message) {
    const char *base=query?XR_ROOT_QUERY_FIXTURES:XR_ROOT_GO_FIXTURES;
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",base,file)>0);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(effect_context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,base};
    XrXirSourceRequest request={session,path,&authority,&effect_context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};char *failure=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    if(status!=expected||(message&&strcmp(message,diagnostic.message)))
        fprintf(stderr,"GO source %s status%u expected%u %d:%d %s\n",file,status,expected,
            diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==expected);if(message)CHECK(!strcmp(message,diagnostic.message));
    else if(expected!=XR_XIR_OK)CHECK(strcmp(go_required,diagnostic.message)&&strcmp(go_unresolved,diagnostic.message));
    if(expected!=XR_XIR_OK)CHECK(!result.checked&&!result.snapshot);
    else CHECK(result.checked&&!failure);
    XrXirArtifact *checked=result.checked;result.checked=NULL;
    xr_compile_resources_free(failure);xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    return checked;
}
static XrXirArtifact *go_replay(XrXirArtifact *checked) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    effect_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);return checked;
}
static XrXirArtifact *go_lower(XrXirArtifact *checked) {
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    effect_artifact_free(closed);return lowered;
}
static void go_literal_table(const XrXirArtifact *artifact,bool lowered) {
    const char *safe[]={"main","pure","selfPure","pureA","pureB","constRead","constAtomicRead",
        "atomicParameter","local","localCell","localBox","defaultOwner","reference","worker","emptyUse"};
    const char *bad[]={"mutableRead","mutableAtomicContainerRead","projection","constBoxRead","atomicSnapshot",
        "relay","rootA","rootB","rootFail","invoke","defaultUse","cleanupOwner","indirect","indirectInvoke",
        "mixed","mixedRelay","goParent","left","right","diamond"};
    XrXirEffects *effects=NULL;CHECK(effect_analyze(artifact,&effects)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    for(size_t i=0;i<sizeof(safe)/sizeof(safe[0]);++i)CHECK(xr_xir_effects_go_safe(effects,root_find(module,safe[i]))==XR_XIR_OK);
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)CHECK(xr_xir_effects_go_safe(effects,root_find(module,bad[i]))==XR_XIR_BAD_TYPE);
    CHECK(xr_xir_effects_go_safe(effects,root_find(module,"instantiateRequirement"))==(lowered?XR_XIR_OK:XR_XIR_BAD_TYPE));
    if(!lowered)CHECK(xr_xir_effects_go_safe(effects,root_find(module,"requirement"))==XR_XIR_BAD_TYPE);
    for(uint32_t m=0;m<module->declarations->module_count;++m)
        CHECK(xr_xir_effects_go_safe(effects,module->declarations->modules[m].initializer)==XR_XIR_BAD_TYPE);
    root_fact(effects,root_find(module,"mixed"),true,true);root_paths(module,effects);
    effect_summary_free(effects);
}
static void go_proof(const XrXirArtifact *lowered) {
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);CHECK(module->function_count<=128);
    XrXirVmBinding bindings[128];XrXirCallEntry entries[128];
    for(uint32_t f=0;f<module->function_count;++f)CHECK(xr_xir_compile_vm_bind(lowered,f,&bindings[f],&entries[f])==XR_XIR_OK);
    XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,*xr_xir_compile_artifact_target(lowered),
        entries,module->function_count,module->declarations,{0},module->types,xr_xir_compile_program_proof(lowered)};
    XrXirProgramPermissions *authority=NULL;EffectMark mark=effect_mark();
    CHECK(xr_xir_compile_program_proof_verify(&effect_context,&spec,&spec.proof,&authority)==XR_XIR_OK&&authority);
    go_authority_oracles(module,authority);xr_compile_resources_free(authority);effect_mark_check(mark);
    go_authority_failures(&spec);
}
static void go_query_cases(void) {
    XrXirArtifact *checked=go_replay(go_source("root.xr",true,XR_XIR_OK,NULL));
    root_source_oracles(checked,false);go_literal_table(checked,false);
    effect_budget_boundaries(checked);effect_analysis_work_cut(checked,1,false);effect_analysis_work_cut(checked,0,true);
    XrXirArtifact *lowered=go_lower(checked);root_source_oracles(lowered,true);go_literal_table(lowered,true);
    go_proof(lowered);effect_artifact_free(lowered);effect_artifact_free(checked);CHECK(effect_balanced());
    XrXirArtifact *builtin=root_builtin(0);effect_artifact_free(builtin);
    builtin=root_group();effect_artifact_free(builtin);root_permutation();
}
static void go_shape_cases(void) {
    static const struct {bool root,unknown;XrXirStatus status;XrXirDiagnosticReason reason;} expected[]={
        {false,false,XR_XIR_OK,XR_XIR_DIAGNOSTIC_NONE},
        {false,false,XR_XIR_BAD_TYPE,XR_XIR_DIAGNOSTIC_NONE},
        {true,false,XR_XIR_BAD_TYPE,XR_XIR_DIAGNOSTIC_GO_ROOT_REQUIRED},
        {false,true,XR_XIR_BAD_TYPE,XR_XIR_DIAGNOSTIC_GO_ROOT_UNRESOLVED},
        {true,true,XR_XIR_BAD_TYPE,XR_XIR_DIAGNOSTIC_GO_ROOT_REQUIRED}};
    for(unsigned mode=0;mode<5;++mode){
        XrXirArtifact *checked=NULL;CHECK(go_shape_check(&effect_context,mode,false,&checked,NULL)==XR_XIR_OK);
        checked=go_replay(checked);XrXirEffects *effects=NULL;CHECK(effect_analyze(checked,&effects)==XR_XIR_OK);
        root_fact(effects,2,expected[mode].root,expected[mode].unknown);
        CHECK(xr_xir_effects_go_safe(effects,2)==expected[mode].status);
        if(mode>=3)CHECK(xr_xir_effects_function(effects,2)->suspend==XR_XIR_EFFECT_NONE);
        effect_summary_free(effects);effect_artifact_free(checked);
        XrXirDiagnostic diagnostic={0};checked=NULL;
        CHECK(go_shape_check(&effect_context,mode,true,&checked,&diagnostic)==expected[mode].status);
        if(mode)CHECK(!checked&&diagnostic.reason==expected[mode].reason&&diagnostic.function==1&&diagnostic.instruction==0);
        effect_artifact_free(checked);
    }
    go_check_failures();
}
static void go_source_cases(void) {
    for(unsigned known=0;known<2;++known){
        XrXirArtifact *ordinary=go_source(known?"mixed_ordinary.xr":"unknown_ordinary.xr",false,XR_XIR_OK,NULL);
        XrXirEffects *summary=NULL;CHECK(effect_analyze(ordinary,&summary)==XR_XIR_OK);
        uint32_t worker=root_find(xr_xir_compile_artifact_module(ordinary),"worker");
        root_fact(summary,worker,known!=0,true);CHECK(xr_xir_effects_go_safe(summary,worker)==XR_XIR_BAD_TYPE);
        effect_artifact_free(ordinary);root_fact(summary,worker,known!=0,true);effect_summary_free(summary);
    }
    XrXirArtifact *checked=go_replay(go_source("positive.xr",false,XR_XIR_OK,NULL));XrXirEffects *effects=NULL;
    CHECK(effect_analyze(checked,&effects)==XR_XIR_OK);const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    root_fact(effects,root_find(module,"main"),true,false);root_fact(effects,root_find(module,"defaultParent"),true,false);
    root_fact(effects,root_find(module,"worker"),false,false);root_fact(effects,root_find(module,"syncRead"),false,false);
    CHECK(xr_xir_effects_go_safe(effects,root_find(module,"worker"))==XR_XIR_OK);
    effect_summary_free(effects);XrXirArtifact *lowered=go_lower(checked);effect_artifact_free(lowered);effect_artifact_free(checked);
    const char *known[]={"known.xr","default.xr","cleanup.xr","recursive.xr","parent.xr","mixed.xr","const_class.xr"};
    for(size_t i=0;i<sizeof(known)/sizeof(known[0]);++i)CHECK(!go_source(known[i],false,XR_XIR_BAD_TYPE,go_required));
    CHECK(!go_source("unknown.xr",false,XR_XIR_BAD_TYPE,go_unresolved));
    CHECK(!go_source("parameter.xr",false,XR_XIR_BAD_TYPE,NULL));
    CHECK(!go_source("unconstrained.xr",false,XR_XIR_BAD_TYPE,NULL));
}
int main(void) {
    effect_case_begin();go_query_cases();effect_case_end();
    effect_case_begin();go_shape_cases();effect_case_end();
    effect_case_begin();go_source_cases();effect_case_end();
    puts("GO same-owner roots, independent borrow/Sendable/role, Source reasons and native proof authority PASS");
    puts("Full source cause payload, direct var Atomic/Atomic GO carrier and Task322 remain OPEN");return 0;
}
