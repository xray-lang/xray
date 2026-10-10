/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_callable_root_bounds.c - Literal contracts, true bodies and resources
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "xir_callable_root_owner.h"
#include "xir/xxir_effects.c"
#include "xir_callable_root_fixture.h"
#include "xir_callable_root_interfaces.h"
#include "xir_callable_root_packet.h"
_Static_assert(XR_XIR_CALLABLE_NO_SUSPEND==1 && XR_XIR_CALLABLE_ROOT_NONE==2 &&
    XR_XIR_CALLABLE_ROOT_REQUIRED==4 && XR_XIR_CALLABLE_ROOT_UNRESOLVED==8 &&
    XR_XIR_CALLABLE_ROOT_MASK==14,"The closed bound bit assignments are one contract");
_Static_assert(XR_XIR_CHECKED_SCHEMA==28 && XR_XIR_CHECKED_CONTRACT==73,"The callable dependency contract has one wire identity");

static void bound_matrix(void) {
    static const bool admitted[16]={false,false,true,true,true,true,false,false,true,true,false,false,true,true,false,false};
    static const uint32_t root[]={2,4,8,12};
    static const bool compatible[4][4]={{true,true,true,true},{false,true,true,true},{false,false,true,true},{false,false,true,true}};
    static const uint32_t joins[4][4]={{2,4,8,12},{4,4,12,12},{8,12,8,12},{12,12,12,12}};
    uint32_t accepted=0, rejected=0;
    XrXirCompileContext context=bound_owner(bound_caps());
    for(uint32_t flags=0;flags<16;++flags) {
        XrXirTypeNode node={.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=flags};
        XrXirTypes types={&node,1,NULL,NULL};
        CHECK(xr_xir_callable_flags_valid(flags)==admitted[flags]);
        CHECK(xr_xir_compile_types_structure_verify(&context,&types)==(admitted[flags]?XR_XIR_OK:XR_XIR_BAD_TYPE));
    }
    for(uint32_t a=0;a<4;++a)for(uint32_t b=0;b<4;++b)for(uint32_t sn=0;sn<2;++sn)for(uint32_t tn=0;tn<2;++tn) {
        uint32_t source=root[a]|sn,target=root[b]|tn;
        bool allowed=compatible[a][b] && (!tn || sn);
        CHECK(xr_xir_callable_flags_compatible(source,target)==allowed);
        XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=source},
            {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=target}};
        XrXirTypes types={nodes,source==target?1u:2u,NULL,NULL};
        CHECK(xr_xir_compile_types_structure_verify(&context,&types)==XR_XIR_OK);
        XrXirType receive=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+(source==target?0:1));
        CHECK(xr_xir_compile_callable_weakening(&context,&types,XR_XIR_CONSTRUCTED_TYPE_BASE,receive)==
            (allowed?XR_XIR_OK:XR_XIR_BAD_TYPE));
        if (allowed) ++accepted; else ++rejected;
        uint32_t joined=UINT32_MAX;
        CHECK(xr_xir_callable_flags_join(source,target,&joined));
        CHECK(joined==(joins[a][b]|(sn&tn)));
        CHECK(xr_xir_callable_root_accepts(a==1 || a==3,a==2 || a==3,target)==compatible[a][b]);
    }
    CHECK(accepted==33 && rejected==31);
    uint32_t untouched=71;
    CHECK(!xr_xir_callable_flags_valid(18));
    CHECK(!xr_xir_callable_flags_join(0,2,&untouched) && untouched==71);
    CHECK(!xr_xir_callable_flags_join(2,1,&untouched) && untouched==71);
    CHECK(!xr_xir_callable_flags_join(2,2,NULL));
    CHECK(xr_xir_compile_callable_weakening(NULL,NULL,XR_XIR_UNIT,XR_XIR_UNIT)==XR_XIR_BAD_STRUCTURE);
    bound_owner_free(&context);
}
static void bound_nested(void) {
    XrXirCompileContext context=bound_owner(bound_caps());
    XrXirCallableParameter parameters[]={{XR_XIR_CONSTRUCTED_TYPE_BASE,0},
        {(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),0}};
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=2},
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=4},
        {.kind=XR_XIR_TYPE_CALLABLE,.parameters=&parameters[0],.parameter_count=1,.result=XR_XIR_I64,.flags=3},
        {.kind=XR_XIR_TYPE_CALLABLE,.parameters=&parameters[1],.parameter_count=1,.result=XR_XIR_I64,.flags=8}};
    XrXirTypes types={nodes,4,NULL,NULL};
    CHECK(xr_xir_compile_types_structure_verify(&context,&types)==XR_XIR_OK);
    CHECK(xr_xir_compile_callable_weakening(&context,&types,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2),
        (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3))==XR_XIR_BAD_TYPE);
    nodes[3].parameters=&parameters[0];nodes[3].result=XR_XIR_BOOL;
    CHECK(xr_xir_compile_callable_weakening(&context,&types,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2),
        (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3))==XR_XIR_BAD_TYPE);
    nodes[3].result=XR_XIR_I64;parameters[1].type=parameters[0].type;parameters[1].mode=1;nodes[3].parameters=&parameters[1];
    CHECK(xr_xir_compile_callable_weakening(&context,&types,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+2),
        (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+3))==XR_XIR_BAD_TYPE);
    bound_owner_free(&context);
}
static void bound_facts(void) {
    static const bool expected[13][2]={{true,false},{false,false},{true,false},{false,false},
        {false,false},{false,false},{false,false},{true,false},{false,true},{true,true},
        {true,true},{false,true},{false,true}};
    XrXirCompileContext context=bound_owner(bound_caps());XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=bound_fixture(&context,0,3,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"bound fixture status%u f%u b%u i%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && checked);
    XrXirEffects *effects=NULL;CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK && effects);
    for(uint32_t f=0;f<13;++f) {
        const XrXirRootEffects *actual=xr_xir_effects_root(effects,f);CHECK(actual);
        CHECK(actual->requires_root==expected[f][0] && actual->unresolved==expected[f][1]);
        CHECK((xr_xir_effects_root_witness(effects,f)!=NULL)==expected[f][0]);
        CHECK((xr_xir_effects_unresolved_witness(effects,f)!=NULL)==expected[f][1]);
    }
    CHECK(xr_xir_effects_function(effects,12)->suspend==XR_XIR_EFFECT_NONE);
    const XrXirRootEffectWitness *known=xr_xir_effects_root_witness(effects,9),*unknown=xr_xir_effects_unresolved_witness(effects,9);
    CHECK(known && unknown && known->cause==XR_XIR_ROOT_CAUSE_INDIRECT && unknown->cause==XR_XIR_ROOT_CAUSE_INDIRECT);
    CHECK(known->instruction==0 && unknown->instruction==0 && known->distance==0 && unknown->distance==0);
    XrXirRootCauseTrace *trace=NULL;CHECK(xr_xir_compile_root_cause_trace_copy(&context,effects,10,&trace)==XR_XIR_OK && trace);
    XrCompileResourceStats before=bound_stats(&context);
    CHECK(xr_xir_compile_effects_infer_verified(&context,xr_xir_compile_artifact_module(checked),&effects)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=bound_stats(&context);CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes);
    xr_xir_compile_effects_free(effects);xr_xir_compile_artifact_free(checked);
    xr_compile_resources_release(context.resources);context=(XrXirCompileContext){0};
    const XrXirRootEffects *detached=xr_xir_root_cause_trace_facts(trace);CHECK(detached && detached->requires_root && detached->unresolved);
    for(uint32_t chain=0;chain<2;++chain) {
        uint32_t count=0;const XrXirRootCauseStep *steps=xr_xir_root_cause_trace_steps(trace,chain!=0,&count);
        CHECK(steps && count==2 && steps[0].function==10 && steps[0].instruction==0 && steps[0].callee==9 && steps[0].distance==1);
        CHECK(steps[0].cause==XR_XIR_ROOT_CAUSE_CALL && steps[1].function==9 && steps[1].instruction==0 && steps[1].distance==0);
        CHECK(steps[1].cause==XR_XIR_ROOT_CAUSE_INDIRECT && steps[1].callee==UINT32_MAX && steps[1].slot==UINT32_MAX);
    }
    xr_xir_compile_root_cause_trace_free(trace);
}
static void bound_false_advertisements(void) {
    XrXirCompileContext context=bound_owner(bound_caps());XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(bound_fixture(&context,0,2,&checked,&diagnostic)==XR_XIR_BAD_TYPE && !checked);
    CHECK(diagnostic.function==4 && diagnostic.instruction==0);
    CHECK(bound_fixture(&context,1,11,&checked,&diagnostic)==XR_XIR_BAD_TYPE && !checked);
    CHECK(diagnostic.function==4 && diagnostic.instruction==0);
    CHECK(bound_fixture(&context,2,2,&checked,&diagnostic)==XR_XIR_OK && checked);
    xr_xir_compile_artifact_free(checked);bound_owner_free(&context);
}
static void bound_resource_gates(void) {
    BoundMark mark=bound_mark();XrXirCompileContext context=bound_owner(bound_caps());
    size_t first=bound_attempts;XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    CHECK(bound_fixture(&context,0,3,&checked,&diagnostic)==XR_XIR_OK && checked);
    size_t sites=bound_attempts-first;XrCompileResourceStats required=bound_stats(&context);
    xr_xir_compile_artifact_free(checked);bound_owner_free(&context);bound_balanced(mark);CHECK(sites>0);
    for(size_t site=0;site<sites;++site) {
        context=bound_owner(bound_caps());BoundMark owner_mark=bound_mark();
        bound_injected=false;bound_fail_at=bound_attempts+site;checked=NULL;
        CHECK(bound_fixture(&context,0,3,&checked,&diagnostic)==XR_XIR_OUT_OF_MEMORY && !checked && bound_injected);
        bound_fail_at=SIZE_MAX;bound_balanced(owner_mark);CHECK(bound_stats(&context).live_bytes==bound_creation.live_bytes);
        XrCompileResourceStats paid=bound_stats(&context);
        CHECK(bound_fixture(&context,0,3,&checked,&diagnostic)==XR_XIR_OK && checked);
        CHECK(bound_stats(&context).work>paid.work && bound_stats(&context).allocated_bytes>paid.allocated_bytes);
        xr_xir_compile_artifact_free(checked);bound_owner_free(&context);bound_balanced(mark);
    }
    for(uint32_t axis=0;axis<3;++axis)for(uint32_t shortfall=0;shortfall<2;++shortfall) {
        XrCompileResourceLimits caps=bound_caps();
        if(axis==0)caps.allocated_bytes=required.allocated_bytes-shortfall;
        else if(axis==1)caps.live_bytes=required.peak_bytes-shortfall;
        else caps.work=required.work-shortfall;
        context=bound_owner(caps);checked=NULL;
        CHECK(bound_fixture(&context,0,3,&checked,&diagnostic)==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
        CHECK(shortfall?checked==NULL:checked!=NULL);
        xr_xir_compile_artifact_free(checked);bound_owner_free(&context);bound_balanced(mark);
    }
    printf("callable bounds %zu actual OOM sites, three exact/minus1 axes, physical0\n",sites);
}
#include "xir_callable_equal_barrier_cases.h"
int main(void) {
    BoundMark mark=bound_mark();bound_matrix();bound_balanced(mark);bound_nested();bound_balanced(mark);
    bound_equal_barrier_cases();bound_balanced(mark);
    bound_facts();bound_balanced(mark);bound_false_advertisements();bound_balanced(mark);
    bound_interfaces();bound_balanced(mark);bound_packets();bound_balanced(mark);
    bound_resource_gates();bound_balanced(mark);puts("closed callable roots: independent flags, target checks and owned witnesses");return 0;
}
