/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_copy_refinement.c - Owned normalization edges and refusal
 *
 * KEY CONCEPT:
 *   These private rows test ownership and boundary checks, not admission of
 *   executable IR. Independent Source tests retain complete semantic checks.
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_root_parameter_fixture.h"
#include "xir/xxir_effects.c"
#include "xir_effect_input_fixture.h"

typedef struct CopyFixture {
    XrXirTypeNode nodes[4];
    XrXirTypes types;
    XrXirType parameters[2];
    XrXirInstruction instructions[4];
    XrXirFunction function;
    XrXirFunctionIdentity identity;
    XrXirDeclarations declarations;
    XrXirRootValueIdentity values[3];
    XrXirFunctionEffectContract contract;
    XrXirProvenance provenance;
    XrXirModule module;
    char name[11];
} CopyFixture;

static void copy_fixture(CopyFixture *f) {
    memset(f,0,sizeof(*f));memcpy(f->name,"copy-owner",11);
    const uint32_t flags[4]={XR_XIR_CALLABLE_ROOT_UNRESOLVED,XR_XIR_CALLABLE_ROOT_REQUIRED,
        XR_XIR_CALLABLE_ROOT_NONE,XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_NO_SUSPEND};
    for (uint32_t t=0;t<4;++t)
        f->nodes[t]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.element=XR_XIR_NEVER,.result=XR_XIR_I64,.flags=flags[t]};
    f->types=(XrXirTypes){.nodes=f->nodes,.count=4};
    f->parameters[0]=f->parameters[1]=(XrXirType)256;
    f->instructions[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={0,0}};
    f->instructions[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_WEAKEN,.type=(XrXirType)256,.args={0,0}};
    f->instructions[2]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)256,.args={1,0}};
    f->instructions[3]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->function=(XrXirFunction){.name=f->name,.name_length=10,.parameters=f->parameters,
        .parameter_count=2,.instructions=f->instructions,.instruction_count=4,.error_type=XR_XIR_NEVER};
    f->declarations=(XrXirDeclarations){.functions=&f->identity};
    f->values[0]=(XrXirRootValueIdentity){0,XR_XIR_EFFECT_VALUE_PROPAGATE,(XrXirType)256};
    f->values[1]=(XrXirRootValueIdentity){1,XR_XIR_EFFECT_VALUE_FIXED,(XrXirType)256};
    f->values[2]=(XrXirRootValueIdentity){2,XR_XIR_EFFECT_VALUE_CALL_BIND,(XrXirType)256};
    f->contract=(XrXirFunctionEffectContract){.values=f->values,.value_count=3};
    f->provenance=(XrXirProvenance){.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=&f->contract,.contract_count=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=&f->function,.function_count=1,
        .types=&f->types,.declarations=&f->declarations,.provenance=&f->provenance};
}

static XrXirStatus copy_operation(const XrXirCompileContext *context,
    const XrXirConstruction *construction,bool oracle) {
    CopyFixture producer,current;copy_fixture(&producer);copy_fixture(&current);
    XrXirEffects effects={.resources=context->resources,.count=1};
    XrXirStatus status=xir_fixture_effects_capture(context,(XirFixtureEffectInput){&producer.module,construction},&effects);
    memset(&producer,0,sizeof(producer));
    XrXirType *declarations=NULL;
    if (status==XR_XIR_OK) {
        const EffectRefinementFunction *original=&effects.refinement->functions[0];
        if (oracle) CHECK(original->copy_count==1 && original->copies[0].instruction==0 &&
            original->copies[0].operation.op==XR_XIR_COPY && original->instructions[0]==(XrXirType)256 &&
            original->name_length==10 && !memcmp(original->name,"copy-owner",10));
        current.parameters[0]=current.parameters[1]=(XrXirType)257;
        current.instructions[0].op=XR_XIR_FUNCTION_WEAKEN;current.instructions[0].type=(XrXirType)257;
        current.values[0].mode=XR_XIR_EFFECT_VALUE_FIXED;current.values[0].declared_type=(XrXirType)257;
        status=effect_invocation_source_declarations(context,&current.module,0,original,&declarations);
        if (status==XR_XIR_OK && oracle) {
            CHECK(declarations && declarations[0]==(XrXirType)257 && declarations[1]==(XrXirType)256);
            for (uint32_t field=0;field<7;++field) {
                XrXirInstruction saved=current.instructions[0];
                switch (field) {
                case 0: current.instructions[0].args[0]=1;break;
                case 1: current.instructions[0].args[1]=1;break;
                case 2: current.instructions[0].targets[0]=1;break;
                case 3: current.instructions[0].targets[1]=1;break;
                case 4: current.instructions[0].immediate=1;break;
                case 5: current.instructions[0].type_arguments[0]=1;break;
                default: current.instructions[0].type_arguments[1]=1;break;
                }
                CHECK(effect_invocation_source_fixed(context,&current.module,0,&current.values[0],original)==XR_XIR_BAD_STRUCTURE);
                current.instructions[0]=saved;
            }
            current.parameters[0]=(XrXirType)256;
            CHECK(effect_invocation_source_fixed(context,&current.module,0,&current.values[0],original)==XR_XIR_BAD_TYPE);
            current.parameters[0]=(XrXirType)257;
            current.instructions[0].type=current.values[0].declared_type=(XrXirType)259;
            CHECK(effect_invocation_source_fixed(context,&current.module,0,&current.values[0],original)==XR_XIR_BAD_TYPE);
            current.instructions[0].type=current.values[0].declared_type=(XrXirType)257;
            for (uint32_t i=1;i<3;++i) {
                current.instructions[i].op=XR_XIR_FUNCTION_WEAKEN;current.instructions[i].type=(XrXirType)257;
                current.values[i].mode=XR_XIR_EFFECT_VALUE_FIXED;current.values[i].declared_type=(XrXirType)257;
                CHECK(effect_invocation_source_fixed(context,&current.module,0,&current.values[i],original)==XR_XIR_BAD_TYPE);
            }
        }
    }
    xr_compile_resources_free(declarations);effect_refinement_free(effects.refinement);return status;
}

static void copy_capture_rejections(const XrXirCompileContext *context,
    const XrXirConstruction *construction) {
    for (uint32_t which=0;which<3;++which) {
        CopyFixture fixture;copy_fixture(&fixture);
        if (!which) fixture.instructions[0].op=XR_XIR_FUNCTION_WEAKEN;
        else if (which==1) fixture.parameters[0]=(XrXirType)257;
        else fixture.values[1].instruction=0;
        XrXirEffects effects={.resources=context->resources,.count=1};
        CHECK(xir_fixture_effects_capture(context,(XirFixtureEffectInput){&fixture.module,construction},&effects)==
            (which==1?XR_XIR_BAD_TYPE:XR_XIR_BAD_STRUCTURE));
        CHECK(!effects.refinement);
    }
}

static void copy_resources(const XrXirConstruction *construction) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;rp_attempts=0;
    CHECK(copy_operation(&context,construction,false)==XR_XIR_OK);
    size_t sites=rp_attempts;XrCompileResourceStats required=rp_stats(&context);
    rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(sites);
    for (size_t site=0;site<sites;++site) {
        context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        RootParameterMark retained=rp_mark();rp_attempts=0;rp_fail_at=site;rp_injected=false;
        XrXirStatus status=copy_operation(&context,construction,false);rp_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected);rp_balanced(retained);
        XrCompileResourceStats paid=rp_stats(&context);CHECK(paid.live_bytes==baseline);
        CHECK(copy_operation(&context,construction,false)==XR_XIR_OK);
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
        CHECK(copy_operation(&context,construction,false)==(less?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    printf("copy refinement: %zu complete OOM sites, same-owner retry, exact/minus1 three axes, physical0\n",sites);
}

int main(int argc,char **argv) {
    if (argc!=1 && (argc!=2 || strcmp(argv[1],"--compiler"))) return 2;
    /* This independent setup ledger stays outside every injected operation. */
    XrXirCompileContext setup=rp_owner(rp_caps());XrXirConstruction *construction=NULL;
    const XrXirConstructionSpec spec={0};const XrXirSlotFacts slots={NULL,0};
    CHECK(xr_xir_compile_construction_new_v4(&setup,NULL,&spec,&slots,&construction, NULL)==XR_XIR_OK && construction);
    if (argc==2) copy_resources(construction);
    else {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(copy_operation(&context,construction,true)==XR_XIR_OK);copy_capture_rejections(&context,construction);
        rp_owner_free(&context,baseline);rp_balanced(physical);
        puts("copy refinement: owned propagation edge, seven mutations, fixed/bind/current bounds, physical0");
    }
    xr_xir_compile_construction_free(construction);xr_compile_resources_release(setup.resources);
    return 0;
}
