/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_owned_run_cases.h - Independent physical range and Cell replay
 */
#ifndef XIR_CELL_OWNED_RUN_CASES_H
#define XIR_CELL_OWNED_RUN_CASES_H
#include "xir_error_cell_index_fixture.h"

static unsigned cell_owned_run_refusals;
static XrXirStatus cell_owned_run_refuse(const XrXirCompileContext *context,
    const XrXirCellProofBodyRead *read,XrXirCellProofRunCursor *cursor,XrXirCellProofRun *output) {
    (void)context;(void)read;(void)cursor;(void)output;
    ++cell_owned_run_refusals;return XR_XIR_IO;
}
static void cell_owned_run_boundary(const XrXirCompileContext *context,
    const XrXirCellProofBodyRead *read,const XrXirCellProvenance *proof) {
    XrXirCellProofRunCursor held={.function=1},cursor=held;
    const XrXirInstruction held_operation={.op=XR_XIR_RETURN,.type=XR_XIR_F64,.immediate=INT64_MIN};
    XrXirCellProofRun sentinel={.first=123,.count=456,.varying=7,.operation=&held_operation},out=sentinel;
    CHECK(read->range(NULL,read,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&held,sizeof(cursor)) && !memcmp(&out,&sentinel,sizeof(out)));
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats before=rp_stats(&foreign);
    CHECK(read->range(&foreign,read,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&held,sizeof(cursor)) && !memcmp(&out,&sentinel,sizeof(out)));
    XrCompileResourceStats after=rp_stats(&foreign);CHECK(!memcmp(&before,&after,sizeof(before)));
    rp_owner_free(&foreign,before.live_bytes);
    XrXirCellProofBodyRead mixed=*read;mixed.instruction=NULL;
    CHECK(mixed.range(context,&mixed,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&held,sizeof(cursor)) && !memcmp(&out,&sentinel,sizeof(out)));
    mixed=*read;++mixed.domain.function_count;
    CHECK(mixed.range(context,&mixed,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&held,sizeof(cursor)) && !memcmp(&out,&sentinel,sizeof(out)));
    cursor.next=held.next=1;
    CHECK(read->range(context,read,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&held,sizeof(cursor)) && !memcmp(&out,&sentinel,sizeof(out)));
    mixed=*read;mixed.range=cell_owned_run_refuse;cell_owned_run_refusals=0;
    XrXirDiagnostic diagnostic={0};uint64_t live=rp_stats(context).live_bytes;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,&mixed,&diagnostic)==XR_XIR_IO &&
        cell_owned_run_refusals==1 && diagnostic.status==XR_XIR_IO &&
        diagnostic.function==0 && diagnostic.instruction==0 && rp_stats(context).live_bytes==live);
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,proof,read,NULL)==XR_XIR_OK);
}

/* These mutations preserve the physical row count. Actual target and zero
 * operand-table bounds must still reject before any proof can be accepted. */
static void cell_owned_run_semantics(const XrXirCompileContext *context,
    const EffectBodyMaterial *material,const XrXirCellProofBodyRead *read) {
    EffectBodyRead physical=effect_body_compile_read(context);const EffectBodyRun *ref=NULL;
    const EffectBodyFunction *function=&material->functions[1];
    CHECK(effect_body_record_ref(&physical,function,516,&ref)==XR_XIR_OK &&
        ref->first==516 && ref->count==256 && !ref->mask);
    XrXirInstruction *base=(XrXirInstruction *)&ref->base;
    XrXirInstruction saved=*base;
    for (uint32_t k=0;k<3;++k) {
        if (!k) base->immediate=3;
        else base->args[k-1]=1;
        CHECK(xr_xir_compile_cell_provenance_body_matches(context,material->cells,read,NULL)==XR_XIR_BAD_STRUCTURE);
        *base=saved;
        CHECK(xr_xir_compile_cell_provenance_body_matches(context,material->cells,read,NULL)==XR_XIR_OK);
    }
}

/* The independent source oracle reads every unused word and signed bit.
 * Provider range projection is not a replacement for this full matcher. */
/* A raw callback borrows the current actual row only for this call. The
 * owned callback publishes the complete ten-word decode in caller storage. */
static void cell_owned_run_instruction_reference(const XrXirCompileContext *context,
    const EffectBodyMaterial *material,const CellIndexFixture *fixture) {
    EffectCellBodyAdapter raw={0},owned={0};XrXirCellProofBodyRead read={0},sealed={0};
    CHECK(effect_cell_body_raw(context,&fixture->module,&raw,&read)==XR_XIR_OK && !read.range);
    CHECK(effect_cell_body_owned(context,material,&owned,&sealed)==XR_XIR_OK && sealed.range);
    XrXirInstruction storage={.op=XR_XIR_RETURN,.type=XR_XIR_F64,.immediate=INT64_MIN},sentinel=storage;
    const XrXirInstruction *output=NULL;
    CHECK(read.instruction(context,read.owner,1,4,&storage,&output)==XR_XIR_OK &&
        output==&fixture->functions[1].instructions[4] && !memcmp(&storage,&sentinel,sizeof(storage)));
    CHECK(sealed.instruction(context,sealed.owner,1,4,&storage,&output)==XR_XIR_OK && output==&storage &&
        conditional_compact_same(output,&fixture->functions[1].instructions[4]));
    const XrXirInstruction *held=output;size_t attempts=rp_attempts;XrCompileResourceStats before=rp_stats(context);
    CHECK(read.instruction(context,read.owner,1,4,NULL,&output)==XR_XIR_BAD_STRUCTURE && output==held);
    CHECK(sealed.instruction(context,sealed.owner,1,4,NULL,&output)==XR_XIR_BAD_STRUCTURE && output==held &&
        rp_attempts==attempts && rp_stats(context).work==before.work);
}

static void cell_owned_run_source(const XrXirCompileContext *context,
    const EffectBodyMaterial *material,CellIndexFixture *fixture) {
    CHECK(effect_body_source_match(context,material,&fixture->module)==XR_XIR_OK);
    XrXirInstruction old=fixture->subject[4];
    for (uint32_t field=0;field<10;++field) {
        conditional_compact_change(&fixture->subject[4],field);
        CHECK(effect_body_source_match(context,material,&fixture->module)==XR_XIR_BAD_STRUCTURE);
        fixture->subject[4]=old;
        CHECK(effect_body_source_match(context,material,&fixture->module)==XR_XIR_OK);
    }
    cell_owned_run_instruction_reference(context,material,fixture);
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    CHECK(effect_cell_body_owned(context,material,&adapter,&read)==XR_XIR_OK && read.range);
    for (uint32_t f=0;f<fixture->module.function_count;++f) {
        XrXirCellProofRunCursor cursor={.function=f};
        while (cursor.next<fixture->functions[f].instruction_count) {
            XrXirCellProofRun range;
            CHECK(read.range(context,&read,&cursor,&range)==XR_XIR_OK);
            CHECK(conditional_compact_same(range.operation,&fixture->functions[f].instructions[range.first]) &&
                range.count && cursor.next==range.first+range.count);
            CHECK(cursor.run && range.operation==&material->functions[f].runs[cursor.run-1].base &&
                range.operation!=&fixture->functions[f].instructions[range.first]);
        }
        CHECK(cursor.next==fixture->functions[f].instruction_count);
    }
}

static void cell_owned_run_malformed(const XrXirCompileContext *context,
    const EffectBodyMaterial *material,const XrXirCellProofBodyRead *read) {
    EffectBodyFunction *body=(EffectBodyFunction *)&material->functions[1];
    CHECK(body->runs && body->run_count>1 && body->run_capacity==body->run_count);
    EffectBodyRun *first=&body->runs[0],*second=&body->runs[1];
    bool pair=!!(first->mask&EFFECT_BODY_LITERAL_ROWS);
    uint32_t count=body->run_count,column=0;
    while (!pair && column<EFFECT_BODY_WORDS && !(first->mask&(UINT32_C(1)<<column))) ++column;
    CHECK(pair || column<EFFECT_BODY_WORDS);
    uint32_t old_mask=first->mask,old_first=first->first,old_delta=pair?0:first->delta[column],old_neighbor=second->first;
    XrXirCellProofRunCursor held={.function=1};
    const XrXirInstruction held_operation={.op=XR_XIR_RETURN,.immediate=INT64_MIN};
    XrXirCellProofRun sentinel={.first=99,.count=3,.operation=&held_operation};
    for (uint32_t mode=0;mode<5;++mode) {
        if (!mode) first->mask|=UINT32_C(1024);
        if (mode==1) first->first=1;
        if (mode==2) {if(pair)first->mask|=1;else first->delta[column]=0;}
        if (mode==3) second->first=0;
        if (mode==4) body->run_capacity=body->run_count-1;
        XrXirCellProofRunCursor cursor=held;XrXirCellProofRun out=sentinel;
        CHECK(read->range(context,read,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&cursor,&held,sizeof(cursor)) && !memcmp(&out,&sentinel,sizeof(out)));
        first->mask=old_mask;first->first=old_first;if(!pair)first->delta[column]=old_delta;
        second->first=old_neighbor;body->run_capacity=count;
    }
    --body->run_count;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,material->cells,read,NULL)==XR_XIR_BAD_STRUCTURE);
    body->run_count=count;
    EffectBodyFunction *single=(EffectBodyFunction *)&material->functions[0];
    uint32_t mask=single->runs[0].mask;single->runs[0].mask=1;
    XrXirCellProofRunCursor cursor={.function=0};XrXirCellProofRun out=sentinel;
    XrXirCellProofRunCursor cursor_before=cursor;
    CHECK(read->range(context,read,&cursor,&out)==XR_XIR_BAD_STRUCTURE &&
        !memcmp(&cursor,&cursor_before,sizeof(cursor)) && !memcmp(&out,&sentinel,sizeof(out)));
    single->runs[0].mask=mask;
    ++body->run_count;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,material->cells,read,NULL)==XR_XIR_BAD_STRUCTURE);
    body->run_count=count;
    CHECK(xr_xir_compile_cell_provenance_body_matches(context,material->cells,read,NULL)==XR_XIR_OK);
}

/* Raw input, the independently derived proof and the receiving material are
 * distinct. Every owned replay occurs after all producer bytes are poisoned. */
static XrXirStatus cell_owned_run_operation(const XrXirCompileContext *context,uint32_t mode,bool oracle) {
    CHECK(mode<5);
    if (mode==3) return conditional_compact_owned(context,oracle);
    CellIndexFixture fixture;
    CellBoundary boundary=mode==1?CI_PHI:mode==2?CI_INVOKE_UNKNOWN:CI_CALL;
    CHECK(ci_expected(boundary,false)==UINT64_C(12));
    ci_fixture(&fixture,boundary,false,mode==0);
    if (mode==4) {
        fixture.nodes[1].element=XR_XIR_UNIT;
        fixture.subject[0]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)CI_CELL};
        fixture.subject[1]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.args={0}};
        fixture.subject[2]=fixture.init;fixture.blocks[0].count=3;
        fixture.functions[1].parameters=NULL;fixture.functions[1].parameter_count=0;
        fixture.functions[1].instruction_count=3;
    }
    XrXirCellProvenance *independent=NULL;EffectBodyMaterial *material=NULL;
    XrXirStatus status=xir_fixture_verify(context,&fixture.module,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_verified(context,&fixture.module,&independent,NULL);
    if (status==XR_XIR_OK) status=effect_body_material_create(context,&fixture.module,&material);
    if (status==XR_XIR_OK && oracle && !mode) cell_owned_run_source(context,material,&fixture);
    memset(&fixture,0xCE,sizeof(fixture));
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    if (status==XR_XIR_OK) status=effect_cell_body_owned(context,material,&adapter,&read);
    if (status==XR_XIR_OK) status=xr_xir_compile_cell_provenance_body_matches(context,independent,&read,NULL);
    if (status==XR_XIR_OK && oracle && !mode) {
        cell_owned_run_boundary(context,&read,material->cells);
        cell_owned_run_semantics(context,material,&read);cell_owned_run_malformed(context,material,&read);
    }
    if (status==XR_XIR_OK) {
        XrXirCellOriginView view={0};uint32_t value=mode==4?0:mode==1?3:5;
        status=xr_xir_compile_cell_origin_view(context,material->cells,1,value,&view);
        if (status==XR_XIR_OK) {
            CHECK(xr_xir_cell_provenance_origin(material->cells,1,value)==XR_XIR_CELL_ORIGIN_OWNED &&
                !view.intrinsic_mask);
            if (mode==4) CHECK(!view.dependencies && !view.word_count && !view.parameter_count);
            else CHECK(view.word_count==1 && view.dependencies && !view.dependencies[0] &&
                view.parameter_count==1 && view.parameters[0]==0);
        }
    }
    xr_xir_compile_cell_provenance_free(independent);effect_body_material_free(material);return status;
}

static void cell_owned_run_literals(void) {
    for (uint32_t mode=0;mode<5;++mode) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        context.limits.instructions=CI_MAX_OPS;uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(cell_owned_run_operation(&context,mode,true)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

/* This independent lane owns the complete five-operation census. Every
 * fault retries on the same finite ledger after all operation storage dies. */
static void cell_owned_run_resources(void) {
    for (uint32_t mode=0;mode<5;++mode) {
        size_t sites=0;XrCompileResourceStats census={0},entry={0};
        for (size_t pass=0;pass<=sites;++pass) {
            RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_injected=false;
            XrXirCompileContext context=rp_owner(rp_caps());context.limits.instructions=CI_MAX_OPS;
            entry=rp_stats(&context);RootParameterMark retained=rp_mark();size_t first=rp_attempts;
            rp_fail_at=pass?first+pass-1:SIZE_MAX;
            XrXirStatus status=cell_owned_run_operation(&context,mode,false);
            if (!pass) {
                CHECK(status==XR_XIR_OK);sites=rp_attempts-first;census=rp_stats(&context);CHECK(sites);
                printf("CELL_OWNED_RUN_CENSUS mode%u sites%zu work%llu allocated%llu peak%llu\n",mode,sites,
                    (unsigned long long)(census.work-entry.work),
                    (unsigned long long)(census.allocated_bytes-entry.allocated_bytes),
                    (unsigned long long)census.peak_bytes);
            } else {
                CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts-first==pass);
                CHECK(rp_stats(&context).live_bytes==entry.live_bytes);rp_balanced(retained);
                XrCompileResourceStats consumed=rp_stats(&context);
                uint64_t work=census.work-entry.work,bytes=census.allocated_bytes-entry.allocated_bytes;
                bool fits=work<=context.resources->limits.work-consumed.work &&
                    bytes<=context.resources->limits.allocated_bytes-consumed.allocated_bytes;
                rp_fail_at=SIZE_MAX;size_t retry=rp_attempts;
                status=cell_owned_run_operation(&context,mode,false);
                CHECK(status==(fits?XR_XIR_OK:XR_XIR_BUDGET));
                CHECK(rp_stats(&context).live_bytes==entry.live_bytes);rp_balanced(retained);
                if (fits) CHECK(rp_stats(&context).work-consumed.work==work &&
                    rp_stats(&context).allocated_bytes-consumed.allocated_bytes==bytes &&
                    rp_stats(&context).allocation_count-consumed.allocation_count==census.allocation_count-entry.allocation_count &&
                    rp_attempts-retry==sites);
            }
            rp_fail_at=SIZE_MAX;rp_owner_free(&context,entry.live_bytes);rp_balanced(physical);
        }
        uint64_t measured[3]={census.allocated_bytes,census.peak_bytes,census.work};
        for (uint32_t axis=0;axis<3;++axis) for (uint32_t minus=0;minus<2;++minus) {
            XrCompileResourceLimits limits=rp_caps();uint64_t limit=measured[axis]-minus;
            if (axis==0) limits.allocated_bytes=limit;
            else if (axis==1) limits.live_bytes=limit;
            else limits.work=limit;
            RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(limits);
            context.limits.instructions=CI_MAX_OPS;uint64_t baseline=rp_stats(&context).live_bytes;
            CHECK(cell_owned_run_operation(&context,mode,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
            rp_owner_free(&context,baseline);rp_balanced(physical);
        }
    }
}
#endif // XIR_CELL_OWNED_RUN_CASES_H
