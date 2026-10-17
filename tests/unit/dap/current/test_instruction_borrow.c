/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_instruction_borrow.c - Borrow immutable physical rows without decoder copies
 */
#define main borrow_original_checks
#include "../../xir/test_xir_root_conditional_helpers.c"
#undef main
static bool borrow_same(const XrXirInstruction *a,const XrXirInstruction *b) {
    return a->op==b->op && a->type==b->type && a->args[0]==b->args[0] && a->args[1]==b->args[1] &&
        a->targets[0]==b->targets[0] && a->targets[1]==b->targets[1] && a->immediate==b->immediate &&
        a->type_arguments[0]==b->type_arguments[0] && a->type_arguments[1]==b->type_arguments[1];
}
typedef struct BorrowReadBudget { uint64_t remaining,used; } BorrowReadBudget;
static bool borrow_consume(const void *owner,uint64_t units) {
    BorrowReadBudget *budget=(BorrowReadBudget *)owner;
    if (units>budget->remaining) return false;
    budget->remaining-=units;budget->used+=units;return true;
}
static void borrow_fields_change(XrXirInstruction *op,uint32_t word) {
    switch (word) {
    case 0:op->op=XR_XIR_CONST_FLOAT;break;case 1:op->type=XR_XIR_F64;break;
    case 2:op->args[0]=19;break;case 3:op->args[1]=23;break;
    case 4:op->targets[0]=29;break;case 5:op->targets[1]=31;break;
    case 6:op->immediate=43;break;case 7:op->immediate=INT64_MIN+42;break;
    case 8:op->type_arguments[0]=37;break;case 9:op->type_arguments[1]=41;break;
    default:CHECK(false);break;
    }
}
/* Private physical mutations are negative correspondence inputs. They never
 * enter a Source producer or receive a Cell/ROOT/execution permission. */
static void borrow_words(const XrXirCompileContext *context,EffectBodyMaterial *material,
    const XrXirFunction *source) {
    EffectBodyRequest request=effect_body_owned_request(context,material);
    EffectBodyFunction *function=&material->functions[0];XrXirInstruction preserved=function->runs[0].base;
    CHECK(effect_body_function_match(context,function,source)==XR_XIR_OK);
    for (uint32_t word=0;word<10;++word) {
        borrow_fields_change(&function->runs[0].base,word);
        XrXirInstruction strict={0},storage={0};const XrXirInstruction *observed=NULL;
        CHECK(effect_body_instruction_get(&request.read,function,0,&strict)==XR_XIR_OK);
        CHECK(effect_body_request_instruction(&request,0,0,&storage,&observed)==XR_XIR_OK);
        CHECK(observed==&function->runs[0].base && borrow_same(observed,&strict));
        CHECK(effect_body_function_match(context,function,source)!=XR_XIR_OK);
        function->runs[0].base=preserved;
    }
    CHECK(effect_body_function_match(context,function,source)==XR_XIR_OK);
    uint32_t mask=function->runs[0].mask;function->runs[0].mask=UINT32_C(1024);
    XrXirInstruction sentinel={.op=XR_XIR_CONST_INT,.immediate=71},storage={0};const XrXirInstruction *out=&sentinel;
    CHECK(effect_body_request_instruction(&request,0,0,&storage,&out)==XR_XIR_BAD_STRUCTURE && out==&sentinel);
    function->runs[0].mask=mask;
}
static void borrow_sequences(const XrXirCompileContext *context,EffectBodyMaterial *material) {
    EffectBodyRequest request=effect_body_owned_request(context,material);
    for (uint32_t f=0;f<effect_body_count(material);++f) {
        EffectBodyRecordCursor cursor={0};CHECK(effect_body_request_sequence_begin(&request,&cursor)==XR_XIR_OK);
        for (uint32_t i=0;i<material->functions[f].instruction_count;++i) {
            XrXirInstruction strict={0},storage={0};const XrXirInstruction *observed=NULL;const EffectBodyRun *ref=NULL;
            CHECK(effect_body_instruction_get(&request.read,&material->functions[f],i,&strict)==XR_XIR_OK);
            CHECK(effect_body_request_sequence_instruction(&request,f,i,&cursor,&storage,&observed)==XR_XIR_OK);
            CHECK(effect_body_record_ref(&request.read,&material->functions[f],i,&ref)==XR_XIR_OK);
            CHECK(borrow_same(observed,&strict));
            CHECK(observed==(!ref->mask || i==ref->first?&ref->base:&storage));
        }
        CHECK(effect_body_request_sequence_finish(&request,f,&cursor)==XR_XIR_OK);
    }
}
static void borrow_work_prefixes(const XrXirCompileContext *context,EffectBodyMaterial *material) {
    EffectBodyRequest request=effect_body_owned_request(context,material);
    for (uint32_t i=0;i<material->functions[0].instruction_count;++i) {
        BorrowReadBudget budget={UINT64_MAX,0};request.read=(EffectBodyRead){&budget,borrow_consume};
        XrXirInstruction strict={0},storage={0},marker={.op=XR_XIR_CONST_INT,.immediate=71};const XrXirInstruction *out=NULL;
        EffectBodyRead original=effect_body_compile_read(context);
        CHECK(effect_body_instruction_get(&original,&material->functions[0],i,&strict)==XR_XIR_OK);
        CHECK(effect_body_request_instruction(&request,0,i,&storage,&out)==XR_XIR_OK);uint64_t full=budget.used;CHECK(full);
        for (uint64_t limit=0;limit<=full;++limit) {
            budget=(BorrowReadBudget){limit,0};out=&marker;
            XrXirStatus status=effect_body_request_instruction(&request,0,i,&storage,&out);
            CHECK(status==(limit==full?XR_XIR_OK:XR_XIR_BUDGET));
            if (limit<full) CHECK(out==&marker);else CHECK(borrow_same(out,&strict));
            budget=(BorrowReadBudget){UINT64_MAX,0};
            CHECK(effect_body_request_instruction(&request,0,i,&storage,&out)==XR_XIR_OK && borrow_same(out,&strict));
        }
    }
}
static void borrow_read(const XrXirCompileContext *context,EffectBodyMaterial *material) {
    EffectBodyRequest request=effect_body_owned_request(context,material);XrXirInstruction storage={0},strict={0};
    const XrXirInstruction *observed=NULL;uint64_t start=rp_stats(context).work;
    for (uint32_t i=0;i<32;++i) {
        CHECK(effect_body_request_instruction(&request,0,0,&storage,&observed)==XR_XIR_OK);
        CHECK(observed && observed->op==XR_XIR_CONST_INT && observed->type==XR_XIR_I64 && observed->immediate==42);
    }
    uint64_t borrowed=rp_stats(context).work-start;start=rp_stats(context).work;
    for (uint32_t i=0;i<32;++i) CHECK(effect_body_instruction_get(&request.read,&material->functions[0],0,&strict)==XR_XIR_OK);
    uint64_t decoded=rp_stats(context).work-start;
    CHECK(borrow_same(observed,&strict));printf("Owned instruction 32 reads: borrow=%llu decode=%llu\n",(unsigned long long)borrowed,(unsigned long long)decoded);
    CHECK(borrowed<decoded && observed==&material->functions[0].runs[0].base);
    CHECK(borrow_same(&storage,&(XrXirInstruction){0}));
}
static XrXirStatus borrow_operation(const XrXirCompileContext *context) {
    OrdinaryMaterialFixture fixture;initial_ordinary_fixture(&fixture,0);
    XrXirEffects *effects=NULL;EffectOrdinaryContexts *ordinary=NULL;EffectBodyMaterial *material=NULL;
    XrXirStatus status=initial_ordinary_effects(context,&fixture.source,&effects);
    if (status==XR_XIR_OK) status=effect_ordinary_build(context,&fixture.source,NULL,NULL,&ordinary);
    if (status==XR_XIR_OK) {
        material=ordinary->uses.generation;ordinary->uses.generation=NULL;
        XrXirFunction original=ordinary->functions[0];char name[128];XrXirInstruction rows[2];XrXirBlock blocks[1];
        CHECK(original.name_length<=sizeof(name) && original.instruction_count==2 && original.block_count<=1 &&
            !original.parameter_count && !original.operand_count);
        if (original.name_length) memcpy(name,original.name,original.name_length);
        memcpy(rows,original.instructions,sizeof(rows));
        if (original.block_count) memcpy(blocks,original.blocks,sizeof(blocks));
        original.name=original.name_length?name:NULL;original.instructions=rows;original.blocks=original.block_count?blocks:NULL;
        effect_ordinary_free(ordinary);ordinary=NULL;xr_xir_compile_effects_free(effects);effects=NULL;
        borrow_words(context,material,&original);borrow_sequences(context,material);borrow_work_prefixes(context,material);
        memset(&fixture,0xCE,sizeof(fixture));borrow_read(context,material);
        status=ordinary_material_cell_match(context,material,false);
    }
    effect_body_material_free(material);effect_ordinary_free(ordinary);xr_xir_compile_effects_free(effects);return status;
}
static XrCompileResourceStats borrow_run(XrCompileResourceLimits caps,size_t fail,XrXirStatus expected) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(caps);uint64_t baseline=rp_stats(&context).live_bytes;
    rp_attempts=0;rp_injected=false;rp_fail_at=fail;
    CHECK(borrow_operation(&context)==expected);size_t attempts=rp_attempts;rp_fail_at=SIZE_MAX;
    if (fail!=SIZE_MAX) {
        CHECK(rp_injected && attempts==fail+1 && rp_stats(&context).live_bytes==baseline);
        CHECK(borrow_operation(&context)==XR_XIR_OK);
    }
    XrCompileResourceStats observed=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    rp_attempts=attempts;return observed;
}
int main(int argc,char **argv) {
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    XrCompileResourceStats measured=borrow_run(rp_caps(),SIZE_MAX,XR_XIR_OK);size_t sites=rp_attempts;CHECK(sites);
    for (size_t i=0;i<sites;++i) if (resources || !i || i+1==sites) (void)borrow_run(rp_caps(),i,XR_XIR_OUT_OF_MEMORY);
    XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,measured.work};
    (void)borrow_run(exact,SIZE_MAX,XR_XIR_OK);
    for (uint32_t axis=0;axis<3;++axis) {
        XrCompileResourceLimits smaller=exact;
        if (!axis) --smaller.allocated_bytes;else if (axis==1) --smaller.live_bytes;else --smaller.work;
        (void)borrow_run(smaller,SIZE_MAX,XR_XIR_BUDGET);
    }
    printf("Owned rows: ten physical words/independent mismatch rejects, sequential EOF, all read-work prefixes, producer death, same-ledger retry and three axes; sites=%zu injection=%s physical0/0 PASS\n",
        sites,resources?"ALL":"FIRST_LAST_ONLY");return 0;
}
