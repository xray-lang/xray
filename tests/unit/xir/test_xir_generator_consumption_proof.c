/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_generator_consumption_proof.c - Literal consumption edges and independent receiving attacks
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_library_compile_owner.h"
#include "xir/xxir_effects.c"
#include "xir_final_origin_receiving_cases.h"
typedef struct ConsumptionProofFixture {
    XrXirTypeNode node;XrXirTypes types;XrXirType scalar,iterator;
    XrXirInstruction init[1],producer[2],consumer[6],run[4],plain[1];
    XrXirBlock blocks[5],consumer_blocks[3];uint32_t actuals[2];
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
    XrXirSourceModule source;XrXirDeclarations declarations;XrXirModule module;
} ConsumptionProofFixture;
static void consumption_proof_fixture(ConsumptionProofFixture *f) {
    memset(f,0,sizeof(*f));f->scalar=XR_XIR_I64;f->iterator=(XrXirType)256;
    f->node=(XrXirTypeNode){.kind=XR_XIR_TYPE_ITERATOR,.element=XR_XIR_I64};
    f->types=(XrXirTypes){&f->node,1,NULL,NULL};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->producer[0]=(XrXirInstruction){.op=XR_XIR_GENERATOR_YIELD,.args={0}};
    f->producer[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->consumer[0]=(XrXirInstruction){.op=XR_XIR_ITERATOR_NEXT,.args={0},.targets={1,2}};
    f->consumer[1]=(XrXirInstruction){.op=XR_XIR_INVOKE_RESULT,.type=XR_XIR_I64};
    f->consumer[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2}};
    f->consumer[3]=(XrXirInstruction){.op=XR_XIR_INVOKE_ERROR,.type=XR_XIR_ERROR};
    f->consumer[4]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=-1};
    f->consumer[5]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={5}};
    f->consumer_blocks[0]=(XrXirBlock){.count=1};
    f->consumer_blocks[1]=(XrXirBlock){.first=1,.count=2};
    f->consumer_blocks[2]=(XrXirBlock){.first=3,.count=3};
    f->run[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=17};
    f->run[1]=(XrXirInstruction){.op=XR_XIR_GENERATOR_NEW,.type=f->iterator,.args={0,1},.immediate=1};
    f->run[2]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={1,1},.immediate=2};
    f->run[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2}};f->actuals[1]=1;
    f->plain[0]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={0}};
    const char *names[]={"init","producer","consume","run","plain"};
    XrXirInstruction *ops[]={f->init,f->producer,f->consumer,f->run,f->plain};uint32_t counts[]={1,2,6,4,1};
    for(uint32_t i=0;i<5;++i) {
        f->blocks[i]=(XrXirBlock){.count=counts[i]};
        f->functions[i]=(XrXirFunction){.name=names[i],.name_length=(uint32_t)strlen(names[i]),
            .result=i==0?XR_XIR_UNIT:i==1?f->iterator:XR_XIR_I64,.blocks=&f->blocks[i],.block_count=1,
            .instructions=ops[i],.instruction_count=counts[i]};
    }
    f->functions[1].parameters=&f->scalar;f->functions[1].parameter_count=1;
    f->functions[4].parameters=&f->scalar;f->functions[4].parameter_count=1;
    f->functions[2].blocks=f->consumer_blocks;f->functions[2].block_count=3;
    f->functions[2].parameters=&f->iterator;f->functions[2].parameter_count=1;
    f->functions[3].operands=f->actuals;f->functions[3].operand_count=2;
    f->identities[1].promises=XR_XIR_FUNCTION_GENERATOR;
    f->source=(XrXirSourceModule){"root",4,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,.entry_function=3};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=5,
        .declarations=&f->declarations,.types=&f->types,.linkage_kind=XR_XIR_PROGRAM};
}
/* A distinct adapter forces the complete SSA reader over the same actual
 * immutable fields. It supplies no expected graph or computed permission. */
static XrXirStatus consumption_proof_instruction(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}
static XrXirStatus consumption_proof_replay(const XrXirCompileContext *context,XrXirEffects *effects,bool shared) {
    EffectBodyRequest body=effect_body_owned_request(context,effects->invocations->body);
    if(!shared) body.instruction=consumption_proof_instruction;
    return final_origin_receive(context,effects,&body);
}
static void consumption_proof_expected(const XrXirCompileContext *context,XrXirEffects *effects,const char *attack,XrXirStatus expected) {
    for(unsigned mode=0;mode<2;++mode) {
        XrXirStatus status=consumption_proof_replay(context,effects,mode!=0);
        if(status!=expected) fprintf(stderr,"Consumption receiving %s mode%u expected%u status%u\n",attack,mode,expected,status);
        CHECK(status==expected);
    }
}
typedef struct ConsumptionProofFailure {const XrXirCompileContext *context;XrXirStatus status;} ConsumptionProofFailure;
static bool consumption_proof_consume(const void *opaque,uint64_t units) {
    return xir_compile_work(((const ConsumptionProofFailure *)opaque)->context,units);
}
static XrXirStatus consumption_proof_denied(const EffectBodyRequest *request,uint32_t f,uint32_t i,
    XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    if(f==2 && !i) return ((const ConsumptionProofFailure *)request->read.owner)->status;
    return effect_body_owned_instruction(request,f,i,storage,output,fields);
}
static void consumption_proof_read_failures(const XrXirCompileContext *context,XrXirEffects *effects) {
    const XrXirStatus failures[]={XR_XIR_BUDGET,XR_XIR_OUT_OF_MEMORY,XR_XIR_BAD_TYPE};
    for(unsigned i=0;i<3;++i) {
        EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationRead read={0};
        CHECK(effect_invocation_read_open(context,certificate,certificate->final,&read)==XR_XIR_OK);
        ConsumptionProofFailure failure={context,failures[i]};EffectBodyRequest body=read.body;
        body.instruction=consumption_proof_denied;body.read=(EffectBodyRead){&failure,consumption_proof_consume};
        CHECK(!effect_body_request_owned_matches(context,&body,certificate->body));
        XrCompileResourceStats before=library_compile_stats(context);
        CHECK(final_origin_receive(context,effects,&body)==failures[i]);
        XrCompileResourceStats after=library_compile_stats(context);CHECK(before.live_bytes==after.live_bytes);
        effect_invocation_read_close(&read);
    }
}
static void consumption_proof_attacks(const XrXirCompileContext *context,XrXirEffects *effects) {
    EffectInvocationFinal *final=effects->invocations->final;uint32_t consume=UINT32_MAX,body=UINT32_MAX;
    CHECK(final->site_count==1 && final->binding_count==1 && final->root_spaces[0]==0 &&
        final->root_spaces[1]==0 && final->root_spaces[2]==2 && final->root_spaces[3]==0);
    CHECK(final->sites[0].function==3 && final->sites[0].instruction==1 && final->sites[0].target==1);
    for(uint32_t n=0;n<final->count;++n) {
        if(final->nodes[n].root==0 && final->nodes[n].body==2) {CHECK(consume==UINT32_MAX);consume=n;}
        if(final->nodes[n].root==0 && final->nodes[n].body==1) {CHECK(body==UINT32_MAX);body=n;}
    }
    CHECK(consume!=UINT32_MAX && body!=UINT32_MAX);
    EffectInvocationNode *node=&final->nodes[consume];uint32_t edge=node->edge_head;
    CHECK(edge<final->edge_count && final->edges[edge].target==body && final->edges[edge].producer==0 &&
        final->edges[edge].instruction==0 && !final->edges[edge].latent);
    bool complete=false;CHECK(effect_final_callable_complete(context,final,consume,0,&complete)==XR_XIR_OK && complete);
    CHECK(final->edges[edge].next==UINT32_MAX && node->parameter_count==1 && node->input[0]==1 &&
        complete && !node->intrinsic_mask && !node->deferred);
    consumption_proof_expected(context,effects,"baseline",XR_XIR_OK);
    uint32_t nodes=final->count;final->count=0;
    consumption_proof_expected(context,effects,"empty_final",XR_XIR_BAD_STRUCTURE);final->count=nodes;
    uint32_t original_body=node->body;node->body=UINT32_MAX;
    consumption_proof_expected(context,effects,"body_out_of_domain",XR_XIR_BAD_STRUCTURE);node->body=original_body;
    uint32_t parameters=node->parameter_count;node->parameter_count=0;
    consumption_proof_expected(context,effects,"parameter_extent",XR_XIR_BAD_STRUCTURE);node->parameter_count=parameters;
    EffectInvocationEdge saved=final->edges[edge];
    final->edges[edge].producer=UINT32_MAX;consumption_proof_expected(context,effects,"producer",XR_XIR_BAD_STRUCTURE);final->edges[edge]=saved;
    final->edges[edge].instruction=1;consumption_proof_expected(context,effects,"instruction",XR_XIR_BAD_STRUCTURE);final->edges[edge]=saved;
    final->edges[edge].latent=true;consumption_proof_expected(context,effects,"latent",XR_XIR_BAD_STRUCTURE);final->edges[edge]=saved;
    final->edges[edge].target=final->roots[0];consumption_proof_expected(context,effects,"target",XR_XIR_BAD_STRUCTURE);final->edges[edge]=saved;
    node->edge_head=UINT32_MAX;consumption_proof_expected(context,effects,"omitted_edge",XR_XIR_BAD_STRUCTURE);node->edge_head=edge;
    final->edges[edge].next=edge;consumption_proof_expected(context,effects,"cycle",XR_XIR_BAD_STRUCTURE);final->edges[edge]=saved;
    uint64_t input=node->input[0];node->input[0]=0;consumption_proof_expected(context,effects,"input",XR_XIR_BAD_STRUCTURE);node->input[0]=input;
    uint32_t ordinal=UINT32_MAX;
    for(uint32_t p=0;p<final->capture.count;++p) {
        EffectCaptureFact fact=final->capture.facts[p];
        if(fact.node==consume && !fact.value && fact.word==EFFECT_CAPTURE_FLAGS && fact.bit==EFFECT_CAPTURE_COMPLETE) ordinal=p;
    }
    CHECK(ordinal!=UINT32_MAX);EffectCaptureFact saved_fact=final->capture.facts[ordinal];
    final->capture.facts[ordinal].rank=UINT64_MAX;
    consumption_proof_expected(context,effects,"rank",XR_XIR_BAD_STRUCTURE);final->capture.facts[ordinal]=saved_fact;
    uint32_t count=final->capture.count,capacity=final->capture.capacity;
    final->capture.count=capacity+1;consumption_proof_expected(context,effects,"origin_count_over_capacity",XR_XIR_BAD_STRUCTURE);
    final->capture.count=UINT32_MAX;final->capture.capacity=UINT32_MAX;
    consumption_proof_expected(context,effects,"origin_capacity_outside_key_space",XR_XIR_BAD_STRUCTURE);
    final->capture.capacity=capacity;final->capture.count=count;CHECK(count);final->capture.count=0;
    consumption_proof_expected(context,effects,"origin_fact",XR_XIR_BAD_STRUCTURE);final->capture.count=count;
    uint32_t plain=final->roots[4];CHECK(plain<final->count && final->nodes[plain].body==4);
    EffectInvocationNode *scalar=&final->nodes[plain];
    complete=true;CHECK(effect_final_callable_complete(context,final,plain,0,&complete)==XR_XIR_OK && !complete);
    CHECK(scalar->parameter_count==1 && scalar->input && !scalar->input[0] && !complete);
    scalar->input[0]=1;consumption_proof_expected(context,effects,"scalar_callable_input",XR_XIR_BAD_STRUCTURE);scalar->input[0]=0;
    final->capture.facts[ordinal]=(EffectCaptureFact){plain,0,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_COMPLETE,0};
    consumption_proof_expected(context,effects,"scalar_completeness",XR_XIR_BAD_STRUCTURE);final->capture.facts[ordinal]=saved_fact;
    consumption_proof_expected(context,effects,"restored",XR_XIR_OK);
    consumption_proof_read_failures(context,effects);
    XrXirEffectFacts facts=xr_xir_effects_read(effects,3);
    CHECK(facts.status==XR_XIR_OK && !facts.root.requires_root && !facts.root.unresolved);
}
static XrXirStatus consumption_proof_operation(const XrXirCompileContext *context,void *opaque) {
    bool oracle=opaque && *(bool *)opaque;ConsumptionProofFixture fixture;consumption_proof_fixture(&fixture);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if(status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xa5,sizeof(fixture));
    if(status==XR_XIR_OK) status=consumption_proof_replay(context,effects,true);
    if(status==XR_XIR_OK) status=consumption_proof_replay(context,effects,false);
    if(status==XR_XIR_OK && oracle) consumption_proof_attacks(context,effects);
    if(oracle && status!=XR_XIR_OK) fprintf(stderr,"Consumption fixture status%u f%u i%u\n",status,diagnostic.function,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);return status;
}

typedef struct ConsumptionRuntimeProbe {uint64_t remaining,work;uint32_t calls,reject;} ConsumptionRuntimeProbe;
static bool consumption_runtime_work(void *owner,uint64_t units) {
    ConsumptionRuntimeProbe *probe=owner;uint32_t call=probe->calls++;
    if(call==probe->reject || units>probe->remaining) return false;
    probe->remaining-=units;probe->work+=units;return true;
}
static uint32_t consumption_runtime_cell(void *owner,uint32_t parameter) {(void)owner;(void)parameter;return 0;}
/* Each operation is checked from real Built input. The consumer's current
 * contextual row belongs to the original root, not its symbolic own root. */
static void consumption_runtime_case(uint32_t mode,uint32_t less) {
    LibraryCompileOwner owner={0};XrCompileResourceLimits limits=library_compile_limits;limits.work=4000000;
    CHECK(library_compile_owner_new(&owner,&limits)==XR_XIR_OK);
    ConsumptionProofFixture fixture;consumption_proof_fixture(&fixture);
    XrXirType parameters[2]={fixture.iterator,XR_XIR_I64};uint32_t actuals[3]={0,1,0};
    if(mode==1) {
        fixture.consumer[0].op=XR_XIR_ITERATOR_HAS_NEXT;fixture.consumer[1].type=XR_XIR_BOOL;
        fixture.consumer[4]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL};
        fixture.functions[2].result=XR_XIR_BOOL;fixture.run[3].args[0]=0;
        fixture.run[2].type=XR_XIR_BOOL;
    } else if(mode==2) {
        fixture.consumer[0].op=XR_XIR_ITERATOR_NTH;fixture.consumer[0].args[1]=1;
        fixture.functions[2].parameters=parameters;fixture.functions[2].parameter_count=2;
        fixture.consumer[2].args[0]=3;fixture.consumer[5].args[0]=6;
        fixture.run[2].args[1]=2;fixture.functions[3].operands=actuals;fixture.functions[3].operand_count=3;
    }
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(&owner.context,&fixture.module,&checked,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"Runtime consume check mode%u status%u f%u i%u\n",mode,status,diagnostic.function,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    EffectInvocationFinal *final=effects->invocations->final;uint32_t parent=UINT32_MAX,target=UINT32_MAX,edge=UINT32_MAX;
    CHECK(final->site_count==1 && final->binding_count==1);
    for(uint32_t n=0;n<final->count;++n)if(final->nodes[n].body==2 && final->nodes[n].root==0) {
        CHECK(parent==UINT32_MAX);parent=n;
    }
    CHECK(parent!=UINT32_MAX);
    for(uint32_t e=final->nodes[parent].edge_head;e!=UINT32_MAX;e=final->edges[e].next)
        if(final->edges[e].instruction==0 && final->edges[e].producer==0) {
            CHECK(edge==UINT32_MAX);edge=e;target=final->edges[e].target;
        }
    CHECK(target<final->count && final->nodes[target].body==1 && !final->edges[edge].latent);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xDA,sizeof(fixture));
    ConsumptionRuntimeProbe probe={1000000,0,0,UINT32_MAX};
    XirEffectInvocationRequest request={.caller=2,.instruction=0,.target=1,.producer=0,.node=parent,
        .count=1,.captures=1,.contextual=true,.read={.owner=&probe,.work=consumption_runtime_work,.cell=consumption_runtime_cell}};
    XirEffectInvocationSelection selected={0};XrCompileResourceStats before=library_compile_stats(&owner.context);
    size_t attempts=source_program_compile_attempts;
    status=xir_effects_execution_edge(effects,&request,&selected);
    if(status!=XR_XIR_OK)fprintf(stderr,"Runtime consume mode%u status%u\n",mode,status);
    CHECK(status==XR_XIR_OK && selected.parent==parent && selected.node==target && selected.edge==edge);
    uint64_t exact=probe.work;uint32_t calls=probe.calls;CHECK(calls && exact);
    for(uint32_t reject=0;reject<calls;++reject) {
        probe=(ConsumptionRuntimeProbe){1000000,0,0,reject};XirEffectInvocationSelection untouched=selected;
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BUDGET &&
            !memcmp(&untouched,&selected,sizeof(selected)) && probe.calls==reject+1);
    }
    EffectInvocationBasis basis={0};probe=(ConsumptionRuntimeProbe){1000000,0,0,UINT32_MAX};
    CHECK(effect_runtime_basis(effects->invocations,0,&basis,&request.read)==XR_XIR_OK);
    uint64_t *input=final->nodes[target].input;CHECK(final->nodes[target].parameter_count==1);
    for(uint32_t w=0;w<basis.row_words;++w) {
        CHECK(!input[w]);input[w]=UINT64_C(1)<<63;
        probe=(ConsumptionRuntimeProbe){1000000,0,0,UINT32_MAX};XirEffectInvocationSelection untouched=selected;
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BAD_TYPE && !memcmp(&untouched,&selected,sizeof(selected)));
        input[w]=0;probe=(ConsumptionRuntimeProbe){1000000,0,0,UINT32_MAX};
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_OK && untouched.node==target);
    }
    for(uint32_t attack=0;attack<6;++attack) {
        XirEffectInvocationRequest forged=request;XirEffectInvocationSelection untouched=selected;
        if(attack==0)forged.producer=UINT32_MAX;
        if(attack==1)forged.captures=0;
        if(attack==2)forged.count=0;
        if(attack==3)forged.target=4;
        if(attack==4)forged.contextual=false;
        if(attack==5)forged.instruction=1;
        probe=(ConsumptionRuntimeProbe){1000000,0,0,UINT32_MAX};
        CHECK(xir_effects_execution_edge(effects,&forged,&untouched)!=XR_XIR_OK && !memcmp(&untouched,&selected,sizeof(selected)));
    }
    probe=(ConsumptionRuntimeProbe){exact-less,0,0,UINT32_MAX};XirEffectInvocationSelection untouched=selected;
    CHECK(xir_effects_execution_edge(effects,&request,&untouched)==(less?XR_XIR_BUDGET:XR_XIR_OK));
    CHECK(less?!memcmp(&untouched,&selected,sizeof(selected)):untouched.node==target);
    XrCompileResourceStats after=library_compile_stats(&owner.context);
    CHECK(before.work==after.work && before.allocation_count==after.allocation_count &&
        before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes && source_program_compile_attempts==attempts);
    xr_xir_compile_effects_free(effects);library_compile_owner_drop(&owner);
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    printf("RUNTIME_CONSUME mode%u genuine current edge/complete scalar row, %u refusals, exact/minus1, producer-dead, no allocations, original4M, physical0 PASS\n",mode,calls);
}

int main(void) {
    LibraryCompileOwner owner={0};bool oracle=true;
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    CHECK(consumption_proof_operation(&owner.context,&oracle)==XR_XIR_OK);library_compile_owner_drop(&owner);
    library_compile_operation_cases("Iterator consumption proof",consumption_proof_operation,NULL);
    for(uint32_t mode=0;mode<3;++mode)for(uint32_t less=0;less<2;++less)consumption_runtime_case(mode,less);
    library_compile_observer_free();CHECK(!source_program_compile_live && !source_program_compile_bytes);
    puts("Iterator consumption: literal current edge, complete input, independent omitted/forged proof, shared/full receiving and finite ownership PASS");
    return 0;
}
