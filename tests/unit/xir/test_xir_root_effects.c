/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_effects.c - Verified root facts, owned causes and failure boundaries
 */
#include "base/xmalloc.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_effect_analysis_owner.h"
#include "xir/xxir_effects.c"
#include "xir_root_effects_builtin.h"
#include "xir_root_effects_oracles.h"

static XrXirArtifact *root_source(const char *file,XrXirStatus expected) {
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_ROOT_EFFECT_FIXTURES,file)>0);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(effect_context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_ROOT_EFFECT_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,&effect_context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};char *failure_path=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,&failure_path);
    if(status!=expected)fprintf(stderr,"root source %s status%u %d:%d %s\n",
        file,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==expected);
    CHECK(expected==XR_XIR_OK?(source.checked!=NULL):(!source.checked&&!source.snapshot));
    XrXirArtifact *checked=source.checked;source.checked=NULL;
    xr_compile_resources_free(failure_path);xr_xir_compile_source_result_free(&source);xr_compile_session_free(session);
    return checked;
}
static XrXirArtifact *root_replay(XrXirArtifact *source) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source,&packet,NULL)==XR_XIR_OK);
    effect_artifact_free(source);XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);return checked;
}
static void root_occupied(XrXirArtifact *artifact) {
    XrXirEffects *summary=NULL;CHECK(effect_analyze(artifact,&summary)==XR_XIR_OK);
    XrXirEffects *output=summary;EffectMark mark=effect_mark();XrCompileResourceStats before=effect_stats(&effect_context);
    uint8_t bytes[sizeof(XrXirEffects)];memcpy(bytes,summary,sizeof(bytes));attempts=0;
    CHECK(xr_xir_compile_effects_analyze(artifact,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(output==summary&&!attempts&&!memcmp(bytes,summary,sizeof(bytes)));effect_mark_check(mark);
    XrCompileResourceStats after=effect_stats(&effect_context);
    CHECK(before.allocated_bytes==after.allocated_bytes&&before.live_bytes==after.live_bytes&&before.work==after.work);
    XrXirArtifact wrong=*artifact;wrong.module.stage=XR_XIR_BUILT;
    CHECK(xr_xir_compile_effects_analyze(&wrong,&output)==XR_XIR_BAD_STAGE&&output==summary);
    CHECK(xr_xir_compile_effects_analyze(NULL,&output)==XR_XIR_BAD_STRUCTURE&&output==summary);
    CHECK(xr_xir_compile_effects_analyze(artifact,NULL)==XR_XIR_BAD_STRUCTURE);effect_mark_check(mark);
    effect_summary_free(summary);
}
static void root_failures(const XrXirArtifact *artifact) {
    XrXirEffects *summary=NULL;attempts=0;fail_at=SIZE_MAX;
    CHECK(effect_analyze(artifact,&summary)==XR_XIR_OK);size_t sites=attempts;
    effect_summary_free(summary);CHECK(sites&&effect_balanced());
    for(size_t point=0;point<sites;++point){
        EffectMark mark=effect_mark();XrXirCompileContext context=effect_owner_new(effect_caps());
        uint64_t baseline=effect_stats(&context).live_bytes;XrXirArtifact *copy=NULL;
        CHECK(effect_reproduce(artifact,&context,&copy)==XR_XIR_OK);EffectMark retained=effect_mark();
        summary=NULL;attempts=0;injected=false;fail_at=point;
        XrXirStatus status=xr_xir_compile_effects_analyze(copy,&summary);
        CHECK(injected&&status==XR_XIR_OUT_OF_MEMORY&&!summary);effect_mark_check(retained);
        XrCompileResourceStats paid=effect_stats(&context);fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_effects_analyze(copy,&summary)==XR_XIR_OK&&summary);
        XrCompileResourceStats retried=effect_stats(&context);
        CHECK(retried.allocated_bytes>paid.allocated_bytes&&retried.work>paid.work);
        xr_xir_compile_effects_free(summary);effect_mark_check(retained);
        xr_xir_compile_artifact_free(copy);effect_owner_free(&context,baseline);effect_mark_check(mark);
    }
    printf("root facts %zu actual analysis allocation failures, retained charges and physical0\n",sites);
}
static void root_detached_owner(const XrXirArtifact *artifact) {
    EffectMark mark=effect_mark();XrXirCompileContext context=effect_owner_new(effect_caps());
    XrXirArtifact *copy=NULL;XrXirEffects *summary=NULL;
    CHECK(effect_reproduce(artifact,&context,&copy)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(copy,&summary)==XR_XIR_OK);
    uint32_t f=root_find(xr_xir_compile_artifact_module(copy),"mixedRelay");
    CHECK(f<xr_xir_compile_artifact_module(copy)->function_count);
    xr_xir_compile_artifact_free(copy);xr_compile_resources_release(context.resources);context.resources=NULL;
    root_fact(summary,f,true,true);
    CHECK(xr_xir_effects_root_witness(summary,f)->distance==1);
    CHECK(xr_xir_effects_unresolved_witness(summary,f)->distance==1);
    xr_xir_compile_effects_free(summary);effect_mark_check(mark);
}
static void root_source_cases(void) {
    XrXirArtifact *checked=root_replay(root_source("root.xr",XR_XIR_OK));root_source_oracles(checked,false);
    root_occupied(checked);root_failures(checked);effect_budget_boundaries(checked);root_detached_owner(checked);
    effect_analysis_work_cut(checked,1,false);effect_analysis_work_cut(checked,0,true);
    XrXirArtifact *specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked,&specialized,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized,&target,&lowered,NULL)==XR_XIR_OK);
    effect_artifact_free(specialized);root_source_oracles(lowered,true);effect_artifact_free(lowered);
    XrXirEffects *summary=NULL;CHECK(effect_analyze(checked,&summary)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    uint32_t mixed=root_find(module,"mixedRelay"),scalar=root_find(module,"mutableRead");
    CHECK(mixed<module->function_count&&scalar<module->function_count);
    XrXirRootEffectWitness known=*xr_xir_effects_root_witness(summary,mixed);
    XrXirRootEffectWitness unknown=*xr_xir_effects_unresolved_witness(summary,mixed);
    effect_artifact_free(checked);root_fact(summary,mixed,true,true);root_fact(summary,scalar,true,false);
    const XrXirRootEffectWitness *owned=xr_xir_effects_root_witness(summary,mixed);
    CHECK(owned->cause==known.cause&&owned->instruction==known.instruction&&owned->callee==known.callee&&owned->distance==known.distance);
    owned=xr_xir_effects_unresolved_witness(summary,mixed);
    CHECK(owned->cause==unknown.cause&&owned->instruction==unknown.instruction&&owned->callee==unknown.callee&&owned->distance==unknown.distance);
    effect_summary_free(summary);CHECK(effect_balanced());
    puts("root literal source/replay/Checked specialization/Lowered facts; producer-dead owned witnesses PASS");
}
static void root_builtin_cases(void) {
    for(unsigned mode=0;mode<2;++mode){
        XrXirArtifact *artifact=root_replay(root_builtin(mode));XrXirEffects *effects=NULL;
        CHECK(effect_analyze(artifact,&effects)==XR_XIR_OK);
        root_fact(effects,0,true,false);root_fact(effects,1,false,false);root_fact(effects,2,true,false);
        root_fact(effects,3,false,false);root_fact(effects,4,false,false);root_fact(effects,5,mode==0,false);
        root_fact(effects,6,false,true);root_fact(effects,7,false,false);root_fact(effects,8,true,false);
        CHECK(xr_xir_effects_function(effects,6)->suspend==XR_XIR_EFFECT_NONE);
        const XrXirRootEffectWitness *w=xr_xir_effects_unresolved_witness(effects,6);
        CHECK(w&&w->cause==XR_XIR_ROOT_CAUSE_INDIRECT&&w->instruction==0&&w->distance==0);
        if(!mode){w=xr_xir_effects_root_witness(effects,5);CHECK(w->cause==XR_XIR_ROOT_CAUSE_CALL&&w->callee==2&&w->instruction==0&&w->distance==1);}
        root_paths(xr_xir_compile_artifact_module(artifact),effects);effect_summary_free(effects);effect_artifact_free(artifact);
    }
    CHECK(!root_builtin(2));CHECK(!root_builtin(3));
    XrXirArtifact *group=root_replay(root_group());XrXirEffects *effects=NULL;
    CHECK(effect_analyze(group,&effects)==XR_XIR_OK);
    root_fact(effects,0,true,false);root_fact(effects,1,false,false);root_fact(effects,2,true,false);
    const XrXirRootEffectWitness *w=xr_xir_effects_root_witness(effects,0);
    CHECK(w&&w->cause==XR_XIR_ROOT_CAUSE_INITIALIZER&&w->instruction==2&&w->slot==1&&w->distance==0);
    effect_summary_free(effects);effect_artifact_free(group);root_permutation();
    puts("root authentic defaults/noSuspend indirect/reference/group first-slot/edge permutation PASS");
}
static void root_var_atomic_case(void) {
    XrXirArtifact *checked=root_replay(root_source("rejected/mutable_atomic_binding.xr",XR_XIR_OK));
    CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
    XrXirArtifact *specialized=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(checked,&specialized,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(specialized,NULL)==XR_XIR_OK);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(specialized,&target,&lowered,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);
    XrXirArtifact *stages[3]={checked,specialized,lowered};
    for (uint32_t stage=0;stage<3;++stage) {
        const XrXirModule *module=xr_xir_compile_artifact_module(stages[stage]);
        uint32_t read=root_find(module,"read");CHECK(read<module->function_count);
        XrXirEffects *effects=NULL;CHECK(effect_analyze(stages[stage],&effects)==XR_XIR_OK);
        root_fact(effects,read,true,false);
        const XrXirRootEffectWitness *witness=xr_xir_effects_root_witness(effects,read);
        CHECK(witness && witness->cause==XR_XIR_ROOT_CAUSE_MUTABLE_SLOT && witness->distance==0);
        CHECK(witness->slot<module->declarations->slot_count);
        const XrXirSlot *slot=&module->declarations->slots[witness->slot];
        CHECK(slot->mutable && xr_xir_type_is_atomic(module->types,slot->type));
        effect_summary_free(effects);
    }
    effect_artifact_free(lowered);effect_artifact_free(specialized);effect_artifact_free(checked);
    puts("direct var Atomic binding Source/Checked/specialization/full recheck/Lowered exact mutable-slot ROOT PASS");
}
static void root_go_rejections(void) {
    CHECK(!root_source("rejected/root_child.xr",XR_XIR_BAD_TYPE));
    CHECK(!root_source("rejected/root_parent.xr",XR_XIR_BAD_TYPE));
    puts("root GO child/parent BAD_TYPE remain fail closed PASS");
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--var-atomic")));
    effect_case_begin();root_var_atomic_case();effect_case_end();
    if (argc==2) return 0;
    effect_case_begin();root_source_cases();effect_case_end();
    effect_case_begin();root_builtin_cases();effect_case_end();
    effect_case_begin();root_go_rejections();effect_case_end();
    puts("owned root fact queries and independent causes PASS; GO consumer/full qualification remain separate");return 0;
}
