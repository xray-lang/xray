/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_error_equation_current.c - Current Input transfers after scratch death
 */
#define main pending_fixture_entry
#include "test_xir_error_cfg_pending.c"
#undef main

typedef struct EquationSnapshot {
    uint32_t *active;
    uint64_t *entries,*summaries;
    uint8_t *reachable;
    EffectErrorAtom *atoms;
    uint32_t active_count,blocks,functions,words;
} EquationSnapshot;
static XrXirStatus equation_entry(const EffectErrorEquationRead *read,uint32_t block,
    uint32_t value,uint32_t word,uint64_t *output) {
    const EquationSnapshot *snapshot=read->owner;
    if(block>=snapshot->blocks || word>=snapshot->words) return XR_XIR_BAD_STRUCTURE;
    *output=0;
    for(uint32_t s=0;s<snapshot->active_count;++s) {
        if(!xir_compile_work(read->work,1)) return XR_XIR_BUDGET;
        if(snapshot->active[s]==value) {
            *output=snapshot->entries[((size_t)block*snapshot->active_count+s)*snapshot->words+word];break;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus equation_call(const EffectErrorEquationRead *read,uint32_t i,
    uint32_t word,uint64_t *output) {
    const EquationSnapshot *snapshot=read->owner;
    XrXirInstruction op;XrXirStatus status=error_equation_operation(read,i,&op);
    if(status!=XR_XIR_OK) return status;
    if(op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_INVOKE_INDIRECT || op.op==XR_XIR_CALL_REQUIREMENT) {
        *output=word?0:1;return XR_XIR_OK;
    }
    if(op.immediate<0 || (uint64_t)op.immediate>=snapshot->functions || word>=snapshot->words)
        return XR_XIR_BAD_STRUCTURE;
    *output=snapshot->summaries[(size_t)op.immediate*snapshot->words+word];return XR_XIR_OK;
}
static void equation_compare(const EffectErrorEquationRead *read,const ErrorFlow *reference,
    uint32_t block,uint32_t cursor,uint32_t value) {
    for(uint32_t w=0;w<read->words;++w) {
        uint64_t actual=UINT64_MAX;
        CHECK(error_equation_value(read,block,cursor,value,w,&actual)==XR_XIR_OK);
        uint64_t expected=error_read(reference,reference->work,value)[w];
        if(actual!=expected) fprintf(stderr,"b%u cursor%u value%u word%u expected%llu actual%llu\n",
            block,cursor,value,w,(unsigned long long)expected,(unsigned long long)actual);
        CHECK(actual==expected);
    }
}
static void equation_filters(const EffectErrorEquationRead *read,ErrorFlow *reference,
    uint32_t block,uint32_t cursor,const XrXirInstruction *branch) {
    for(unsigned yes=0;yes<2;++yes) {
        memcpy(reference->edge,reference->work,reference->stride*sizeof(uint64_t));
        bool possible=false;CHECK(error_filter(reference,branch,yes!=0,&possible)==XR_XIR_OK);
        EffectErrorFilterRead filter;
        CHECK(error_equation_filter_read(read,branch,yes!=0,&filter)==XR_XIR_OK);
        bool equation_possible=filter.possible;
        if(filter.recognized) {
            equation_possible=false;
            for(uint32_t w=0;w<read->words;++w) {
                uint64_t row=0,mask=0;
                CHECK(error_equation_value(read,block,cursor,filter.value,w,&row)==XR_XIR_OK);
                CHECK(error_equation_filter_mask(read,&filter,filter.value,w,&mask)==XR_XIR_OK);
                equation_possible|=(row&mask)!=0;
            }
        }
        CHECK(equation_possible==possible);
        if(!possible) continue;
        for(uint32_t s=0;s<reference->active_count;++s) for(uint32_t w=0;w<read->words;++w) {
            uint32_t value=reference->active[s];uint64_t row=0,mask=0;
            CHECK(error_equation_value(read,block,cursor,value,w,&row)==XR_XIR_OK);
            CHECK(error_equation_filter_mask(read,&filter,value,w,&mask)==XR_XIR_OK);
            CHECK((row&mask)==error_read(reference,reference->edge,value)[w]);
        }
    }
}

static void equation_edge_compare(const EffectErrorEquationRead *read,ErrorFlow *reference,
    const EffectErrorEdgeRead *edge) {
    const EquationSnapshot *snapshot=read->owner;
    XrXirInstruction terminal={0};bool possible=true;const XrXirInstruction *branch=NULL;
    if(edge->kind!=EFFECT_ERROR_EDGE_PANIC) {
        CHECK(error_equation_operation(read,edge->cursor-1,&terminal)==XR_XIR_OK);
        if(terminal.op==XR_XIR_BRANCH) {
            branch=&terminal;
            memcpy(reference->edge,reference->work,reference->stride*sizeof(uint64_t));
            CHECK(error_filter(reference,branch,edge->kind==EFFECT_ERROR_EDGE_NORMAL,&possible)==XR_XIR_OK);
        } else if(edge->kind==EFFECT_ERROR_EDGE_ALTERNATE) {
            memset(reference->snapshot,0,(size_t)read->words*sizeof(uint64_t));
            if(terminal.op==XR_XIR_TASK_AWAIT)
                memcpy(reference->snapshot,error_read(reference,reference->work,terminal.args[0]),
                    (size_t)read->words*sizeof(uint64_t));
            else {
                reference->instruction=edge->cursor-1;
                CHECK(error_call(reference,&terminal,reference->snapshot)==XR_XIR_OK && !reference->restart);
            }
            possible=error_any(reference->snapshot,read->words);
        }
    }
    uint64_t *destination=reference->states+(size_t)edge->to*reference->stride;
    memset(destination,0,reference->stride*sizeof(uint64_t));
    uint8_t reachable=reference->reachable[edge->to];
    if(possible) CHECK(error_edge(reference,edge->from,edge->to,branch,
        edge->kind==EFFECT_ERROR_EDGE_NORMAL)==XR_XIR_OK);
    for(uint32_t s=0;s<snapshot->active_count;++s) for(uint32_t w=0;w<read->words;++w) {
        uint64_t row=UINT64_MAX;bool available=false;
        CHECK(error_equation_edge_word(read,edge,snapshot->active[s],w,&row,&available)==XR_XIR_OK);
        if(available!=possible || row!=destination[(size_t)s*read->words+w])
            fprintf(stderr,"edge %u->%u cursor%u kind%u value%u expected%llu actual%llu possible%u/%u\n",
                edge->from,edge->to,edge->cursor,(unsigned)edge->kind,snapshot->active[s],
                (unsigned long long)destination[(size_t)s*read->words+w],(unsigned long long)row,possible,available);
        CHECK(available==possible && row==destination[(size_t)s*read->words+w]);
    }
    if(snapshot->active_count)
        memcpy(destination,snapshot->entries+(size_t)edge->to*snapshot->active_count*read->words,
            (size_t)snapshot->active_count*read->words*sizeof(uint64_t));
    reference->reachable[edge->to]=reachable;
}
static void equation_terminal_edges(const EffectErrorEquationRead *read,ErrorFlow *reference,
    uint32_t block,const XrXirInstruction *op) {
    const XrXirBlock *source=&read->head.blocks[block];
    uint32_t count=op->op==XR_XIR_BRANCH || op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_DEFAULT ||
        op->op==XR_XIR_INVOKE_INDIRECT || op->op==XR_XIR_TASK_AWAIT?2:
        op->op==XR_XIR_JUMP || op->op==XR_XIR_CLEANUP_REGISTER || op->op==XR_XIR_CLEANUP_LEAVE ||
        op->op==XR_XIR_CLEANUP_ERROR?1:0;
    for(uint32_t side=0;side<count;++side) {
        EffectErrorEdgeRead edge={block,op->targets[side],source->first+source->count,
            side?EFFECT_ERROR_EDGE_ALTERNATE:EFFECT_ERROR_EDGE_NORMAL};
        equation_edge_compare(read,reference,&edge);
    }
}

#include "error_equation_proof_probe.inc.c"
#ifndef XIR_ERROR_EQUATION_TEST_ENTRY
#define XIR_ERROR_EQUATION_TEST_ENTRY main
#endif
int XIR_ERROR_EQUATION_TEST_ENTRY(int argc,char **argv) {
    CHECK(argc==3 || argc==4);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);
    bool saved=strtoul(argv[2],NULL,10)!=0;CHECK(mode<15);
    unsigned proof_attack=argc==4?(unsigned)strtoul(argv[3],NULL,10):0;
    EdgeFixture fixture;
    if(mode<10) pc_fixture(&fixture,(PendingCase)mode,saved);
    else if(mode<14) pc_unit_cell_fixture(&fixture,mode-10);
    else equation_unseeded_fixture(&fixture);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    XrXirEffects effects={0};EffectTerms terms={0};ErrorFlow flow={.remaining=&context};
    pc_private_begin(&context,&fixture,&effects,&terms,&flow);
    if(mode<10 && mode!=PC_TEMPORARY) effects.outcomes_work.errors[2]=UINT64_C(8);
    EffectBodyMaterial *material=NULL;
    CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    flow.body=effect_body_owned_request(&context,material);
    CHECK(error_function(&flow,1)==XR_XIR_OK && !flow.restart);
    XrXirStatus status=XR_XIR_OK;EquationSnapshot snapshot={0};
    snapshot.active_count=flow.active_count;snapshot.blocks=flow.function->block_count;
    snapshot.functions=effects.count;snapshot.words=effects.outcomes_work.words;
    snapshot.active=xir_compile_copy(&context,flow.active,(size_t)flow.active_count*sizeof(uint32_t),&status);
    size_t row_words=(size_t)snapshot.blocks*snapshot.active_count*snapshot.words;
    snapshot.entries=row_words?xir_compile_alloc(&context,row_words*sizeof(uint64_t),&status):NULL;
    for(uint32_t b=0;b<snapshot.blocks;++b) if(snapshot.active_count)
        memcpy(snapshot.entries+(size_t)b*snapshot.active_count*snapshot.words,
            flow.states+(size_t)b*flow.stride,(size_t)snapshot.active_count*snapshot.words*sizeof(uint64_t));
    snapshot.reachable=xir_compile_copy(&context,flow.reachable,snapshot.blocks,&status);
    snapshot.atoms=xir_compile_copy(&context,effects.outcomes_work.atoms,
        (size_t)effects.outcomes_work.atom_count*sizeof(EffectErrorAtom),&status);
    snapshot.summaries=xir_compile_copy(&context,effects.outcomes_work.errors,
        (size_t)effects.count*snapshot.words*sizeof(uint64_t),&status);
    CHECK(status==XR_XIR_OK);
    EffectErrorEquationRead read={.body=flow.body,.head=*flow.function,.work=&context,
        .atoms=snapshot.atoms,.context=1,.function=1,.atom_count=effects.outcomes_work.atom_count,
        .words=snapshot.words,.owner=&snapshot,.entry=equation_entry,.call=equation_call};
    EffectErrorEquationFact *facts=NULL;EffectErrorEquationProof proof={0};
    if(argc==4) proof=equation_proof_build(&read,&snapshot,&facts);
    size_t allocations=attempts;uint64_t prepared=pc_stats(&context).live_bytes;
    for(uint32_t b=0;b<snapshot.blocks;++b) if(snapshot.reachable[b]&ERROR_BLOCK_REACHABLE) {
        memcpy(flow.work,flow.states+(size_t)b*flow.stride,flow.stride*sizeof(uint64_t));
        const XrXirBlock *block=&read.head.blocks[b];
        for(uint32_t i=block->first;i-block->first<block->count;++i) {
            XrXirInstruction op;CHECK(error_equation_operation(&read,i,&op)==XR_XIR_OK);
            if(block->panic) {
                EffectErrorEdgeRead edge={b,block->panic,i,EFFECT_ERROR_EDGE_PANIC};
                equation_edge_compare(&read,&flow,&edge);
            }
            CHECK(error_instruction(&flow,i,&op)==XR_XIR_OK && !flow.restart);
            if(op.op==XR_XIR_CONST_INT && op.type==XR_XIR_I64) {
                equation_compare(&read,&flow,b,i+1,read.head.parameter_count+i);continue;
            }
            for(uint32_t s=0;s<snapshot.active_count;++s)
                equation_compare(&read,&flow,b,i+1,snapshot.active[s]);
            if(op.op==XR_XIR_BRANCH) equation_filters(&read,&flow,b,i+1,&op);
            if(i+1==block->first+block->count) equation_terminal_edges(&read,&flow,b,&op);
        }
    }
    CHECK(attempts==allocations && pc_stats(&context).live_bytes==prepared);
    uint64_t expected=effects.outcomes_work.errors[1];
    if(mode<10) CHECK(expected==pc_expected((PendingCase)mode,saved));
    else if(mode<14) {
        unsigned cell=mode-10;
        uint32_t first=cell==0 || cell==2?2:1,alias=cell==3?3:cell==1?2:3;
        uint64_t literal=cell<2?0:cell==2?UINT64_C(12):UINT64_C(4),a=UINT64_MAX,b=UINT64_MAX;
        CHECK(error_equation_value(&read,0,read.head.blocks[0].count,first,0,&a)==XR_XIR_OK);
        CHECK(error_equation_value(&read,0,read.head.blocks[0].count,alias,0,&b)==XR_XIR_OK);
        CHECK(a==literal && b==literal && expected==0);
    } else CHECK(expected==0);
    pc_private_free(&flow,&terms,&effects);memset(&fixture,0xDC,sizeof(fixture));
    allocations=attempts;prepared=pc_stats(&context).live_bytes;uint64_t escaping=0;
    for(uint32_t b=0;b<snapshot.blocks;++b) if(snapshot.reachable[b]&ERROR_BLOCK_REACHABLE) {
        const XrXirBlock *block=&read.head.blocks[b];
        for(uint32_t i=block->first;i-block->first<block->count;++i) {
            XrXirInstruction op;CHECK(error_equation_operation(&read,i,&op)==XR_XIR_OK);
            uint64_t row=0;
            if(op.op==XR_XIR_THROW) CHECK(error_equation_value(&read,b,i,op.args[0],0,&row)==XR_XIR_OK);
            if(op.op==XR_XIR_CALL || op.op==XR_XIR_CALL_INDIRECT || op.op==XR_XIR_CALL_REQUIREMENT)
                CHECK(equation_call(&read,i,0,&row)==XR_XIR_OK);
            escaping|=row;
        }
    }
    CHECK(escaping==expected && attempts==allocations && pc_stats(&context).live_bytes==prepared);
    xr_compile_resources_free(snapshot.active);xr_compile_resources_free(snapshot.entries);
    xr_compile_resources_free(snapshot.reachable);
    xr_compile_resources_free(snapshot.summaries);
    if(argc==4) {
        allocations=attempts;prepared=pc_stats(&context).live_bytes;
        XrXirStatus current=error_equation_proof_current(&proof);
        fprintf(stderr,"post-scratch current%u work%llu\n",current,
            (unsigned long long)pc_stats(&context).work);
        CHECK(current==XR_XIR_OK);
        equation_proof_attack(&proof,facts,proof_attack);
        CHECK(error_equation_proof_current(&proof)==(proof_attack?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
        CHECK(attempts==allocations && pc_stats(&context).live_bytes==prepared);
        xr_compile_resources_free(facts);
        printf("current grounded proof attack%u; all original flow/entry/summary scratch dead\n",proof_attack);
    }
    xr_compile_resources_free(snapshot.atoms);effect_body_material_free(material);
    pc_release(&context,baseline);CHECK(!live && !live_bytes);
    printf("Error equations mode%u snapshot%u; scratch/source dead; no allocation/free; physical0\n",mode,saved);
    return 0;
}
