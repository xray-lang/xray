/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_effect_record_sharing.c - Independent encoding lifetime and refusal
 */
#define main record_original_checks
#include "../../xir/test_xir_root_conditional_helpers.c"
#undef main

typedef struct RecordSink { uint32_t rows;bool refuse; } RecordSink;
static XrXirStatus record_row(void *owner,uint32_t instruction,const XrXirInstruction *op) {
    RecordSink *sink=owner;
    CHECK(instruction==sink->rows && instruction<2);
    CHECK(instruction ? op->op==XR_XIR_RETURN :
        op->op==XR_XIR_CONST_INT && op->type==XR_XIR_I64 && op->immediate==42);
    ++sink->rows;return sink->refuse?XR_XIR_BUDGET:XR_XIR_OK;
}
static XrXirStatus record_span(void *owner,uint32_t first,uint32_t count) {
    (void)owner;(void)first;(void)count;
    CHECK(false);return XR_XIR_BAD_STRUCTURE;
}
static void record_change_word(XrXirInstruction *op,uint32_t word) {
    switch (word) {
    case 0:op->op=XR_XIR_CONST_FLOAT;break;
    case 1:op->type=XR_XIR_F64;break;
    case 2:op->args[0]=19;break;case 3:op->args[1]=23;break;
    case 4:op->targets[0]=29;break;case 5:op->targets[1]=31;break;
    case 6:op->immediate=43;break;
    case 7:op->immediate=INT64_C(0x10000002a);break;
    case 8:op->type_arguments[0]=37;break;case 9:op->type_arguments[1]=41;break;
    default:CHECK(false);break;
    }
}

/* A real complete checker owns the prior. The receiving generation builds
 * its own types, metadata and proofs before the storage-only experiment. */
static XrXirStatus record_operation(const XrXirCompileContext *context,int change,bool refuse) {
    OrdinaryMaterialFixture fixture;initial_ordinary_fixture(&fixture,0);
    XrXirEffects *effects=NULL;EffectOrdinaryContexts *ordinary=NULL;EffectBodyMaterial *material=NULL;
    XrXirStatus status=initial_ordinary_effects(context,&fixture.source,&effects);
    if (status==XR_XIR_OK) status=effect_ordinary_build(context,&fixture.source,NULL,NULL,&ordinary);
    if (status==XR_XIR_OK) {
        material=ordinary->uses.generation;ordinary->uses.generation=NULL;
        EffectBodyMaterial *parent=effects->invocations->body;
        uint32_t references=parent->references;
        CHECK(material->cells!=parent->cells && material->physical!=parent->physical && !material->backbone);
        EffectBodyFunction *target=&material->functions[0];
        effect_body_runs_release(target->runs);target->runs=NULL;target->run_count=target->run_capacity=0;
        XrXirInstruction rows[2];memcpy(rows,ordinary->functions[0].instructions,sizeof(rows));
        XrXirFunction current=ordinary->functions[0];current.instructions=rows;
        if (change>=0) record_change_word(&rows[0],(uint32_t)change);
        EffectBodyOrdinaryInput input={.contexts=ordinary,.source=&fixture.source,.record_source=parent};
        RecordSink observation={0,refuse};EffectBodyWriteSink sink={&observation,record_row,record_span};
        EffectBodyRecordShareRequest request={0,&current,&sink};bool shared=false;
        status=effect_body_records_share(context,&input,material,parent,&request,&shared);
        CHECK(parent->references==references);
        if (status==XR_XIR_OK && change>=0) {
            CHECK(!shared && !target->runs && !observation.rows);
        } else if (status==XR_XIR_OK) {
            CHECK(shared && observation.rows==2 && target->borrowed_runs && target->runs==parent->functions[0].runs);
            CHECK(effect_body_runs_reserve(context,target)==XR_XIR_BAD_STRUCTURE);
        } else if (refuse) CHECK(status==XR_XIR_BUDGET && !shared && observation.rows==1);
        xr_xir_compile_effects_free(effects);effects=NULL;
        effect_ordinary_free(ordinary);ordinary=NULL;
        memset(&fixture,0xCE,sizeof(fixture));memset(rows,0xCE,sizeof(rows));
        if (status==XR_XIR_OK && change<0) {
            EffectBodyRequest read=effect_body_owned_request(context,material);
            XrXirInstruction decoded={0};const XrXirInstruction *op=NULL;
            status=effect_body_request_instruction(&read,0,0,&decoded,&op);
            if (status==XR_XIR_OK) CHECK(op->op==XR_XIR_CONST_INT && op->immediate==42);
            if (status==XR_XIR_OK) status=ordinary_material_cell_match(context,material,false);
        }
    }
    effect_body_material_free(material);effect_ordinary_free(ordinary);xr_xir_compile_effects_free(effects);
    return status;
}

static XrCompileResourceStats record_run(XrCompileResourceLimits caps,int change,bool refuse,
    size_t fail,XrXirStatus expected) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(caps);
    uint64_t baseline=rp_stats(&context).live_bytes;
    rp_attempts=0;rp_injected=false;rp_fail_at=fail;
    CHECK(record_operation(&context,change,refuse)==expected);
    if (fail!=SIZE_MAX) CHECK(rp_injected && rp_attempts==fail+1);
    rp_fail_at=SIZE_MAX;
    XrCompileResourceStats observed=rp_stats(&context);
    rp_owner_free(&context,baseline);rp_balanced(physical);return observed;
}

int main(int argc,char **argv) {
    bool full=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || full);
    initial_ordinary_controls();ordinary_material_controls();
    XrCompileResourceStats measured=record_run(rp_caps(),-1,false,SIZE_MAX,XR_XIR_OK);
    size_t points=rp_attempts;CHECK(points);
    printf("Exact physical record sharing: %zu allocation positions\n",points);
    for (int word=0;word<10;++word) (void)record_run(rp_caps(),word,false,SIZE_MAX,XR_XIR_OK);
    (void)record_run(rp_caps(),-1,true,SIZE_MAX,XR_XIR_BUDGET);
    for (size_t point=0;point<points;++point) {
        if (!full && point && point+1!=points) continue;
        (void)record_run(rp_caps(),-1,false,point,XR_XIR_OUT_OF_MEMORY);
    }
    XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,measured.work};
    (void)record_run(exact,-1,false,SIZE_MAX,XR_XIR_OK);
    for (unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits smaller=exact;
        if (!axis) --smaller.allocated_bytes;else if (axis==1) --smaller.live_bytes;else --smaller.work;
        (void)record_run(smaller,-1,false,SIZE_MAX,XR_XIR_BUDGET);
    }
    CHECK(!rp_live && !rp_live_bytes);return 0;
}
