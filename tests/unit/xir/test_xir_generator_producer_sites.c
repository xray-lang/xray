/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_producer_sites.c - Independent parked-producer site correspondence
 *
 * KEY CONCEPT:
 *   Real generator creation and function references share one finite inventory.
 *   Site identity supplies no ROOT permission and survives producer destruction.
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_library_compile_owner.h"
#include "xir/xxir_effects.c"
typedef struct GeneratorSiteFixture {
    XrXirTypeNode nodes[2];XrXirTypes types;XrXirType parameter;
    XrXirInstruction init[1],producer[2],plain[2],factory[5];
    XrXirBlock blocks[4];uint32_t actuals[2];
    XrXirFunction functions[4];XrXirFunctionIdentity identities[4];
    XrXirSourceModule source;XrXirDeclarations declarations;XrXirModule module;
} GeneratorSiteFixture;
static void generator_site_fixture(GeneratorSiteFixture *f) {
    memset(f,0,sizeof(*f));const XrXirType iterator=(XrXirType)256,callable=(XrXirType)257;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ITERATOR,.element=XR_XIR_I64};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->types=(XrXirTypes){f->nodes,2,NULL,NULL};f->parameter=XR_XIR_I64;
    f->init[0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->producer[0]=(XrXirInstruction){.op=XR_XIR_GENERATOR_YIELD,.args={0}};
    f->producer[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->plain[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->plain[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    f->factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=17};
    f->factory[1]=(XrXirInstruction){.op=XR_XIR_GENERATOR_NEW,.type=iterator,.args={0,1},.immediate=1};
    f->factory[2]=(XrXirInstruction){.op=XR_XIR_GENERATOR_NEW,.type=iterator,.args={1,1},.immediate=1};
    f->factory[3]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=callable,.args={0,0},.immediate=2};
    f->factory[4]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    const char *names[]={"init","producer","plain","factory"};
    XrXirInstruction *ops[]={f->init,f->producer,f->plain,f->factory};uint32_t counts[]={1,2,2,5};
    for(uint32_t i=0;i<4;++i) {
        f->blocks[i]=(XrXirBlock){.count=counts[i]};
        f->functions[i]=(XrXirFunction){.name=names[i],.name_length=(uint32_t)strlen(names[i]),
            .result=i==0?XR_XIR_UNIT:i==1?iterator:XR_XIR_I64,.blocks=&f->blocks[i],.block_count=1,
            .instructions=ops[i],.instruction_count=counts[i]};
    }
    f->functions[1].parameters=&f->parameter;f->functions[1].parameter_count=1;
    f->functions[3].operands=f->actuals;f->functions[3].operand_count=2;
    f->identities[1].promises=XR_XIR_FUNCTION_GENERATOR;
    f->source=(XrXirSourceModule){"root",4,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.entry_function=3};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=4,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_PROGRAM};
}
static void generator_site_rejected(const XrXirCompileContext *context,XrXirEffects *effects,
    uint32_t instruction,const XirEffectProducerView *original) {
    XirEffectProducerView output=*original;
    CHECK(xir_effects_producer(context,effects,3,instruction,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&output,original,sizeof(output)));
}
static XrXirStatus generator_site_replay(const XrXirCompileContext *context,XrXirEffects *effects) {
    EffectInvocationRead read={0};EffectInvocationCertificate *certificate=effects->invocations;
    XrXirStatus status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&read);
    effect_invocation_read_close(&read);return status;
}
static void generator_site_attacks(const XrXirCompileContext *context,XrXirEffects *effects,
    const XirEffectProducerView views[3]) {
    EffectInvocationFinal *final=effects->invocations->final;
    CHECK(final->site_count==3 && final->binding_count==2);
    CHECK(generator_site_replay(context,effects)==XR_XIR_OK);
    final->site_count=2;CHECK(generator_site_replay(context,effects)==XR_XIR_BAD_STRUCTURE);
    final->site_count=4;CHECK(generator_site_replay(context,effects)==XR_XIR_BAD_STRUCTURE);final->site_count=3;
    final->binding_count=3;CHECK(generator_site_replay(context,effects)==XR_XIR_BAD_STRUCTURE);final->binding_count=2;
    EffectInvocationSite *site=&final->sites[0],saved=*site;
    site->target=2;generator_site_rejected(context,effects,1,&views[0]);*site=saved;
    site->captures=0;generator_site_rejected(context,effects,1,&views[0]);*site=saved;
    site->binding=1;CHECK(generator_site_replay(context,effects)==XR_XIR_BAD_STRUCTURE);*site=saved;
    site->binding=UINT32_MAX;generator_site_rejected(context,effects,1,&views[0]);*site=saved;
    site->function=2;generator_site_rejected(context,effects,1,&views[0]);*site=saved;
    site->instruction=2;
    generator_site_rejected(context,effects,1,&views[0]);generator_site_rejected(context,effects,2,&views[1]);*site=saved;
    XrXirFunctionIdentity *identities=(XrXirFunctionIdentity *)effects->invocations->declarations->functions;
    uint32_t promises=identities[1].promises;identities[1].promises=0;
    generator_site_rejected(context,effects,1,&views[0]);identities[1].promises=promises;
    promises=identities[2].promises;identities[2].promises|=XR_XIR_FUNCTION_GENERATOR;
    generator_site_rejected(context,effects,3,&views[2]);identities[2].promises=promises;
    XrXirTypeNode *types=(XrXirTypeNode *)effects->invocations->terms.types.nodes;
    XrXirTypeNode type=types[0];types[0].kind=XR_XIR_TYPE_ARRAY;
    generator_site_rejected(context,effects,1,&views[0]);types[0]=type;
    for(uint32_t i=0;i<3;++i) {
        XirEffectProducerView output={0};CHECK(xir_effects_producer(context,effects,3,i+1,&output)==XR_XIR_OK);
        CHECK(!memcmp(&output,&views[i],sizeof(output)));
    }
    CHECK(generator_site_replay(context,effects)==XR_XIR_OK);
    XrXirEffectFacts facts=xr_xir_effects_read(effects,3);
    CHECK(facts.status==XR_XIR_OK && !facts.root.requires_root && !facts.root.unresolved);
}
static XrXirStatus generator_site_operation(const XrXirCompileContext *context,void *opaque) {
    bool oracle=opaque && *(bool *)opaque;GeneratorSiteFixture fixture;generator_site_fixture(&fixture);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XirEffectProducerView views[3]={{0}};
    uint32_t stage=1;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if(status==XR_XIR_OK) {stage=2;status=xr_xir_compile_artifact_verify(checked,&diagnostic);}
    if(status==XR_XIR_OK) {stage=3;status=xr_xir_compile_effects_analyze(checked,&effects);}
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xa5,sizeof(fixture));
    for(uint32_t i=0;status==XR_XIR_OK && i<3;++i) {
        stage=4+i;status=xir_effects_producer(context,effects,3,i+1,&views[i]);
        if(status==XR_XIR_OK && oracle) {
            CHECK(views[i].site==i && views[i].function==3 && views[i].instruction==i+1);
            CHECK(views[i].target==(i<2?1u:2u) && views[i].capture_count==(i<2?1u:0u));
            CHECK(views[i].type==(XrXirType)(i<2?256:257));
        }
    }
    if(status==XR_XIR_OK && oracle) generator_site_attacks(context,effects,views);
    if(oracle && status!=XR_XIR_OK) fprintf(stderr,"Generator site stage%u status%u f%u i%u\n",
        stage,status,diagnostic.function,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);return status;
}
int main(void) {
    LibraryCompileOwner owner={0};bool oracle=true;
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    CHECK(generator_site_operation(&owner.context,&oracle)==XR_XIR_OK);library_compile_owner_drop(&owner);
    library_compile_operation_cases("Generator producer inventory",generator_site_operation,NULL);
    library_compile_observer_free();CHECK(!source_program_compile_live && !source_program_compile_bytes);
    puts("Generator sites: two distinct creations with equal arguments, FnRef identity, dead producer, forged coordinates/role/type and finite ownership PASS");
    return 0;
}
