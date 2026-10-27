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
    EffectErrorFiniteRead read={proof,work,UINT32_MAX,NULL};uint32_t at=0;
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
        changed=false;EffectErrorFiniteRead read={proof,work,UINT32_MAX,NULL};
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
        proof->rows[f]=(EffectErrorFiniteRow){.first=at,.end=at+size,
            .blocks=head->block_count,.values=values,.node=node,.returned=0};at+=size;
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
    EffectErrorFiniteRead read={proof,&context,UINT32_MAX,NULL};uint64_t result=UINT64_MAX;
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
static void finite_input_payloads(const XrXirCompileContext *context,EffectErrorFinite *proof) {
    EffectBodyFunction *head=&proof->material->functions[0];EffectBodyRun *runs=head->runs;
    uint32_t node=proof->rows[0].node;proof->rows[0].node=proof->invocations->count;
    finite_verify(context,proof,XR_XIR_BAD_STRUCTURE);proof->rows[0].node=node;
    head->runs=(EffectBodyRun *)(uintptr_t)1;finite_verify(context,proof,XR_XIR_BAD_STRUCTURE);head->runs=runs;
    XrXirStatus status=XR_XIR_OK;
    EffectBodyRun *dead=xir_compile_calloc(context,head->run_capacity,sizeof(*dead),&status);
    CHECK(dead && status==XR_XIR_OK);xr_compile_resources_free(dead);
    head->runs=dead;finite_verify(context,proof,XR_XIR_BAD_STRUCTURE);head->runs=runs;
    XrXirCompileContext foreign=*context;foreign.resources=NULL;XrCompileResourceLimits caps=pc_caps();
    CHECK(xr_compile_resources_new(&caps,&foreign.resources)==XR_COMPILE_RESOURCE_OK);
    EffectBodyRun *other=xir_compile_calloc(&foreign,head->run_capacity,sizeof(*other),&status);
    CHECK(other && status==XR_XIR_OK);head->runs=other;
    finite_verify(context,proof,XR_XIR_BAD_STRUCTURE);head->runs=runs;
    xr_compile_resources_free(other);xr_compile_resources_release(foreign.resources);
    finite_verify(context,proof,XR_XIR_OK);
    puts("finite Error current read: invalid context and arbitrary/dead/foreign Input runs rejected before access");
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
    finite_input_payloads(&context,proof);
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
/* Local assignment must replace the earlier enum seed even when a later COPY
 * reads it. All three storage forms retain the complete local transfer. */
static void finite_local_assignments(void) {
    static const XrXirOp news[]={XR_XIR_LOCAL_NEW,XR_XIR_OWNED_LOCAL_NEW,XR_XIR_SCALAR_LOCAL_NEW};
    static const XrXirOp writes[]={XR_XIR_LOCAL_WRITE,XR_XIR_OWNED_LOCAL_WRITE,XR_XIR_SCALAR_LOCAL_WRITE};
    for(uint32_t kind=0;kind<XR_COUNTOF(news);++kind) {
        EdgeFixture fixture;ee_fixture(&fixture,EE_JUMP,false);
        fixture.subject[0]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM};
        fixture.subject[1]=(XrXirInstruction){.op=news[kind],.type=(XrXirType)EE_ENUM,.args={2}};
        fixture.subject[2]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
        fixture.subject[3]=(XrXirInstruction){.op=XR_XIR_ENUM_NEW,.type=(XrXirType)EE_ENUM,.immediate=1};
        fixture.subject[4]=(XrXirInstruction){.op=writes[kind],.args={3,5}};
        fixture.subject[5]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)EE_ENUM,.args={3}};
        fixture.subject[6]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
        fixture.subject[7]=(XrXirInstruction){.op=XR_XIR_THROW,.args={7}};
        fixture.blocks[0]=(XrXirBlock){.count=7};fixture.blocks[1]=(XrXirBlock){.first=7,.count=1};
        fixture.functions[1].instruction_count=8;
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        EffectBodyMaterial *material=NULL;
        CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
        XrXirEffects seed={.resources=context.resources,.count=fixture.module.function_count};
        CHECK(effect_errors_seed(&material->domain,&seed,&context)==XR_XIR_OK);
        EffectErrorFinite *proof=finite_prepare(&context,&fixture.module,seed.outcomes_work.atoms,seed.outcomes_work.atom_count);
        CHECK(finite_build(&context,proof,&fixture.module)==XR_XIR_OK);
        finite_verify(&context,proof,XR_XIR_OK);
        EffectErrorFiniteRead plain={proof,&context,UINT32_MAX,NULL};uint64_t result=0;
        uint64_t summary=proof->rows[1].end-64;
        CHECK(error_finite_word(&plain,summary,64,&result)==XR_XIR_OK && result==8);
        uint32_t at=0;CHECK(error_finite_lower(&plain,summary+3,&at)==XR_XIR_OK && at<proof->count);
        CHECK(proof->facts[at].coordinate==summary+3);
        proof->facts[at].coordinate=summary+2;
        finite_verify(&context,proof,XR_XIR_BAD_STRUCTURE);proof->facts[at].coordinate=summary+3;
        finite_clear(proof);effect_outcomes_clear(&seed.outcomes_work,seed.count);
        effect_body_material_free(material);pc_release(&context,baseline);
    }
}
static EffectErrorFiniteView *finite_index_live_view;
static void finite_index_growth_boundary(void *memory,size_t bytes) {
    (void)memory;(void)bytes;
    CHECK(finite_index_live_view && !finite_index_live_view->work &&
        !finite_index_live_view->input.work && !finite_index_live_view->predecessors.ready);
}
/* Two live enum carriers cross the same cyclic block rows.
 * The filtered parallel arms carry distinct enum halves to the same PHI.
 * A high-numbered back edge exercises the last source bit and the general
 * path beyond one word, independently of the function's instruction offset. */
static void finite_index_fixture(EdgeFixture *fixture,XrXirBlock *blocks,uint32_t count) {
    ee_fixture(fixture,EE_FILTER,false);
    fixture->parameters[0]=(XrXirType)EE_ENUM;
    uint32_t n=0,phi=UINT32_MAX;
    for (uint32_t b=0;b<count;++b) {
        blocks[b]=(XrXirBlock){.first=n};
        if (!b) {
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_COPY,.type=(XrXirType)EE_ENUM,.args={0}};
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
        } else if (b==1) {
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={1,2}};
        } else if (b==2) {
            uint32_t tag=2+n;
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_ENUM_TAG,.type=XR_XIR_I64,.args={0}};
            uint32_t zero=2+n;
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64};
            uint32_t test=2+n;
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_EQ_INT,.type=XR_XIR_BOOL,.args={tag,zero}};
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={test},.targets={3,3}};
        } else if (b==3) {
            phi=2+n;
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)EE_ENUM,.args={0,2}};
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={5}};
        } else if (b==4) {
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_THROW,.args={phi}};
        } else if (b+1==count) {
            fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={b,4}};
        } else fixture->subject[n++]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={b+1}};
        blocks[b].count=n-blocks[b].first;
    }
    CHECK(n<EE_MAX_OPS && phi==9);
    fixture->operands[0]=2;fixture->operands[1]=0;
    fixture->functions[1].blocks=blocks;fixture->functions[1].block_count=count;
    fixture->functions[1].instruction_count=n;
    fixture->functions[1].operands=fixture->operands;fixture->functions[1].operand_count=2;
}
typedef struct FiniteEligiblePoll { bool requested;uint32_t calls; } FiniteEligiblePoll;
static bool finite_eligible_poll(const void *owner) {
    FiniteEligiblePoll *poll=(FiniteEligiblePoll *)owner;++poll->calls;return poll->requested;
}
/* Direct reader negatives use private head projections, not live Input writes.
 * Edge possibility and the canonical Phi decode succeed before the rejected
 * span, prefix, source, or predecessor projection is consumed. */
static void finite_eligible_edges(const EffectErrorEquationRead *equation) {
    CHECK(equation->head.block_count<=65 && equation->head.parameter_count==2);
    EffectErrorEdgeRead edge={2,3,equation->head.blocks[2].first+equation->head.blocks[2].count,
        EFFECT_ERROR_EDGE_NORMAL};
    EffectErrorEdgeValueRead query={9,0,equation};uint64_t expected=0,actual=0,joined=0;
    bool possible=false,original=false;
    for(uint32_t side=0;side<2;++side) {
        edge.kind=side?EFFECT_ERROR_EDGE_ALTERNATE:EFFECT_ERROR_EDGE_NORMAL;
        CHECK(error_equation_edge_word(equation,&edge,9,0,&expected,&original)==XR_XIR_OK);
        CHECK(error_equation_edge_word_read(equation,&edge,&query,&actual,&possible)==XR_XIR_OK);
        CHECK(original && possible && actual==expected && actual==(side?8u:4u));joined|=actual;
    }
    CHECK(joined==12);edge.kind=EFFECT_ERROR_EDGE_NORMAL;
    EffectErrorEquationRead other=*equation;actual=UINT64_MAX;
    CHECK(error_equation_edge_word_read(&other,&edge,&query,&actual,&possible)==XR_XIR_BAD_STRUCTURE);
    CHECK(actual==UINT64_MAX);query.eligible=&other;
    for(uint32_t kind=0;kind<4;++kind) {
        other=*equation;
        EffectBodyCurrentRead current=*equation->body.current;
        if(!kind) other.body.current=NULL;
        else if(kind==1) { current.work=NULL;other.body.current=&current; }
        else if(kind==2) other.body.read.owner=NULL;
        else other.body.type=NULL;
        CHECK(error_equation_edge_word_read(&other,&edge,&query,&actual,&possible)==XR_XIR_BAD_STRUCTURE);
        CHECK(actual==UINT64_MAX);
    }
    for(uint32_t kind=0;kind<4;++kind) {
        other=*equation;uint32_t operands[2]={2,0};XrXirBlock blocks[65];
        memcpy(blocks,equation->head.blocks,(size_t)equation->head.block_count*sizeof(*blocks));
        if(!kind) other.head.operand_count=1;
        else if(kind==1) { operands[0]=1;other.head.operands=operands; }
        else if(kind==2) { operands[1]=UINT32_MAX;other.head.operands=operands; }
        else { blocks[3].first=6;blocks[3].count=3;other.head.blocks=blocks; }
        EffectErrorFilterRead filter;XrXirInstruction phi;
        CHECK(error_equation_edge_possible(&other,&edge,&filter,&possible)==XR_XIR_OK && possible);
        CHECK(error_equation_operation(&other,7,&phi)==XR_XIR_OK && phi.op==XR_XIR_PHI);
        CHECK(error_equation_edge_word_read(&other,&edge,&query,&actual,&possible)==XR_XIR_BAD_STRUCTURE);
        CHECK(actual==UINT64_MAX);
    }
    query.eligible=NULL;
    CHECK(error_equation_edge_word_read(equation,&edge,&query,&actual,&possible)==XR_XIR_OK &&
        possible && actual==4);
    query.eligible=equation;FiniteEligiblePoll poll={0};
    CHECK(xr_compile_resources_begin_cancel(equation->work->resources,finite_eligible_poll,&poll)==XR_COMPILE_RESOURCE_OK);
    CHECK(error_equation_edge_word_read(equation,&edge,&query,&actual,&possible)==XR_XIR_OK &&
        possible && actual==4 && poll.calls);
    poll.requested=true;actual=UINT64_MAX;
    CHECK(error_equation_edge_word_read(equation,&edge,&query,&actual,&possible)==XR_XIR_BUDGET);
    CHECK(actual==UINT64_MAX);
    CHECK(xr_compile_resources_end_cancel(equation->work->resources,finite_eligible_poll,&poll)==XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(error_equation_edge_word_read(equation,&edge,&query,&actual,&possible)==XR_XIR_OK &&
        possible && actual==4);
}
typedef struct FiniteEligibleCompare { uint32_t selected,carriers;bool controls; } FiniteEligibleCompare;
/* The ordinary RHS rebuilds its own equation and never receives eligibility.
 * Literal words additionally prevent two readers sharing a wrong result. */
static XrXirStatus finite_eligible_compare(EffectErrorFiniteRead *read,const EffectErrorEquationRead *equation,
    uint32_t function,uint64_t coordinate,void *owner,bool *paused) {
    FiniteEligibleCompare *compare=owner;*paused=false;
    const EffectErrorFiniteRow *row=&read->proof->rows[function];
    uint64_t phi=error_finite_entry_coordinate(read->proof,function,3,9,0);
    bool carrier=coordinate==phi ||
        coordinate==error_finite_entry_coordinate(read->proof,function,4,2,0) ||
        coordinate==error_finite_entry_coordinate(read->proof,function,row->blocks-1,9,0);
    bool reach=coordinate==row->first,summary=coordinate==row->end-64;
    if(!carrier && !reach && !summary) return XR_XIR_OK;
    EffectErrorFiniteRead independent={read->proof,read->work,read->limit,NULL};
    uint64_t expected=0,actual=0;uint32_t expected_bits=0,actual_bits=0;
    CHECK(error_finite_rhs(&independent,function,coordinate,&expected,&expected_bits)==XR_XIR_OK);
    CHECK(error_finite_rhs_walk(read,equation,function,coordinate,&actual,&actual_bits)==XR_XIR_OK);
    CHECK(actual==expected && actual_bits==expected_bits && actual_bits==(reach?1u:64u));
    CHECK(actual==(reach?1u:read->limit==1?0u:12u));
    ++compare->selected;if(carrier) ++compare->carriers;
    if(compare->controls && coordinate==phi) finite_eligible_edges(equation);
    return XR_XIR_OK;
}
static void finite_eligible_walk(EffectErrorFiniteRead *read,bool controls) {
    CHECK(read->current && read->proof->words==1);
    FiniteEligibleCompare compare={.controls=controls};bool empty=true;
    size_t allocated=attempts;uint64_t physical=live_bytes,work=pc_stats(read->work).work;
    pc_allocate_hook=finite_no_callback;pc_free_hook=finite_no_callback;
    CHECK(error_finite_walk(read,1,finite_eligible_compare,&compare,&empty)==XR_XIR_OK && !empty);
    pc_allocate_hook=NULL;pc_free_hook=NULL;
    CHECK(compare.selected==5 && compare.carriers==3);
    CHECK(attempts==allocated && live_bytes==physical && pc_stats(read->work).work>=work);
}
static void finite_index_words(void) {
    for (uint32_t blocks_count=63;blocks_count<=65;++blocks_count) {
        EdgeFixture fixture;XrXirBlock blocks[65];
        finite_index_fixture(&fixture,blocks,blocks_count);
        XrXirCompileContext context=pc_owner(pc_caps());uint64_t baseline=pc_stats(&context).live_bytes;
        EffectBodyMaterial *material=NULL;
        CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
        XrXirEffects seed={.resources=context.resources,.count=fixture.module.function_count};
        CHECK(effect_errors_seed(&material->domain,&seed,&context)==XR_XIR_OK);
        EffectErrorFinite *proof=finite_prepare(&context,&fixture.module,
            seed.outcomes_work.atoms,seed.outcomes_work.atom_count);
        CHECK(error_finite_derive(&context,proof)==XR_XIR_OK);
        finite_verify(&context,proof,XR_XIR_OK);
        EffectErrorFiniteRead plain={proof,&context,UINT32_MAX,NULL};uint64_t result=0;
        CHECK(error_finite_word(&plain,proof->rows[1].end-64,64,&result)==XR_XIR_OK && result==12);
        CHECK(error_finite_word(&plain,error_finite_entry_coordinate(proof,1,4,2,0),64,&result)==
            XR_XIR_OK && result==12);
        EffectErrorFiniteView view={0};
        CHECK(error_finite_view_open(&context,proof,&view)==XR_XIR_OK);
        EffectErrorFiniteRead current={proof,&context,UINT32_MAX,&view};
        uint64_t coordinate=error_finite_entry_coordinate(proof,1,4,9,0),expected=0,actual=0;
        uint32_t expected_bits=0,actual_bits=0;
        CHECK(error_finite_rhs(&plain,1,coordinate,&expected,&expected_bits)==XR_XIR_OK && expected==12);
        CHECK(error_finite_rhs(&current,1,coordinate,&actual,&actual_bits)==XR_XIR_OK &&
            actual==expected && actual_bits==expected_bits);
        CHECK(blocks_count>64 || (view.predecessors.ready && view.predecessors.function==1 &&
            !view.predecessors.local_writes));
        finite_eligible_walk(&current,true);
        plain.limit=current.limit=1;
        CHECK(error_finite_rhs(&plain,1,coordinate,&expected,&expected_bits)==XR_XIR_OK && !expected);
        CHECK(error_finite_rhs(&current,1,coordinate,&actual,&actual_bits)==XR_XIR_OK && actual==expected);
        {
            EffectErrorEquationRead original,indexed;
            CHECK(error_finite_equation(&plain,1,&original)==XR_XIR_OK);
            CHECK(error_finite_equation(&current,1,&indexed)==XR_XIR_OK);
            EffectErrorFiniteEdgeQuery a={.to=4,.reach=true},b=a;
            CHECK(error_finite_predecessors(&original,&a)==XR_XIR_OK && !a.result);
            CHECK(error_finite_predecessors(&indexed,&b)==XR_XIR_OK && b.result==a.result);
        }
        finite_eligible_walk(&current,false);
        proof->capacity=proof->count;finite_index_live_view=&view;
        pc_allocate_hook=finite_index_growth_boundary;pc_free_hook=finite_index_growth_boundary;
        CHECK(error_finite_grow(&context,proof,&view)==XR_XIR_OK);
        pc_allocate_hook=NULL;pc_free_hook=NULL;finite_index_live_view=NULL;
        CHECK(!view.work && !view.input.work && !view.predecessors.ready);
        CHECK(error_finite_view_open(&context,proof,&view)==XR_XIR_OK);
        current.limit=UINT32_MAX;
        CHECK(error_finite_rhs(&current,1,coordinate,&actual,&actual_bits)==XR_XIR_OK && actual==12);
        finite_eligible_walk(&current,true);
        view=(EffectErrorFiniteView){0};
        finite_verify(&context,proof,XR_XIR_OK);
        finite_clear(proof);effect_outcomes_clear(&seed.outcomes_work,seed.count);
        effect_body_material_free(material);pc_release(&context,baseline);
    }
    puts("finite Error predecessors: 63/64/65 blocks, parallel filters, high back edges and fresh rank/growth reads");
}
#include "error_marker_type_cases.h"
#include "xir_error_finite_growth_cases.h"
#include "xir_error_edge_identity_cases.h"
#ifndef FINITE_PROOF_ENTRY
#define FINITE_PROOF_ENTRY main
#endif
int FINITE_PROOF_ENTRY(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"edge-identity")) { finite_edge_identity_cases();return 0; }
    CHECK(argc==2);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);
    if(!mode) { finite_local_assignments();finite_index_words(); }
    if(mode>=31) { CHECK(mode<35);finite_growth_case(mode-31); }
    else if(mode>=15) marker_case(mode-15);
    else if(mode==14) finite_cycle();else { CHECK(mode<14);finite_cfg(mode,false);finite_cfg(mode,true); }
    return 0;
}
