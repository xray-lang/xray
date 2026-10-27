/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_edge_identity_cases.h - Direct current edge identity regressions
 *
 * KEY CONCEPT:
 *   Raw Input, complete decoding and literal edge results independently bound
 *   a local projection. Each fee boundary owns a fresh test ledger.
 */
#ifndef XIR_ERROR_EDGE_IDENTITY_CASES_H
#define XIR_ERROR_EDGE_IDENTITY_CASES_H

static bool edge_identity_full_equal(const XrXirInstruction *a,const XrXirInstruction *b) {
    return a->op==b->op && a->type==b->type && a->args[0]==b->args[0] &&
        a->args[1]==b->args[1] && a->targets[0]==b->targets[0] && a->targets[1]==b->targets[1] &&
        a->immediate==b->immediate && a->type_arguments[0]==b->type_arguments[0] &&
        a->type_arguments[1]==b->type_arguments[1];
}

/* These rows test the physical reader, not independent semantic admission.
 * Unused columns deliberately vary and must still belong to the live frame.
 * The scalar JUMP type and branch condition stay valid during construction. */
static void edge_identity_fixture(EdgeFixture *f,unsigned pattern) {
    ee_fixture(f,EE_JUMP,false);
    for(uint32_t i=0;i<3;++i) {
        f->subject[i]=(XrXirInstruction){.op=XR_XIR_JUMP,.type=XR_XIR_UNIT,.args={1,17},
            .targets={1,2},.immediate=INT64_C(0x1234567812345678),.type_arguments={31,47}};
        if(pattern) {
            f->subject[i].args[0]+=i;f->subject[i].args[1]+=2*i;
            f->subject[i].targets[0]=i;f->subject[i].targets[1]=2-i;
            f->subject[i].immediate+=(int64_t)(UINT64_C(0x100000001)*i);
            f->subject[i].type_arguments[0]+=i;f->subject[i].type_arguments[1]+=3*i;
        }
        f->blocks[i]=(XrXirBlock){.first=i,.count=1};
    }
    if(pattern==2) {
        f->subject[0].op=XR_XIR_JUMP;f->subject[1].op=XR_XIR_BRANCH;
        f->subject[2].op=XR_XIR_JUMP;
        for(uint32_t i=0;i<3;++i) { f->subject[i].type=XR_XIR_UNIT;f->subject[i].args[0]=1; }
    }
    f->functions[1].instruction_count=3;f->functions[1].block_count=3;
}
static void edge_identity_open(const XrXirCompileContext *work,EdgeFixture *f,
    EffectBodyMaterial **material,EffectBodyCurrentRead *current,EffectErrorEquationRead *read) {
    CHECK(effect_body_material_create(work,&f->module,material)==XR_XIR_OK);
    CHECK(effect_body_current_open(work,*material,current)==XR_XIR_OK);
    *read=(EffectErrorEquationRead){.body=effect_body_owned_request(work,*material),
        .work=work,.function=1};
    read->body.current=current;
    CHECK(effect_body_request_head(&read->body,1,&read->head)==XR_XIR_OK);
}
static void edge_identity_close(XrXirCompileContext *work,EffectBodyMaterial *material,
    EffectBodyCurrentRead *current,uint64_t baseline) {
    *current=(EffectBodyCurrentRead){0};
    effect_body_material_free(material);pc_release(work,baseline);
}
static void edge_identity_fields(const XrXirInstruction *value,const XrXirInstruction *raw,
    uint32_t side) {
    CHECK(value->op==raw->op && value->args[0]==raw->args[0] &&
        value->targets[side]==raw->targets[side]);
}

/* Parameters preserve the ABI-specific fee oracle without a constant
 * controlling expression in the strict MSVC build. */
static bool edge_identity_reference_layout(size_t pointer_bytes,size_t instruction_bytes,size_t opcode_bytes) {
    return pointer_bytes==8 && instruction_bytes==40 && opcode_bytes==4;
}

/* The reference is measured before any oracle read. Later fresh ledgers repeat
 * only that setup and helper, so an exact/minus-one cap cannot hide work. */
static void edge_identity_fee_sample(unsigned pattern,uint32_t instruction,uint32_t side,
    uint64_t *setup,uint64_t *fee) {
    EdgeFixture f;edge_identity_fixture(&f,pattern);
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead read;
    edge_identity_open(&work,&f,&material,&current,&read);
    CHECK(material->functions[1].runs[0].count>=2);
    if(pattern==2) CHECK(material->functions[1].runs[0].mask&1);
    *setup=pc_stats(&work).work;XrXirInstruction projected={0},full={0},raw={0};
    size_t allocated=attempts;uint64_t physical=live_bytes;
    pc_allocate_hook=finite_no_callback;pc_free_hook=finite_no_callback;
    CHECK(error_equation_edge_identity(&read,instruction,side,&projected)==XR_XIR_OK);
    *fee=pc_stats(&work).work-*setup;CHECK(*fee);
    uint64_t before=pc_stats(&work).work;
    CHECK(error_equation_operation(&read,instruction,&full)==XR_XIR_OK);
    uint64_t full_fee=pc_stats(&work).work-before;
    CHECK(full_fee>*fee);
    if(edge_identity_reference_layout(sizeof(void *),sizeof(XrXirInstruction),sizeof(XrXirOp)))
        CHECK(full_fee-*fee>=30);
    EffectErrorEquationRead independent=read;
    independent.body=effect_body_raw_request(&work,&f.module);
    CHECK(error_equation_operation(&independent,instruction,&raw)==XR_XIR_OK);
    CHECK(edge_identity_full_equal(&raw,&f.subject[instruction]));
    CHECK(edge_identity_full_equal(&full,&raw));edge_identity_fields(&projected,&raw,side);
    CHECK(attempts==allocated && live_bytes==physical);
    pc_allocate_hook=NULL;pc_free_hook=NULL;
    edge_identity_close(&work,material,&current,baseline);
}
static void edge_identity_fee_boundary(unsigned pattern,uint32_t instruction,uint32_t side,
    uint64_t setup,uint64_t fee,bool less) {
    XrCompileResourceLimits caps=pc_caps();CHECK(setup<=UINT64_MAX-fee && fee>1);
    caps.work=setup+fee-(less?1u:0u);
    EdgeFixture f;edge_identity_fixture(&f,pattern);
    XrXirCompileContext work=pc_owner(caps);uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead read;
    edge_identity_open(&work,&f,&material,&current,&read);
    CHECK(pc_stats(&work).work==setup);
    XrXirInstruction value;memset(&value,0xA5,sizeof(value));
    XrXirStatus status=error_equation_edge_identity(&read,instruction,side,&value);
    if(less) CHECK(status==XR_XIR_BUDGET && value.targets[side]==UINT32_C(0xA5A5A5A5));
    else {
        CHECK(status==XR_XIR_OK && pc_stats(&work).work==caps.work);
        edge_identity_fields(&value,&f.subject[instruction],side);
    }
    edge_identity_close(&work,material,&current,baseline);
}
static void edge_identity_fees(void) {
    for(unsigned pattern=0;pattern<3;++pattern) for(uint32_t i=0;i<2;++i)
        for(uint32_t side=0;side<2;++side) {
            uint64_t setup=0,fee=0;edge_identity_fee_sample(pattern,i,side,&setup,&fee);
            edge_identity_fee_boundary(pattern,i,side,setup,fee,false);
            edge_identity_fee_boundary(pattern,i,side,setup,fee,true);
        }
}

static uint32_t edge_identity_delegated_calls;
static XrXirStatus edge_identity_delegated(const EffectBodyRequest *request,uint32_t function,
    uint32_t instruction,XrXirInstruction *storage,const XrXirInstruction **output,
    EffectBodyInstructionFields *fields) {
    ++edge_identity_delegated_calls;
    return effect_body_owned_instruction(request,function,instruction,storage,output,fields);
}
static void edge_identity_adapters(void) {
    EdgeFixture f;edge_identity_fixture(&f,1);
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead read;
    edge_identity_open(&work,&f,&material,&current,&read);
    for(unsigned kind=0;kind<2;++kind) {
        EffectErrorEquationRead other=read;
        if(!kind) other.body.current=NULL;else other.body.instruction=edge_identity_delegated;
        XrXirInstruction actual={0},full={0};edge_identity_delegated_calls=0;
        uint64_t before=pc_stats(&work).work;
        CHECK(error_equation_operation(&other,1,&full)==XR_XIR_OK);
        uint64_t expected=pc_stats(&work).work-before;before=pc_stats(&work).work;
        CHECK(error_equation_edge_identity(&other,1,1,&actual)==XR_XIR_OK);
        CHECK(pc_stats(&work).work-before==expected);
        CHECK(edge_identity_full_equal(&actual,&full) && edge_identity_full_equal(&actual,&f.subject[1]));
        CHECK(edge_identity_delegated_calls==(kind?2u:0u));
    }
    XrXirCompileContext copy=work;
    for(unsigned kind=0;kind<5;++kind) {
        EffectErrorEquationRead other=read;EffectBodyCurrentRead view=current;
        other.body.current=&view;
        if(!kind) view.work=NULL;else if(kind==1) view.material=NULL;
        else if(kind==2) other.body.read.owner=NULL;else if(kind==3) other.work=&copy;
        else other.body.owner=NULL;
        XrXirInstruction actual,saved;memset(&actual,0xA5,sizeof(actual));saved=actual;
        CHECK(error_equation_edge_identity(&other,1,1,&actual)==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&actual,&saved,sizeof(actual)));
    }
    FiniteEligiblePoll poll={0};XrXirInstruction actual={0};
    CHECK(xr_compile_resources_begin_cancel(work.resources,finite_eligible_poll,&poll)==XR_COMPILE_RESOURCE_OK);
    CHECK(error_equation_edge_identity(&read,1,1,&actual)==XR_XIR_OK && poll.calls);
    poll.requested=true;memset(&actual,0xA5,sizeof(actual));
    CHECK(error_equation_edge_identity(&read,1,1,&actual)==XR_XIR_BUDGET);
    CHECK(actual.targets[1]==UINT32_C(0xA5A5A5A5));poll.requested=false;
    CHECK(error_equation_edge_identity(&read,1,1,&actual)==XR_XIR_BUDGET);
    CHECK(xr_compile_resources_end_cancel(work.resources,finite_eligible_poll,&poll)==XR_COMPILE_RESOURCE_CANCELLED);
    CHECK(error_equation_edge_identity(&read,1,1,&actual)==XR_XIR_OK);
    edge_identity_fields(&actual,&f.subject[1],1);
    edge_identity_close(&work,material,&current,baseline);
}
/* Qualified existing fixtures provide real CALL/CLEANUP metadata. The raw
 * reader and literal edge kinds remain independent of the projected columns. */
static void edge_identity_terminals(void) {
    static const EdgeCase modes[]={EE_JUMP,EE_BRANCH,EE_INVOKE,EE_CLEANUP,EE_JUMP};
    for(size_t n=0;n<sizeof(modes)/sizeof(*modes);++n) {
        EdgeFixture f;ee_fixture(&f,modes[n],false);
        if(n==4) f.subject[4].op=XR_XIR_RETURN;
        XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
        EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead read;
        edge_identity_open(&work,&f,&material,&current,&read);
        EffectErrorEquationRead raw=read;raw.body=effect_body_raw_request(&work,&f.module);
        CHECK(effect_body_request_head(&raw.body,1,&raw.head)==XR_XIR_OK);
        uint32_t end=f.blocks[0].first+f.blocks[0].count;
        const XrXirInstruction *literal=&f.subject[end-1];
        for(uint32_t side=0;side<2;++side) {
            EffectErrorEdgeRead edge={0,literal->targets[side],end,
                side?EFFECT_ERROR_EDGE_ALTERNATE:EFFECT_ERROR_EDGE_NORMAL};
            EffectErrorFilterRead a,b;bool exceptional=false,raw_exceptional=false;
            XrXirInstruction projected={0},full={0};
            XrXirStatus expected=error_equation_edge_classify(&raw,&edge,&full,&a,&raw_exceptional);
            XrXirStatus actual=error_equation_edge_classify(&read,&edge,&projected,&b,&exceptional);
            bool single=n==0 || n==3,unknown=n==4;
            CHECK(expected==(unknown || (single && side)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
            CHECK(actual==expected);
            if(actual==XR_XIR_OK) {
                edge_identity_fields(&projected,literal,side);
                CHECK(edge_identity_full_equal(&full,literal));
                CHECK(exceptional==raw_exceptional && exceptional==(n==2 && side!=0));
                CHECK(a.possible && b.possible && !a.recognized && !b.recognized);
            }
        }
        for(unsigned bad=0;bad<4;++bad) {
            EffectErrorEdgeRead edge={0,0,end,EFFECT_ERROR_EDGE_NORMAL};
            if(bad==1) { edge.to=1;--edge.cursor; }
            else if(bad==2) edge.from=read.head.block_count;
            else if(bad==3) edge.kind=(EffectErrorEdgeKind)3;
            EffectErrorFilterRead filter;bool possible=true;
            CHECK(error_equation_edge_possible(&read,&edge,&filter,&possible)==XR_XIR_BAD_STRUCTURE && possible);
        }
        edge_identity_close(&work,material,&current,baseline);
    }
}
static void edge_identity_panic(void) {
    EdgeFixture f;ee_fixture(&f,EE_PANIC,false);
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;EffectBodyCurrentRead current={0};EffectErrorEquationRead read;
    edge_identity_open(&work,&f,&material,&current,&read);
    const XrXirBlock *from=&read.head.blocks[1];CHECK(from->panic==2 && from->count);
    EffectErrorEdgeRead edge={1,2,from->first,EFFECT_ERROR_EDGE_PANIC};
    XrXirInstruction terminal,saved;memset(&terminal,0xA5,sizeof(terminal));saved=terminal;
    EffectErrorFilterRead filter;bool exceptional=true;
    CHECK(error_equation_edge_classify(&read,&edge,&terminal,&filter,&exceptional)==XR_XIR_OK);
    CHECK(!memcmp(&terminal,&saved,sizeof(terminal)) && !exceptional);
    edge_identity_close(&work,material,&current,baseline);
}

/* Corruption happens before opening any borrowed view. No test mutates a live
 * framed payload or treats a successful earlier frame as cached permission. */
static void edge_identity_unused_frame(void) {
    EdgeFixture f;edge_identity_fixture(&f,1);
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;CHECK(effect_body_material_create(&work,&f.module,&material)==XR_XIR_OK);
    EffectBodyRun *run=&material->functions[1].runs[0];
    CHECK(run->count>=2);
    static const uint32_t omitted[]={1,3,5,6,7,8,9};
    for(size_t i=0;i<sizeof(omitted)/sizeof(*omitted);++i) {
        uint32_t word=omitted[i],mask=run->mask;bool selected=!!(mask&(UINT32_C(1)<<word));
        uint32_t delta=selected?run->delta[word]:0;
        if(selected) CHECK(delta);
        run->mask=mask|(UINT32_C(1)<<word);run->delta[word]=0;
        EffectBodyCurrentRead current={0};
        CHECK(effect_body_current_open(&work,material,&current)==XR_XIR_BAD_STRUCTURE);
        CHECK(!current.work && !current.material);
        if(selected) run->delta[word]=delta;
        run->mask=mask;
    }
    uint32_t mask=run->mask;run->mask|=UINT32_C(1)<<10;EffectBodyCurrentRead current={0};
    CHECK(effect_body_current_open(&work,material,&current)==XR_XIR_BAD_STRUCTURE);run->mask=mask;
    uint32_t count=run->count;run->count=1;
    CHECK(effect_body_current_open(&work,material,&current)==XR_XIR_BAD_STRUCTURE);run->count=count;
    XrXirOp op=run->base.op;run->base.op=XR_XIR_INVALID;
    CHECK(effect_body_current_open(&work,material,&current)==XR_XIR_BAD_STRUCTURE);run->base.op=op;
    CHECK(!current.work && !current.material);
    CHECK(effect_body_current_open(&work,material,&current)==XR_XIR_OK);
    edge_identity_close(&work,material,&current,baseline);
}
static void edge_identity_parallel(void) {
    EdgeFixture f;XrXirBlock blocks[6];finite_index_fixture(&f,blocks,6);
    XrXirCompileContext work=pc_owner(pc_caps());uint64_t baseline=pc_stats(&work).live_bytes;
    EffectBodyMaterial *material=NULL;CHECK(effect_body_material_create(&work,&f.module,&material)==XR_XIR_OK);
    XrXirEffects seed={.resources=work.resources,.count=f.module.function_count};
    CHECK(effect_errors_seed(&material->domain,&seed,&work)==XR_XIR_OK);
    EffectErrorFinite *proof=finite_prepare(&work,&f.module,seed.outcomes_work.atoms,seed.outcomes_work.atom_count);
    CHECK(error_finite_derive(&work,proof)==XR_XIR_OK);finite_verify(&work,proof,XR_XIR_OK);
    EffectErrorFiniteView view={0};CHECK(error_finite_view_open(&work,proof,&view)==XR_XIR_OK);
    EffectErrorFiniteRead plain={proof,&work,UINT32_MAX,NULL},current={proof,&work,UINT32_MAX,&view};
    EffectErrorEquationRead raw,indexed;
    CHECK(error_finite_equation(&plain,1,&raw)==XR_XIR_OK);
    CHECK(error_finite_equation(&current,1,&indexed)==XR_XIR_OK);
    uint32_t end=blocks[2].first+blocks[2].count;
    for(uint32_t side=0;side<2;++side) {
        EffectErrorEdgeRead edge={2,3,end,side?EFFECT_ERROR_EDGE_ALTERNATE:EFFECT_ERROR_EDGE_NORMAL};
        XrXirInstruction identity={0},complete={0};
        CHECK(error_equation_edge_identity(&indexed,end-1,side,&identity)==XR_XIR_OK);
        CHECK(error_equation_operation(&raw,end-1,&complete)==XR_XIR_OK);
        CHECK(edge_identity_full_equal(&complete,&f.subject[end-1]));
        edge_identity_fields(&identity,&complete,side);
        uint64_t expected=0,actual=0;bool a=false,b=false;
        CHECK(error_equation_edge_word(&raw,&edge,9,0,&expected,&a)==XR_XIR_OK);
        CHECK(error_equation_edge_word(&indexed,&edge,9,0,&actual,&b)==XR_XIR_OK);
        CHECK(a && b && expected==actual && actual==(side?8u:4u));
    }
    error_finite_view_reset(&view);finite_clear(proof);
    effect_outcomes_clear(&seed.outcomes_work,seed.count);effect_body_material_free(material);
    pc_release(&work,baseline);
}
static void finite_edge_identity_cases(void) {
    CHECK(!live && !live_bytes && !pc_allocate_hook && !pc_free_hook);
    edge_identity_terminals();edge_identity_panic();edge_identity_fees();edge_identity_adapters();
    edge_identity_unused_frame();edge_identity_parallel();
    CHECK(!live && !live_bytes && !pc_allocate_hook && !pc_free_hook);
    puts("finite edge identity: raw/full, adapters, exact fees, framing, cancellation and physical release PASS");
}
#endif // XIR_ERROR_EDGE_IDENTITY_CASES_H
