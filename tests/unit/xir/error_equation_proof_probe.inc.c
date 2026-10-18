/* Current CFG proof probes use owned Input after the producer's scratch dies. */
#include "xir/xxir_effect_error_equation_proof.inc.c"

/* This fixture boundary has one literal, non-generic throwing callee. Read its
 * actual body rather than borrowing the producer's proposed summary. General
 * invocation substitution and coverage remain separate integration work. */
static XrXirStatus equation_proof_fixture_call(const EffectErrorEquationRead *read,
    uint32_t i,uint32_t word,uint64_t *output) {
    XrXirInstruction call;XrXirStatus status=error_equation_operation(read,i,&call);
    if(status!=XR_XIR_OK) return status;
    if(call.op==XR_XIR_CALL_INDIRECT || call.op==XR_XIR_INVOKE_INDIRECT ||
        call.op==XR_XIR_CALL_REQUIREMENT) { *output=word?0:1;return XR_XIR_OK; }
    if(call.immediate<0 || (uint64_t)call.immediate>=read->body.domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectErrorEquationRead callee=*read;callee.function=(uint32_t)call.immediate;
    status=effect_body_request_head(&read->body,callee.function,&callee.head);
    if(status!=XR_XIR_OK) return status;
    if(callee.head.parameter_count || callee.head.block_count!=1) return XR_XIR_BAD_STRUCTURE;
    if(callee.head.instruction_count==1) {
        XrXirInstruction only;
        status=error_equation_operation(&callee,0,&only);
        if(status!=XR_XIR_OK) return status;
        if(only.op!=XR_XIR_RETURN) return XR_XIR_BAD_STRUCTURE;
        *output=0;return XR_XIR_OK;
    }
    if(callee.head.instruction_count!=2) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction seed={0},thrower={0};
    status=error_equation_operation(&callee,0,&seed);
    if(status==XR_XIR_OK) status=error_equation_operation(&callee,1,&thrower);
    if(status!=XR_XIR_OK) return status;
    if(seed.op!=XR_XIR_ENUM_NEW || thrower.op!=XR_XIR_THROW || thrower.args[0])
        return XR_XIR_BAD_STRUCTURE;
    uint64_t result=0;
    for(uint32_t a=0;a<read->atom_count;++a) {
        if(!xir_compile_work(read->work,3)) return XR_XIR_BUDGET;
        if(read->atoms[a].type==seed.type && read->atoms[a].variant==(uint32_t)seed.immediate &&
            (a+2)/64==word) result|=UINT64_C(1)<<((a+2)%64);
    }
    *output=result;return XR_XIR_OK;
}
static uint32_t equation_bits(uint64_t word) {
    uint32_t count=0;while(word) { word&=word-1;++count; }return count;
}
static EffectErrorEquationProof equation_proof_build(const EffectErrorEquationRead *read,
    const EquationSnapshot *snapshot,EffectErrorEquationFact **output) {
    uint64_t count=0;
    for(uint32_t b=0;b<snapshot->blocks;++b) {
        count+=(snapshot->reachable[b]&ERROR_BLOCK_REACHABLE)!=0;
        for(uint32_t s=0;s<snapshot->active_count;++s) for(uint32_t w=0;w<read->words;++w)
            count+=equation_bits(snapshot->entries[((size_t)b*snapshot->active_count+s)*read->words+w]);
    }
    for(uint32_t w=0;w<read->words;++w)
        count+=equation_bits(snapshot->summaries[(size_t)read->function*read->words+w]);
    CHECK(count<=UINT32_MAX-2);
    XrXirStatus status=XR_XIR_OK;
    EffectErrorEquationFact *facts=xir_compile_alloc(read->work,
        ((size_t)count+2)*sizeof(*facts),&status);
    CHECK(facts && status==XR_XIR_OK);
    uint32_t at=0;
    for(uint32_t b=0;b<snapshot->blocks;++b) {
        for(uint32_t s=0;s<snapshot->active_count;++s) for(uint32_t w=0;w<read->words;++w) {
            uint64_t row=snapshot->entries[((size_t)b*snapshot->active_count+s)*read->words+w];
            for(uint32_t bit=0;bit<64;++bit) if(row&(UINT64_C(1)<<bit))
                facts[at++]=(EffectErrorEquationFact){b,snapshot->active[s],w*64+bit,0};
        }
        if(snapshot->reachable[b]&ERROR_BLOCK_REACHABLE)
            facts[at++]=(EffectErrorEquationFact){b,UINT32_MAX,0,0};
    }
    for(uint32_t w=0;w<read->words;++w) {
        uint64_t row=snapshot->summaries[(size_t)read->function*read->words+w];
        for(uint32_t bit=0;bit<64;++bit) if(row&(UINT64_C(1)<<bit))
            facts[at++]=(EffectErrorEquationFact){UINT32_MAX,UINT32_MAX,w*64+bit,0};
    }
    CHECK(at==count);
    EffectErrorEquationProof proof={.base=*read,.facts=facts,.count=at};
    proof.base.call=equation_proof_fixture_call;
    status=error_equation_proof_ground(&proof,facts);
    if(status!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(read->work->resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"proof grounding status%u facts%u work%llu\n",status,at,
            (unsigned long long)stats.work);
    }
    CHECK(status==XR_XIR_OK);
    CHECK(error_equation_proof_current(&proof)==XR_XIR_OK);
    *output=facts;return proof;
}
static void equation_fact_insert(EffectErrorEquationProof *proof,EffectErrorEquationFact *facts,
    EffectErrorEquationFact incoming) {
    uint32_t at=0;
    while(at<proof->count && error_equation_fact_order(&facts[at],&incoming)<0) ++at;
    memmove(facts+at+1,facts+at,(size_t)(proof->count-at)*sizeof(*facts));
    facts[at]=incoming;++proof->count;
}
static void equation_proof_attack(EffectErrorEquationProof *proof,EffectErrorEquationFact *facts,
    unsigned attack) {
    CHECK(proof->count);
    if(attack==1) facts[0].rank=0;
    else if(attack==2) facts[0].rank=proof->count+1;
    else if(attack==3) {
        memmove(facts,facts+1,(size_t)(proof->count-1)*sizeof(*facts));--proof->count;
    } else if(attack==4) {
        equation_fact_insert(proof,facts,(EffectErrorEquationFact){UINT32_MAX,UINT32_MAX,0,1});
    } else if(attack==5) {
        /* Padding cannot become a new Error atom. */
        equation_fact_insert(proof,facts,(EffectErrorEquationFact){UINT32_MAX,UINT32_MAX,
            proof->base.atom_count+2,1});
    } else if(attack==6) {
        /* Block 0 is authentically reached; its derivation still cannot cite
         * another fact at the same rank to justify the downstream block. */
        for(uint32_t n=0;n<proof->count;++n) facts[n].rank=1;
    } else if(attack==7 || attack==8) {
        /* The literal Input has no path from entry into block 1. Closure alone
         * accepts a self-supported reachable SCC and its self-Phi Error bit. */
        equation_fact_insert(proof,facts,(EffectErrorEquationFact){1,UINT32_MAX,0,2});
        if(attack==8) equation_fact_insert(proof,facts,(EffectErrorEquationFact){1,5,3,2});
        CHECK(error_equation_proof_closure(proof)==XR_XIR_OK);
    } else if(attack==9) proof->facts=(const EffectErrorEquationFact *)(uintptr_t)1;
    else if(attack==10) proof->facts=(const EffectErrorEquationFact *)((const uint8_t *)facts+1);
    else if(attack==11) proof->count+=4;
    else if(attack==12) proof->facts=NULL;
    else CHECK(!attack);
}
static void equation_unseeded_fixture(EdgeFixture *fixture) {
    ee_fixture(fixture,EE_JUMP,false);
    fixture->subject[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM};
    fixture->subject[1]=(XrXirInstruction){.op=XR_XIR_ENUM_TAG,.type=XR_XIR_I64,.args={0}};
    fixture->subject[2]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1};
    fixture->subject[3]=(XrXirInstruction){.op=XR_XIR_EQ_INT,.type=XR_XIR_BOOL,.args={1,2}};
    fixture->subject[4]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={3},.targets={1,2}};
    fixture->subject[5]=(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)EE_ENUM,.args={0,4}};
    fixture->subject[6]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    fixture->subject[7]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture->blocks[0]=(XrXirBlock){.count=5};
    fixture->blocks[1]=(XrXirBlock){.first=5,.count=2};
    fixture->blocks[2]=(XrXirBlock){.first=7,.count=1};
    fixture->operands[0]=0;fixture->operands[1]=0;
    fixture->operands[2]=1;fixture->operands[3]=5;
    fixture->functions[1].parameters=NULL;fixture->functions[1].parameter_count=0;
    fixture->functions[1].instructions=fixture->subject;fixture->functions[1].instruction_count=8;
    fixture->functions[1].blocks=fixture->blocks;fixture->functions[1].block_count=3;
    fixture->functions[1].operands=fixture->operands;fixture->functions[1].operand_count=4;
}
