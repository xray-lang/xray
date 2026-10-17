/* Current Error closure and finite support are independent obligations. */
#define main pending_fixture_entry
#include "test_xir_error_cfg_pending.c"
#undef main

typedef struct FiniteProposedProof { EffectErrorFinite proof;XrXirEffects *owner; } FiniteProposedProof;
static void finite_clear(EffectErrorFinite *proof) {
    FiniteProposedProof *proposed=(FiniteProposedProof *)proof;
    xr_xir_compile_effects_free(proposed->owner);
    xr_compile_resources_free(proof->rows);xr_compile_resources_free(proof->facts);
    xr_compile_resources_free(proof);
}
static XrXirStatus finite_insert(const XrXirCompileContext *work,EffectErrorFinite *proof,
    uint64_t coordinate,uint32_t rank,bool *changed) {
    EffectErrorFiniteRead read={proof,work,UINT32_MAX};uint32_t at=0;
    XrXirStatus lookup=error_finite_lower(&read,coordinate,&at);if(lookup!=XR_XIR_OK) return lookup;
    if(at<proof->count && proof->facts[at].coordinate==coordinate) return XR_XIR_OK;
    if(proof->count==proof->capacity) {
        uint32_t capacity=proof->capacity?proof->capacity*2:32;XrXirStatus status=XR_XIR_OK;
        EffectErrorFiniteFact *facts=xir_compile_alloc(work,(size_t)capacity*sizeof(*facts),&status);
        if(!facts) return status;
        if(!xir_compile_work(work,(uint64_t)proof->count*sizeof(*facts))) {
            xr_compile_resources_free(facts);return XR_XIR_BUDGET;
        }
        if(proof->count) memcpy(facts,proof->facts,(size_t)proof->count*sizeof(*facts));
        xr_compile_resources_free(proof->facts);proof->facts=facts;proof->capacity=capacity;
    }
    if(!xir_compile_work(work,(uint64_t)(proof->count-at)*sizeof(*proof->facts)+sizeof(*proof->facts)))
        return XR_XIR_BUDGET;
    memmove(proof->facts+at+1,proof->facts+at,(size_t)(proof->count-at)*sizeof(*proof->facts));
    proof->facts[at]=(EffectErrorFiniteFact){coordinate,rank};++proof->count;*changed=true;return XR_XIR_OK;
}
/* Raw successor fields choose an evaluation order, never a permission. Every
 * physical block is still visited, including disconnected and cyclic blocks. */
static XrXirStatus finite_block_order(const XrXirCompileContext *work,
    const XrXirFunction *function,uint32_t order[EE_BLOCKS]) {
    if(!function->block_count || function->block_count>EE_BLOCKS) return XR_XIR_BAD_STRUCTURE;
    bool seen[EE_BLOCKS]={false};uint32_t count=1;order[0]=0;seen[0]=true;
    for(uint32_t at=0;at<function->block_count;++at) {
        if(at==count) for(uint32_t b=0;b<function->block_count;++b) if(!seen[b]) {
            order[count++]=b;seen[b]=true;break;
        }
        const XrXirBlock *block=&function->blocks[order[at]];
        if(!block->count || block->first>function->instruction_count ||
            block->count>function->instruction_count-block->first) return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *end=&function->instructions[block->first+block->count-1];
        uint32_t successors[3]={end->targets[0],end->targets[1],block->panic};
        for(uint32_t i=0;i<3;++i) {
            if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
            uint32_t b=successors[i];
            if(b<function->block_count && !seen[b]) { order[count++]=b;seen[b]=true; }
        }
    }
    return count==function->block_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus finite_build(const XrXirCompileContext *work,EffectErrorFinite *proof,const XrXirModule *raw) {
    bool changed=true;
    while(changed) {
        changed=false;EffectErrorFiniteRead read={proof,work,UINT32_MAX};
        for(uint32_t f=0;f<proof->functions;++f) {
            /* The independent dense oracle classifies the raw source array.
             * It never consumes the production canonical run walker. */
            bool empty=true;CHECK(raw && f<raw->function_count);
            for(uint32_t i=0;i<raw->functions[f].instruction_count;++i) {
                XrXirOp op=raw->functions[f].instructions[i].op;
                CHECK(op>XR_XIR_INVALID && op<XR_XIR_OP_COUNT);
                if(!effect_physical_error_empty_opcode(op)) empty=false;
            }
            if(empty) continue;
            const EffectErrorFiniteRow *row=&proof->rows[f];
            uint32_t order[EE_BLOCKS];
            XrXirStatus ordering=finite_block_order(work,&raw->functions[f],order);
            if(ordering!=XR_XIR_OK) return ordering;
            uint64_t summary=row->end-(uint64_t)proof->words*64;
            for(uint32_t region=0;region<3;++region) for(uint32_t at=0;at<(region==2?1u:row->blocks);++at) {
                uint64_t first=region==2?summary:region==0?row->first+order[at]:
                    row->first+row->blocks+(uint64_t)order[at]*row->values*proof->words*64;
                uint64_t end=region==2?row->end:region==0?first+1:first+(uint64_t)row->values*proof->words*64;
                for(uint64_t coordinate=first;coordinate<end;) {
                    if(coordinate>=row->first+row->blocks && coordinate<summary) {
                        uint64_t offset=(coordinate-row->first-row->blocks)/((uint64_t)proof->words*64);
                        uint32_t value=(uint32_t)(offset%row->values);
                        const XrXirFunction *function=&raw->functions[f];
                        if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
                        XrXirType type=XR_XIR_UNIT;bool forced=false;
                        if(value<function->parameter_count) type=function->parameters[value];
                        else {
                            const XrXirInstruction *op=&function->instructions[value-function->parameter_count];
                            type=op->type;forced=op->op==XR_XIR_GO || op->op==XR_XIR_INVOKE_ERROR;
                        }
                        /* Closed primitive values have no Error carrier in any
                         * CFG entry. GO and exceptional result rows carry Error
                         * independently of their declared physical scalar type.
                         * This oracle reads raw arrays, never canonical runs. */
                        if(!forced && (type==XR_XIR_UNIT || type==XR_XIR_I64 || type==XR_XIR_BOOL)) {
                            coordinate+=(uint64_t)proof->words*64;continue;
                        }
                    }
                    uint64_t result=0;uint32_t bits=0;
                    XrXirStatus status=error_finite_rhs(&read,f,coordinate,&result,&bits);
                    if(status!=XR_XIR_OK) return status;
                    for(uint32_t bit=0;bit<bits;++bit) if(result&(UINT64_C(1)<<bit)) {
                        /* Every inserted bit follows all currently read causes.
                         * The receiver still checks its strictly smaller support. */
                        if(proof->count>=UINT32_MAX-1) return XR_XIR_BUDGET;
                        status=finite_insert(work,proof,coordinate+bit,proof->count+1,&changed);
                        if(status!=XR_XIR_OK) return status;
                    }
                    coordinate+=bits;
                }
            }
        }
    }
    return XR_XIR_OK;
}
/* The proposed dense Error facts stay independent of the sparse constructor.
 * Their topology now borrows a genuine invocation Final produced by the common
 * constructor from this exact raw fixture, rather than fabricated node rows. */
static EffectErrorFinite *finite_prepare(const XrXirCompileContext *work,
    const XrXirModule *raw,const EffectErrorAtom *atoms,uint32_t count) {
    XrXirStatus status=XR_XIR_OK;XrXirEffects *owner=NULL;
    CHECK(effect_infer_construct(work,raw,NULL,&owner)==XR_XIR_OK && owner && owner->invocations);
    FiniteProposedProof *proposed=xir_compile_calloc(work,1,sizeof(*proposed),&status);
    CHECK(proposed && status==XR_XIR_OK);proposed->owner=owner;
    EffectErrorFinite *proof=&proposed->proof;
    const EffectBodyMaterial *material=owner->invocations->body;
    proof->resources=work->resources;proof->material=material;proof->atoms=atoms;proof->atom_count=count;
    proof->words=(count+65)/64;proof->functions=effect_body_count(material);
    proof->invocations=owner->invocations->final;
    CHECK(error_finite_context_count(work,proof,&proof->contexts,&proof->contextual)==XR_XIR_OK);
    CHECK(proof->contexts==proof->functions);
    proof->rows=xir_compile_calloc(work,proof->contexts,sizeof(*proof->rows),&status);CHECK(proof->rows);
    uint64_t at=0;
    for(uint32_t f=0;f<proof->contexts;++f) {
        uint32_t node=UINT32_MAX;CHECK(error_finite_context_node(work,proof,f,&node)==XR_XIR_OK);
        const EffectBodyFunction *head=&material->functions[proof->invocations->nodes[node].body];
        uint32_t values=head->parameter_count+head->instruction_count;
        uint64_t size=head->block_count+(uint64_t)head->block_count*values*proof->words*64+(uint64_t)proof->words*64;
        proof->rows[f]=(EffectErrorFiniteRow){at,at+size,head->block_count,values,node};at+=size;
    }
    return proof;
}
static void finite_no_callback(void *memory,size_t bytes) { (void)memory;(void)bytes;CHECK(false); }
static void finite_verify(const XrXirCompileContext *work,EffectErrorFinite *proof,XrXirStatus expected) {
    size_t before=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=finite_no_callback;pc_free_hook=finite_no_callback;
    XrXirStatus actual=error_finite_check(work,proof);
    pc_allocate_hook=NULL;pc_free_hook=NULL;
    if(actual!=expected) fprintf(stderr,"finite actual%u expected%u work%llu facts%u\n",actual,expected,
        (unsigned long long)pc_stats(work).work,proof->count);
    CHECK(actual==expected && before==attempts && physical==live_bytes);
}
static void finite_cfg(unsigned mode,bool saved) {
    EdgeFixture fixture;if(mode<10) pc_fixture(&fixture,(PendingCase)mode,saved);
    else pc_unit_cell_fixture(&fixture,mode-10);
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    EffectBodyMaterial *material=NULL;CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    XrXirEffects seed={.resources=context.resources,.count=fixture.module.function_count};
    CHECK(effect_errors_seed(&material->domain,&seed,&context)==XR_XIR_OK);
    EffectErrorFinite *proof=finite_prepare(&context,&fixture.module,seed.outcomes_work.atoms,seed.outcomes_work.atom_count);
    XrXirStatus status=finite_build(&context,proof,&fixture.module);
    if(status!=XR_XIR_OK) fprintf(stderr,"finite build mode%u saved%u status%u work%llu\n",mode,saved,status,
        (unsigned long long)pc_stats(&context).work);
    CHECK(status==XR_XIR_OK);finite_verify(&context,proof,XR_XIR_OK);
    EffectErrorFiniteRead read={proof,&context,UINT32_MAX};uint64_t result=UINT64_MAX;
    CHECK(error_finite_word(&read,proof->rows[1].end-64,64,&result)==XR_XIR_OK);
    CHECK(result==(mode<10?pc_expected((PendingCase)mode,saved):0));
    memset(&fixture,0xDC,sizeof(fixture));
    finite_verify(&context,proof,XR_XIR_OK);
    CHECK(proof->count);uint32_t rank=proof->facts[0].rank;proof->facts[0].rank=0;
    finite_verify(&context,proof,XR_XIR_BAD_STRUCTURE);proof->facts[0].rank=rank;
    uint32_t at=0;CHECK(error_finite_lower(&read,proof->rows[1].end-64,&at)==XR_XIR_OK);
    if(result) {
        CHECK(at<proof->count);EffectErrorFiniteFact saved_fact=proof->facts[at];
        memmove(proof->facts+at,proof->facts+at+1,(size_t)(proof->count-at-1)*sizeof(*proof->facts));--proof->count;
        finite_verify(&context,proof,XR_XIR_BAD_STRUCTURE);
        memmove(proof->facts+at+1,proof->facts+at,(size_t)(proof->count-at)*sizeof(*proof->facts));
        proof->facts[at]=saved_fact;++proof->count;
    }
    finite_verify(&context,proof,XR_XIR_OK);finite_clear(proof);
    effect_outcomes_clear(&seed.outcomes_work,seed.count);effect_body_material_free(material);
    pc_release(&context,baseline);CHECK(!live && !live_bytes);
    printf("finite Error CFG mode%u saved%u: literal/current Input; closure/rootedness; no callbacks; physical0\n",mode,saved);
}
static void finite_cycle(void) {
    XrXirInstruction ops[2][2]={
        {{.op=XR_XIR_CALL,.immediate=1},{.op=XR_XIR_RETURN}},
        {{.op=XR_XIR_CALL,.immediate=0},{.op=XR_XIR_RETURN}}};
    XrXirBlock blocks[2]={{.count=2},{.count=2}};
    XrXirFunction functions[2]={{.instructions=ops[0],.instruction_count=2,.blocks=&blocks[0],.block_count=1},
        {.instructions=ops[1],.instruction_count=2,.blocks=&blocks[1],.block_count=1}};
    XrXirModule module={.functions=functions,.function_count=2};
    XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
    EffectBodyMaterial *material=NULL;CHECK(effect_body_material_create(&context,&module,&material)==XR_XIR_OK);
    EffectErrorFinite *proof=finite_prepare(&context,&module,NULL,0);
    CHECK(finite_build(&context,proof,&module)==XR_XIR_OK);finite_verify(&context,proof,XR_XIR_OK);
    bool changed=false;
    CHECK(finite_insert(&context,proof,proof->rows[0].end-64+1,3,&changed)==XR_XIR_OK);
    CHECK(finite_insert(&context,proof,proof->rows[1].end-64+1,3,&changed)==XR_XIR_OK);
    /* Both summaries satisfy their cyclic equations; neither has finite support. */
    finite_verify(&context,proof,XR_XIR_BAD_STRUCTURE);
    for(uint32_t i=0;i<proof->count;++i) if(proof->facts[i].coordinate==proof->rows[0].end-64+1)
        proof->facts[i].rank=2;
    finite_verify(&context,proof,XR_XIR_BAD_STRUCTURE);
    finite_clear(proof);effect_body_material_free(material);pc_release(&context,baseline);
    puts("finite Error direct recursion: empty least solution; jointly consistent fake positive cycle rejected");
}
#include "error_marker_type_cases.h"
#ifndef FINITE_PROOF_ENTRY
#define FINITE_PROOF_ENTRY main
#endif
int FINITE_PROOF_ENTRY(int argc,char **argv) {
    CHECK(argc==2);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);
    if(mode>=15) { CHECK(mode<31);marker_case(mode-15); } else if(mode==14) finite_cycle();else { CHECK(mode<14);finite_cfg(mode,false);finite_cfg(mode,true); }
    return 0;
}
