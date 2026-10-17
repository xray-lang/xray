/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_receiver_proof.c - Literal complete Class capture relations and attacks
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_source_fixture_owner.h"
#include "xir/xxir_effects.c"
typedef struct ReceiverProofFixture {
    XrXirNominalField field;XrXirNominalDeclaration nominal;XrXirNominalTable nominals;
    XrXirType field_type,body_parameter,constructor_parameter;
    XrXirTypeNode nodes[4];XrXirTypes types;
    XrXirInstruction init[1],body[2],constructor[2],factory[12],run[3];
    XrXirBlock blocks[5];uint32_t constructor_actual,factory_actuals[3];
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;XrXirDeclarations declarations;XrXirModule module;
} ReceiverProofFixture;
static void receiver_proof_fixture(ReceiverProofFixture *f,bool unknown,bool returned,bool stored) {
    memset(f,0,sizeof(*f));const XrXirType object=(XrXirType)256,cell=(XrXirType)257,callback=(XrXirType)258;
    f->field=(XrXirNominalField){{"value",5},XR_XIR_I64,XR_XIR_FIELD_PRIVATE};
    f->nominal=(XrXirNominalDeclaration){.module={"root",4},.name={"CaptureBox",10},.exported=1,
        .fields=&f->field,.field_count=1,.kind=XR_XIR_NOMINAL_CLASS,.flags=XR_XIR_NOMINAL_FINAL};
    f->nominals=(XrXirNominalTable){&f->nominal,1,NULL};f->field_type=XR_XIR_I64;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&f->field_type,1}};
    f->nodes[1]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=object};
    f->nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_NONE};
    f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=callback};
    f->types=(XrXirTypes){f->nodes,4,&f->nominals,NULL};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->body_parameter=object;f->constructor_parameter=XR_XIR_I64;
    f->body[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->body[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    f->constructor[0]=(XrXirInstruction){.op=XR_XIR_CLASS_NEW,.type=object,.args={0,1}};
    f->constructor[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    f->factory[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=17};
    f->factory[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=object,.args={0,1},.immediate=2};
    uint32_t ref=unknown?4u:2u;f->factory_actuals[1]=unknown?3u:1u;
    if(unknown) {
        f->factory[2]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=cell,.args={1}};
        f->factory[3]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=object,.args={2}};
    }
    f->factory[ref]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=callback,.args={1,1},.immediate=1};
    f->factory[ref+1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={ref}};
    if(!returned) {
        f->factory[ref+1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=ref};
        f->factory[ref+2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={ref+1}};
    }
    if(stored) {
        CHECK(!returned);
        f->factory[ref+1]=f->factory[ref];f->factory[ref+1].args[0]=2;
        f->factory_actuals[2]=f->factory_actuals[1];
        f->factory[ref+2]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)259,.args={ref}};
        f->factory[ref+3]=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.args={ref+2,ref+1}};
        f->factory[ref+4]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.type=callback,.args={ref+2}};
        f->factory[ref+5]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=ref+4};
        f->factory[ref+6]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={ref+5}};
    }
    f->run[0]=(XrXirInstruction){.op=XR_XIR_CALL,.type=callback,.immediate=3};
    f->run[1]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64};
    f->run[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1}};
    if(!returned) {
        f->run[0].type=XR_XIR_I64;f->run[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    }
    const char *names[]={"init","body","new","factory","run"};
    XrXirInstruction *ops[]={f->init,f->body,f->constructor,f->factory,f->run};
    uint32_t counts[]={1,2,2,ref+(stored?7u:returned?2u:3u),returned?3u:2u};
    for(uint32_t i=0;i<5;++i) {
        f->blocks[i]=(XrXirBlock){.count=counts[i]};
        f->functions[i]=(XrXirFunction){.name=names[i],.name_length=(uint32_t)strlen(names[i]),
            .result=i==0?XR_XIR_UNIT:i==2?object:i==3 && returned?callback:XR_XIR_I64,.blocks=&f->blocks[i],.block_count=1,
            .instructions=ops[i],.instruction_count=counts[i]};
    }
    f->functions[1].parameters=&f->body_parameter;f->functions[1].parameter_count=1;
    f->functions[2].parameters=&f->constructor_parameter;f->functions[2].parameter_count=1;
    f->functions[2].operands=&f->constructor_actual;f->functions[2].operand_count=1;
    f->functions[3].operands=f->factory_actuals;f->functions[3].operand_count=stored?3u:2u;
    f->identities[2]=(XrXirFunctionIdentity){.nominal_owner=1,.method_kind=XR_XIR_CONSTRUCTOR};
    f->source=(XrXirSourceModule){"root",4,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.entry_function=4};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=5,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_PROGRAM};
}
typedef struct ReceiverProofRead {uint64_t remaining,spent;uint32_t publication;} ReceiverProofRead;
static bool receiver_proof_work(void *owner,uint64_t units) {
    ReceiverProofRead *read=owner;if(units>read->remaining) return false;
    read->remaining-=units;read->spent+=units;return true;
}
static uint32_t receiver_proof_cell(void *owner,uint32_t parameter) {(void)owner;(void)parameter;return 2;}
static XrXirStatus receiver_proof_class(void *owner,uint32_t parameter,uint32_t *output) {
    CHECK(parameter==0 && output);*output=((ReceiverProofRead *)owner)->publication;return XR_XIR_OK;
}
static XrXirStatus receiver_proof_replay(const XrXirCompileContext *context,XrXirEffects *effects) {
    EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationRead replay={0};
    XrXirStatus status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&replay);
    if(status!=XR_XIR_OK) return status;
    replay.work=context;replay.effects=effects;replay.declared=certificate->declared;
    EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
    status=effect_invocation_producers_verify_read(&replay,&body);
    if(status==XR_XIR_OK) status=effect_invocation_class_verify_read(&replay,&body);
    effect_invocation_read_close(&replay);return status;
}
static void receiver_proof_case(bool unknown,bool returned,bool stored) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);ReceiverProofFixture fixture;
    receiver_proof_fixture(&fixture,unknown,returned,stored);XrXirArtifact *checked=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&owner.context,&fixture.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK) fprintf(stderr,"Receiver proof mode%u check%u f%u i%u\n",unknown,status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK);XrXirEffects *effects=NULL;
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    memset(&fixture,0,sizeof(fixture));CHECK(receiver_proof_replay(&owner.context,effects)==XR_XIR_OK);
    XirEffectProducerView producer={0};
    CHECK(xir_effects_producer(&owner.context,effects,3,(unknown?4u:2u)+(stored?1u:0u),&producer)==XR_XIR_OK);
    uint32_t caller=returned?4u:3u,value=returned?0u:(unknown?4u:2u)+(stored?4u:0u);
    ReceiverProofRead read={1000000,0,0};XirEffectInvocationRequest request={.caller=caller,.instruction=returned?1u:value+1,.target=1,
        .producer=producer.site,.node=UINT32_MAX,.count=1,.captures=1,
        .read={.owner=&read,.work=receiver_proof_work,.cell=receiver_proof_cell,.class_value=receiver_proof_class}};
    XirEffectInvocationSelection selected={0};CHECK(xir_effects_execution_edge(effects,&request,&selected)==XR_XIR_OK);
    uint64_t exact=read.spent;XirEffectInvocationSelection baseline=selected;
    read=(ReceiverProofRead){exact,0,0};CHECK(xir_effects_execution_edge(effects,&request,&selected)==XR_XIR_OK && !read.remaining);
    read=(ReceiverProofRead){exact-1,0,0};selected=baseline;
    CHECK(xir_effects_execution_edge(effects,&request,&selected)==XR_XIR_BUDGET && !memcmp(&selected,&baseline,sizeof(selected)));
    for(uint32_t publication=1;publication<3;++publication) {
        read=(ReceiverProofRead){1000000,0,publication};selected=baseline;
        CHECK(xir_effects_execution_edge(effects,&request,&selected)==XR_XIR_BAD_TYPE && !memcmp(&selected,&baseline,sizeof(selected)));
    }
    EffectInvocationFinal *final=effects->invocations->final;uint32_t parent=final->roots[caller];
    EffectInvocationNode *node=&final->nodes[parent];uint32_t captured=UINT32_MAX;
    for(uint32_t p=0;p<node->producer_count;++p)
        if(node->producers[p].value==value && node->producers[p].site==producer.site && node->producers[p].atom) {captured=p;break;}
    CHECK(captured!=UINT32_MAX);uint32_t saved=node->producers[captured].atom;
    node->producers[captured].atom=UINT32_MAX;
    CHECK(receiver_proof_replay(&owner.context,effects)==XR_XIR_BAD_STRUCTURE);node->producers[captured].atom=saved;
    CHECK(receiver_proof_replay(&owner.context,effects)==XR_XIR_OK);
    if(stored) {
        uint32_t next_value=node->producers[captured].next_value;
        node->producers[captured].next_value=value-4;
        CHECK(receiver_proof_replay(&owner.context,effects)==XR_XIR_BAD_STRUCTURE);
        node->producers[captured].next_value=next_value;
        CHECK(receiver_proof_replay(&owner.context,effects)==XR_XIR_OK);
    }
    if(unknown || stored) {
        uint32_t count=node->producer_count;EffectInvocationProducer last=node->producers[count-1],removed=node->producers[captured];
        node->producers[captured]=last;node->producer_count=count-1;
        CHECK(receiver_proof_replay(&owner.context,effects)==XR_XIR_BAD_STRUCTURE);
        node->producer_count=count;node->producers[captured]=removed;node->producers[count-1]=last;
        CHECK(receiver_proof_replay(&owner.context,effects)==XR_XIR_OK);
    }
    xr_xir_compile_effects_free(effects);source_fixture_owner_free(&owner);
    printf("Class capture proof mode%u returned%u stored%u: complete current row, dead producer, physical publication, exact work and forged atom PASS\n",unknown,returned,stored);
}
int main(void) {
    for(unsigned unknown=0;unknown<2;++unknown) for(unsigned returned=0;returned<2;++returned)
        receiver_proof_case(unknown!=0,returned!=0,false);
    for(unsigned unknown=0;unknown<2;++unknown) receiver_proof_case(unknown!=0,false,true);
    return 0;
}
