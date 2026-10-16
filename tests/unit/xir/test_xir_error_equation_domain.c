/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_equation_domain.c - Grounded Error facts across real calls
 */
#define XIR_ERROR_EQUATION_TEST_ENTRY local_equation_fixture_entry
#include "test_xir_error_equation_current.c"
#undef XIR_ERROR_EQUATION_TEST_ENTRY
#include "xir/xxir_effect_error_equation_domain.inc.c"

static XrXirStatus domain_function(const EffectErrorEquationDomain *domain,uint32_t context,
    uint32_t *output) {
    if(domain->context_count!=domain->body.domain.function_count || context>=domain->context_count)
        return XR_XIR_BAD_STRUCTURE;
    *output=context;return XR_XIR_OK;
}
static XrXirStatus domain_call(const EffectErrorEquationRead *read,uint32_t instruction,
    uint32_t word,uint64_t *output) {
    const EffectErrorEquationDomain *domain=read->owner;
    XrXirInstruction op={0};XrXirStatus status=error_equation_operation(read,instruction,&op);
    if(status!=XR_XIR_OK) return status;
    if(op.op!=XR_XIR_CALL && op.op!=XR_XIR_INVOKE && op.op!=XR_XIR_GO)
        return XR_XIR_BAD_STRUCTURE;
    if(op.type_arguments[1] || op.immediate<0 || (uint64_t)op.immediate>=domain->context_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectErrorEquationQuery query={(uint32_t)op.immediate,UINT32_MAX,UINT32_MAX,word,read->rank_limit};
    return error_equation_domain_word(domain,&query,output);
}
static void domain_fixture(EdgeFixture *fixture,unsigned mode) {
    ee_fixture(fixture,EE_JUMP,false);
    fixture->subject[0]=(XrXirInstruction){.op=XR_XIR_CALL,.immediate=2};
    fixture->subject[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture->blocks[0]=(XrXirBlock){.count=2};
    fixture->functions[1]=(XrXirFunction){.name="caller",.name_length=6,.result=XR_XIR_UNIT,
        .blocks=fixture->blocks,.block_count=1,.instructions=fixture->subject,.instruction_count=2};
    fixture->thrower[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM};
    fixture->thrower[1]=(XrXirInstruction){.op=XR_XIR_THROW,.args={0}};
    if(mode==1) {
        fixture->thrower[0]=(XrXirInstruction){.op=XR_XIR_CALL,.immediate=1};
        fixture->thrower[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    } else if(mode==2) {
        fixture->cleanup[0]=(XrXirInstruction){.op=XR_XIR_CALL,.immediate=1};
        fixture->cleanup[1]=fixture->thrower[0];
        fixture->cleanup[2]=(XrXirInstruction){.op=XR_XIR_THROW,.args={1}};
        fixture->functions[2].instructions=fixture->cleanup;fixture->functions[2].instruction_count=3;
        fixture->throw_block.count=3;
    } else if(mode>=3 && mode<=5) {
        fixture->thrower[0].immediate=mode==3?61:mode==4?62:69;
    } else if(mode==6) {
        fixture->parameters[0]=XR_XIR_ERROR;
        fixture->functions[1].parameters=fixture->parameters;fixture->functions[1].parameter_count=1;
        fixture->subject[0].args[1]=1;fixture->operands[0]=0;
        fixture->functions[1].operands=fixture->operands;fixture->functions[1].operand_count=1;
        fixture->functions[2].parameters=fixture->parameters;fixture->functions[2].parameter_count=1;
        fixture->thrower[0]=(XrXirInstruction){.op=XR_XIR_THROW,.args={0}};
        fixture->functions[2].instruction_count=1;fixture->throw_block.count=1;
    }
}
static uint64_t domain_literal(unsigned mode,uint32_t function,uint32_t word) {
    if(!function || mode==1) return 0;
    uint32_t bit=mode==6?1:mode==3?63:mode==4?64:mode==5?71:2;
    return bit/64==word?UINT64_C(1)<<(bit%64):0;
}
static void domain_append(EffectErrorEquationFact *facts,uint32_t *count,
    uint32_t block,uint32_t value,uint32_t word,uint64_t bits) {
    for(uint32_t bit=0;bit<64;++bit) if(bits&(UINT64_C(1)<<bit)) {
        CHECK(*count<128);facts[(*count)++]=(EffectErrorEquationFact){block,value,word*64+bit,0};
    }
}
static void domain_snapshot(ErrorFlow *flow,EffectErrorEquationDomain *domain,
    EffectErrorEquationContext *contexts,EffectErrorEquationFact *facts,unsigned mode) {
    for(uint32_t c=0;c<domain->context_count;++c) {
        CHECK(error_function(flow,c)==XR_XIR_OK && !flow->restart);
        contexts[c].first=domain->fact_count;
        for(uint32_t b=0;b<flow->function->block_count;++b) {
            for(uint32_t s=0;s<flow->active_count;++s) for(uint32_t w=0;w<domain->words;++w)
                domain_append(facts,&domain->fact_count,b,flow->active[s],w,
                    flow->states[(size_t)b*flow->stride+(size_t)s*domain->words+w]);
            if(flow->reachable[b]&ERROR_BLOCK_REACHABLE)
                domain_append(facts,&domain->fact_count,b,UINT32_MAX,0,1);
        }
        for(uint32_t w=0;w<domain->words;++w) {
            uint64_t word=flow->effects->outcomes_work.errors[(size_t)c*domain->words+w];
            CHECK(word==domain_literal(mode,c,w));
            domain_append(facts,&domain->fact_count,UINT32_MAX,UINT32_MAX,w,word);
        }
        contexts[c].count=domain->fact_count-contexts[c].first;
    }
}
static void domain_insert(EffectErrorEquationDomain *domain,EffectErrorEquationContext *contexts,
    EffectErrorEquationFact *facts,uint32_t c) {
    CHECK(domain->fact_count<128);
    uint32_t at=contexts[c].first+contexts[c].count;
    memmove(facts+at+1,facts+at,(size_t)(domain->fact_count-at)*sizeof(*facts));
    facts[at]=(EffectErrorEquationFact){UINT32_MAX,UINT32_MAX,1,domain->fact_count+1};
    ++contexts[c].count;++domain->fact_count;
    for(uint32_t n=c+1;n<domain->context_count;++n) ++contexts[n].first;
}
static void domain_corrupt(EffectErrorEquationDomain *domain,EffectErrorEquationContext *contexts,
    EffectErrorEquationFact *facts,unsigned attack) {
    if(attack==1) {
        domain_insert(domain,contexts,facts,1);domain_insert(domain,contexts,facts,2);
        CHECK(error_equation_domain_closure(domain)==XR_XIR_OK);
    } else if(attack==2) {
        for(uint32_t n=0;n<domain->fact_count;++n) facts[n].rank=1;
        CHECK(error_equation_domain_closure(domain)==XR_XIR_OK);
    } else if(attack==3) {
        CHECK(contexts[2].count==2);
        --contexts[2].count;--domain->fact_count;
    } else if(attack==4) domain->contexts=(const EffectErrorEquationContext *)(uintptr_t)1;
    else if(attack==5) domain->facts=(const EffectErrorEquationFact *)(uintptr_t)1;
    else if(attack==6) {
        for(uint32_t n=0;n<domain->fact_count;++n)
            if(facts[n].block==UINT32_MAX) facts[n].bit=1;
    } else if(attack==7) {
        EffectErrorAtom *atoms=(EffectErrorAtom *)domain->atoms;atoms[0].variant=1;
        CHECK(contexts[2].count==2 && contexts[1].count==2);
        --contexts[2].count;--domain->fact_count;
        uint32_t at=contexts[1].first+contexts[1].count-1;
        memmove(facts+at,facts+at+1,(size_t)(domain->fact_count-at-1)*sizeof(*facts));
        --contexts[1].count;--contexts[2].first;--domain->fact_count;
        for(uint32_t c=0;c<domain->context_count;++c) {
            EffectErrorEquationProof proof;
            CHECK(error_equation_domain_open(domain,c,&proof)==XR_XIR_OK);
            CHECK(error_equation_proof_closure(&proof)==XR_XIR_OK);
        }
    } else if(attack==8) domain->atoms=(const EffectErrorAtom *)(uintptr_t)1;
    else CHECK(!attack);
}

int main(int argc,char **argv) {
    CHECK(argc==3);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);
    unsigned attack=(unsigned)strtoul(argv[2],NULL,10);CHECK(mode<7 && attack<9);
    EdgeFixture fixture;domain_fixture(&fixture,mode);
    XrXirNominalVariant variants[70];char names[70][8];
    if(mode>=3 && mode<=5) {
        for(unsigned v=0;v<70;++v) {
            int length=snprintf(names[v],sizeof(names[v]),"V%u",v);CHECK(length>0 && length<8);
            variants[v]=(XrXirNominalVariant){{names[v],(uint32_t)length},0,0};
        }
        fixture.nominal.variants=variants;fixture.nominal.variant_count=70;
    }
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    XrXirEffects effects={0};EffectTerms terms={0};ErrorFlow flow={.remaining=&work};
    pc_private_begin(&work,&fixture,&effects,&terms,&flow);
    EffectBodyMaterial *body=NULL;
    CHECK(effect_body_material_create(&work,&fixture.module,&body)==XR_XIR_OK);
    flow.body=effect_body_owned_request(&work,body);
    bool stable=false;
    for(unsigned iteration=0;iteration<8 && !stable;++iteration) {
        uint64_t before[6];size_t bytes=(size_t)effects.count*effects.outcomes_work.words*sizeof(uint64_t);
        CHECK(bytes<=sizeof(before));memcpy(before,effects.outcomes_work.errors,bytes);
        for(uint32_t f=0;f<effects.count;++f) CHECK(error_function(&flow,f)==XR_XIR_OK && !flow.restart);
        stable=!memcmp(before,effects.outcomes_work.errors,bytes);
    }
    CHECK(stable && effects.count==3);
    XrXirStatus status=XR_XIR_OK;
    EffectErrorEquationContext *contexts=xir_compile_calloc(&work,effects.count,sizeof(*contexts),&status);
    EffectErrorEquationFact *facts=xir_compile_calloc(&work,128,sizeof(*facts),&status);
    EffectErrorAtom *atoms=xir_compile_copy(&work,effects.outcomes_work.atoms,
        (size_t)effects.outcomes_work.atom_count*sizeof(*atoms),&status);
    CHECK(status==XR_XIR_OK);
    EffectErrorEquationDomain domain={.body=flow.body,.work=&work,.atoms=atoms,
        .atom_count=effects.outcomes_work.atom_count,.words=effects.outcomes_work.words,
        .context_count=effects.count,.contexts=contexts,.facts=facts,.owner=body,
        .function=domain_function,.call=domain_call};
    domain_snapshot(&flow,&domain,contexts,facts,mode);
    CHECK(error_equation_domain_ground(&domain,facts)==XR_XIR_OK);
    CHECK(error_equation_domain_current(&domain)==XR_XIR_OK);
    pc_private_free(&flow,&terms,&effects);
    memset(&fixture,0xDC,sizeof(fixture));memset(variants,0xDC,sizeof(variants));memset(names,0xDC,sizeof(names));
    EffectErrorEquationContext saved_contexts[3];EffectErrorEquationFact saved_facts[128];
    EffectErrorAtom saved_atoms[70];CHECK(domain.atom_count<=70);
    memcpy(saved_atoms,atoms,(size_t)domain.atom_count*sizeof(*atoms));
    memcpy(saved_contexts,contexts,sizeof(saved_contexts));memcpy(saved_facts,facts,sizeof(saved_facts));
    EffectErrorEquationDomain saved=domain;
    size_t allocations=attempts;uint64_t live_before=pc_stats(&work).live_bytes;
    CHECK(error_equation_domain_current(&domain)==XR_XIR_OK);
    domain_corrupt(&domain,contexts,facts,attack);
    CHECK(error_equation_domain_current(&domain)==(attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
    domain=saved;memcpy(contexts,saved_contexts,sizeof(saved_contexts));memcpy(facts,saved_facts,sizeof(saved_facts));
    memcpy(atoms,saved_atoms,(size_t)domain.atom_count*sizeof(*atoms));
    CHECK(error_equation_domain_current(&domain)==XR_XIR_OK);
    CHECK(attempts==allocations && pc_stats(&work).live_bytes==live_before);
    fprintf(stderr,"global Error mode%u attack%u facts%u work%llu; scratch/source dead; no alloc/free; recovery\n",
        mode,attack,domain.fact_count,(unsigned long long)pc_stats(&work).work);
    xr_compile_resources_free(contexts);xr_compile_resources_free(facts);xr_compile_resources_free(atoms);
    effect_body_material_free(body);pc_release(&work,baseline);CHECK(!live && !live_bytes);
    return 0;
}
