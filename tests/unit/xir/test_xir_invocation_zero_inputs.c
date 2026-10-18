/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_invocation_zero_inputs.c - Immutable rows and independent node keys
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"
#include "xir/xxir_effects.c"
#include "xir_construction_fixture.h"

static XrXirStatus zero_storage(const XrXirCompileContext *context,bool oracle) {
    XrXirStatus status=XR_XIR_OK;
    EffectInvocationOwner *owner=xir_compile_calloc(context,1,sizeof(*owner),&status);
    if (!owner) return status;
    owner->work=context;
    uint64_t source[17]={0},other[17]={0};const size_t widths[3]={1,2,17};
    for (uint32_t i=0;i<3 && status==XR_XIR_OK;++i) {
        const uint64_t *first=NULL,*second=NULL;
        status=effect_invocation_zero_input(owner,source,widths[i],&first);
        if (status==XR_XIR_OK) status=effect_invocation_zero_input(owner,other,widths[i],&second);
        if (status==XR_XIR_OK && oracle) {
            CHECK(first && first==second && first!=source && first!=other);
            memset(source,0xff,sizeof(source));
            for (size_t w=0;w<widths[i];++w) CHECK(!first[w]);
            memset(source,0,sizeof(source));
        }
    }
    if (status==XR_XIR_OK && oracle) {
        const uint64_t *output=NULL;source[16]=1;
        CHECK(effect_invocation_zero_input(owner,source,17,&output)==XR_XIR_OK && !output);
        source[16]=0;CHECK(effect_invocation_zero_input(owner,NULL,0,&output)==XR_XIR_OK && !output);
        CHECK(effect_invocation_zero_input(owner,NULL,1,&output)==XR_XIR_BAD_STRUCTURE && !output);
        CHECK(effect_invocation_zero_input(owner,source,SIZE_MAX/sizeof(*source),&output)==XR_XIR_BUDGET && !output);
        CHECK(effect_invocation_zero_input(owner,source,17,&output)==XR_XIR_OK && output);
        ((uint64_t *)output)[0]=1;
        const uint64_t *refused=NULL;
        CHECK(effect_invocation_zero_input(owner,source,17,&refused)==XR_XIR_BAD_STRUCTURE && !refused);
        ((uint64_t *)output)[0]=0;
        CHECK(effect_invocation_zero_input(owner,source,17,&output)==XR_XIR_BAD_STRUCTURE);
    }
    effect_invocation_free(owner);return status;
}

typedef struct ZeroSemanticFixture {
    XrXirTypeNode nodes[2];
    XrXirCallableParameter parameter;
    XrXirTypes types;
    XrXirType scalar;
    XrXirInstruction leaf,entry[5],maker[2],initializer;
    XrXirBlock blocks[5];
    uint32_t operands[2];
    XrXirFunction functions[5];
    XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;
    XrXirDeclarations declarations;
    XrXirModule module;
} ZeroSemanticFixture;

static void zero_semantic_fixture(ZeroSemanticFixture *f) {
    memset(f,0,sizeof(*f));f->scalar=XR_XIR_I64;
    f->parameter=(XrXirCallableParameter){XR_XIR_I64,XR_PARAM_READ};
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.parameters=&f->parameter,
        .parameter_count=1,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,
        .result=(XrXirType)256,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=2};
    f->leaf=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,.immediate=4};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    f->entry[2]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,1},.immediate=0};
    f->entry[3]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={1,1},.immediate=1};
    f->entry[4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};
    f->maker[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)256,.immediate=0};
    f->maker[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0,0}};
    f->initializer=(XrXirInstruction){.op=XR_XIR_RETURN};f->operands[0]=f->operands[1]=1;
    for (uint32_t i=0;i<5;++i) f->blocks[i]=(XrXirBlock){.count=i==2?5:i==4?2:1};
    f->functions[0]=(XrXirFunction){.name="id",.name_length=2,.parameters=&f->scalar,.parameter_count=1,
        .result=XR_XIR_I64,.instructions=&f->leaf,.instruction_count=1,.blocks=&f->blocks[0],.block_count=1};
    f->functions[1]=f->functions[0];f->functions[1].name="other";f->functions[1].name_length=5;
    f->functions[2]=(XrXirFunction){.name="entry",.name_length=5,.result=XR_XIR_I64,
        .instructions=f->entry,.instruction_count=5,.blocks=&f->blocks[2],.block_count=1,
        .operands=f->operands,.operand_count=2};
    f->functions[3]=(XrXirFunction){.name="init",.name_length=4,.instructions=&f->initializer,
        .instruction_count=1,.blocks=&f->blocks[3],.block_count=1};
    f->functions[4]=(XrXirFunction){.name="maker",.name_length=5,.result=(XrXirType)256,
        .instructions=f->maker,.instruction_count=2,.blocks=&f->blocks[4],.block_count=1};
    f->source=(XrXirSourceModule){.name="zero-inputs",.name_length=11,.initializer=3};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.entry_function=2};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.types=&f->types,.declarations=&f->declarations,
        .functions=f->functions,.function_count=5,.linkage_kind=XR_XIR_PROGRAM};
}

static XrXirStatus zero_semantic(const XrXirCompileContext *context,bool oracle) {
    ZeroSemanticFixture source;zero_semantic_fixture(&source);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&source.module,&checked,&diagnostic);
    memset(&source,0,sizeof(source));
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);
    if (status==XR_XIR_OK && oracle) {
        CHECK(effects && effects->invocations && effects->invocations->equations);
        const EffectInvocationOwner *owner=effects->invocations->equations;
        uint32_t a=owner->roots[0],b=owner->roots[1];
        CHECK(a!=b && a<owner->count && b<owner->count && owner->zero_inputs);
        CHECK(owner->nodes[a].body==0 && owner->nodes[b].body==1);
        CHECK(owner->nodes[a].input && owner->nodes[a].input==owner->nodes[b].input);
        CHECK(!owner->nodes[a].input[0] && !owner->nodes[a].input[1]);
        for (uint32_t f=0;f<5;++f) {
            const XrXirRootEffects *root=xr_xir_effects_root(effects,f);
            /* An authentic initializer requires root even with an empty body. */
            bool expected=f==3;
            if (!root || root->requires_root!=expected || root->unresolved)
                fprintf(stderr,"zero semantic f%u root=%u unknown=%u\n",f,root?root->requires_root:9,root?root->unresolved:9);
            CHECK(root && root->requires_root==expected && !root->unresolved);
        }
        CHECK(xr_xir_effects_go_safe(effects,3)==XR_XIR_BAD_TYPE);
    }
    xr_xir_compile_effects_free(effects);return status;
}

static XrXirStatus zero_operation(const XrXirCompileContext *context,uint32_t which,bool oracle) {
    return which?zero_semantic(context,oracle):zero_storage(context,oracle);
}

static void zero_resources(uint32_t which) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;rp_attempts=0;
    CHECK(zero_operation(&context,which,false)==XR_XIR_OK);
    size_t sites=rp_attempts;XrCompileResourceStats required=rp_stats(&context);CHECK(sites);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    for (size_t site=0;site<sites;++site) {
        context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        RootParameterMark retained=rp_mark();rp_attempts=0;rp_fail_at=site;rp_injected=false;
        XrXirStatus status=zero_operation(&context,which,false);rp_fail_at=SIZE_MAX;
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"zero%u site%zu/%zu status%u\n",which,site,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected);rp_balanced(retained);
        XrCompileResourceStats paid=rp_stats(&context);CHECK(paid.live_bytes==baseline);
        CHECK(zero_operation(&context,which,false)==XR_XIR_OK);
        XrCompileResourceStats retried=rp_stats(&context);
        CHECK(retried.allocated_bytes>paid.allocated_bytes && retried.work>paid.work);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    for (uint32_t axis=0;axis<3;++axis) for (uint32_t less=0;less<2;++less) {
        XrCompileResourceLimits limits=rp_caps();
        uint64_t amount=axis==0?required.allocated_bytes:axis==1?required.peak_bytes:required.work;
        CHECK(amount);
        if (!axis) limits.allocated_bytes=amount-less;
        else if (axis==1) limits.live_bytes=amount-less;else limits.work=amount-less;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(zero_operation(&context,which,false)==(less?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    printf("zero%u fullFI=%zu same-owner retry and exact/minus1 axes physical0\n",which,sites);
}

int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--compiler")) {zero_resources(0);zero_resources(1);}
    else if (argc==1) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(zero_storage(&context,true)==XR_XIR_OK);CHECK(zero_semantic(&context,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
        puts("Owned zero words: producer death, independent root/body keys, corruption/nonzero refusal, physical0");
    } else return 2;
    return 0;
}
