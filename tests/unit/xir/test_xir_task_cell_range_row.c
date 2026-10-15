/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_cell_range_row.c - Fresh complete rows from owned Cell ranges
 */
#define main range_row_retained_original_main
#include "test_xir_root_conditional_helpers.c"
#undef main
#include "xir_cell_edge_provider_cases.h"
#include "xir_cell_edge_original_cases.h"
#include "xir_cell_edge_growth_cases.h"

static void range_row_rejections(const XrXirCompileContext *context,
    const XrXirCellProofBodyRead *read,const XrXirCellProofRunCursor *cursor,uint32_t first) {
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_F64,.immediate=INT64_MIN},out;
    for (unsigned damage=0;damage<7;++damage) {
        XrXirCellProofRunCursor held=*cursor;XrXirCellProofBodyRead mixed=*read;
        uint32_t start=first,index=first+1;
        if (damage==0) held.run=0;
        else if (damage==1) --held.next;
        else if (damage==2) held.function=read->domain.function_count;
        else if (damage==3) ++start;
        else if (damage==4) index=held.next;
        else if (damage==5) ++mixed.domain.function_count;
        else mixed.instruction=NULL;
        out=sentinel;
        CHECK(read->range_row(context,&mixed,&held,start,index,&out)==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&out,&sentinel,sizeof(out)));
    }
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats before=rp_stats(&foreign);
    out=sentinel;
    CHECK(read->range_row(&foreign,read,cursor,first,first+1,&out)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=rp_stats(&foreign);
    CHECK(!memcmp(&before,&after,sizeof(before)) && !memcmp(&out,&sentinel,sizeof(out)));
    rp_owner_free(&foreign,before.live_bytes);
}

static void range_row_varying_types(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    context.limits.instructions=CI_MAX_OPS;uint64_t baseline=rp_stats(&context).live_bytes;
    CellIndexFixture fixture;ci_fixture(&fixture,CI_CALL,false,true);
    fixture.subject[4]=(XrXirInstruction){.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1};
    fixture.subject[5]=(XrXirInstruction){.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64};
    XrXirCellProvenance *independent=NULL;EffectBodyMaterial *material=NULL;
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_cell_provenance_verified(&context,&fixture.module,&independent,NULL)==XR_XIR_OK);
    CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    memset(&fixture,0xCE,sizeof(fixture));
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    CHECK(effect_cell_body_owned(&context,material,&adapter,&read)==XR_XIR_OK);
    XrXirCellProofRunCursor cursor={.function=1};bool varying=false;
    while (cursor.next<material->functions[1].instruction_count) {
        XrXirCellProofRun run={0};CHECK(read.range(&context,&read,&cursor,&run)==XR_XIR_OK);
        varying|=run.count>1 && !!(run.varying&UINT32_C(2));
    }
    CHECK(varying && xr_xir_compile_cell_provenance_body_matches(&context,independent,&read,NULL)==XR_XIR_OK);
    xr_xir_compile_cell_provenance_free(independent);effect_body_material_free(material);
    rp_owner_free(&context,baseline);rp_balanced(physical);
}

static void shape_reject(const XrXirCompileContext *context,const XrXirCellProofBodyRead *read,
    const XrXirCellProofRunCursor *cursor,uint32_t first,uint32_t i,XrXirStatus status) {
    XrXirCellProofInstructionShape held={XR_XIR_RETURN,XR_XIR_F64},output=held;
    CHECK(read->range_shape(context,read,cursor,first,i,&output)==status);
    CHECK(!memcmp(&held,&output,sizeof(held)));
}
static void shape_owned(uint32_t runs,uint32_t less) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    XrXirInstruction operations[9]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL,.immediate=1},
        {.op=XR_XIR_CONST_BOOL,.type=XR_XIR_BOOL},
        {.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64},
        {.op=XR_XIR_CONST_FLOAT,.type=XR_XIR_F64,.immediate=1},
        {.op=XR_XIR_CONST_RUNE,.type=XR_XIR_RUNE,.immediate=97},
        {.op=XR_XIR_CONST_RUNE,.type=XR_XIR_RUNE,.immediate=98},
        {.op=XR_XIR_RETURN}};
    uint32_t count=2*runs-1;operations[count-1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture.functions[3].instructions=operations;
    fixture.functions[3].instruction_count=fixture.init_block.count=count;
    XrXirInstruction expected[9];memcpy(expected,operations,sizeof(expected));
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));memset(operations,0xCE,sizeof(operations));
    EffectBodyMaterial *body=effects->invocations->body;EffectBodyFunction *head=&body->functions[3];
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    CHECK(effect_cell_body_owned(&context,body,&adapter,&read)==XR_XIR_OK);
    CHECK(head->run_count==runs && head->instruction_count==count);
    XrXirCellProofRunCursor cursor={.function=3},first_cursor={0};
    while(cursor.next<count) {
        XrXirCellProofRun run={0};CHECK(read.range(&context,&read,&cursor,&run)==XR_XIR_OK);
        if (!run.first) {first_cursor=cursor;}
        for(uint32_t i=run.first;i-run.first<run.count;++i) {
            XrXirCellProofInstructionShape shape={0};XrXirInstruction row={0};
            CHECK(read.range_shape(&context,&read,&cursor,run.first,i,&shape)==XR_XIR_OK);
            CHECK(read.range_row(&context,&read,&cursor,run.first,i,&row)==XR_XIR_OK);
            CHECK(shape.op==expected[i].op && shape.type==expected[i].type &&
                shape.op==row.op && shape.type==row.type);
        }
    }
    for(uint32_t damage=0;damage<9;++damage) {
        XrXirCellProofRunCursor bad=first_cursor;XrXirCellProofBodyRead mixed=read;
        uint32_t start=0,index=0;
        if(damage==0) bad.run=0;
        else if(damage==1) bad.next=0;
        else if(damage==2) bad.function=read.domain.function_count;
        else if(damage==3) start=1;
        else if(damage==4) index=bad.next;
        else if(damage==5) ++mixed.domain.function_count;
        else if(damage==6) mixed.instruction=NULL;
        else if(damage==7) mixed.range_row=NULL;
        else mixed.range=NULL;
        shape_reject(&context,&mixed,&bad,start,index,XR_XIR_BAD_STRUCTURE);
    }
    XrXirCellProofBodyRead mixed=read;mixed.range_shape=NULL;
    CHECK(xr_xir_compile_cell_provenance_body_matches(&context,body->cells,&mixed,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirInstruction full_held={.op=XR_XIR_RETURN},full_out=full_held;
    CHECK(read.range_row(&context,&mixed,&first_cursor,0,0,&full_out)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&full_out,&full_held,sizeof(full_out)));
    EffectCellBodyAdapter fake_adapter=adapter;fake_adapter.request.instruction=NULL;mixed=read;mixed.owner=&fake_adapter;
    shape_reject(&context,&mixed,&first_cursor,0,0,XR_XIR_BAD_STRUCTURE);
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats foreign_before=rp_stats(&foreign);
    shape_reject(&foreign,&read,&first_cursor,0,0,XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats foreign_after=rp_stats(&foreign);
    CHECK(!memcmp(&foreign_before,&foreign_after,sizeof(foreign_before)));rp_owner_free(&foreign,foreign_before.live_bytes);
    EffectBodyRun saved=head->runs[0];
    if(runs>1) {
        for(uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
            head->runs[0].mask|=UINT32_C(1)<<w;head->runs[0].delta[w]=0;
            shape_reject(&context,&read,&first_cursor,0,1,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
        }
        /* The first row remains valid while the next opcode wraps to INVALID. */
        head->runs[0].base.op=(XrXirOp)(XR_XIR_INVALID+1);head->runs[0].mask|=1;head->runs[0].delta[0]=UINT32_MAX;
        shape_reject(&context,&read,&first_cursor,0,1,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
        /* Both requested columns must be current, with unsigned codec arithmetic. */
        head->runs[0].mask|=3;head->runs[0].delta[0]=1;head->runs[0].delta[1]=UINT32_MAX;
        head->runs[0].base.op=XR_XIR_CONST_INT;head->runs[0].base.type=XR_XIR_I64;
        XrXirCellProofInstructionShape actual={0};
        CHECK(read.range_shape(&context,&read,&first_cursor,0,1,&actual)==XR_XIR_OK);
        CHECK(actual.op==XR_XIR_CONST_INT+1 && actual.type==(uint32_t)XR_XIR_I64-1);head->runs[0]=saved;
    }
    head->runs[0].count=0;shape_reject(&context,&read,&first_cursor,0,0,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
    head->runs[0].first=1;shape_reject(&context,&read,&first_cursor,0,0,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
    head->runs[0].mask|=1024;shape_reject(&context,&read,&first_cursor,0,0,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
    head->runs[0].base.op=XR_XIR_OP_COUNT;shape_reject(&context,&read,&first_cursor,0,0,XR_XIR_BAD_STRUCTURE);head->runs[0]=saved;
    ++head->instruction_count;shape_reject(&context,&read,&cursor,count-1,count-1,XR_XIR_BAD_STRUCTURE);--head->instruction_count;
    XrCompileResourceStats before=rp_stats(&context);XrXirCellProofInstructionShape output={0};
    CHECK(read.range_shape(&context,&read,&first_cursor,0,0,&output)==XR_XIR_OK);
    XrCompileResourceStats after=rp_stats(&context);uint64_t required=after.work-before.work;
    CHECK(required && after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes && after.peak_bytes==before.peak_bytes);
    uint64_t remaining=required-less,limit=rp_caps().work;
    CHECK(after.work+remaining<limit && xr_compile_resources_work(context.resources,limit-after.work-remaining)==XR_COMPILE_RESOURCE_OK);
    if(less) shape_reject(&context,&read,&first_cursor,0,0,XR_XIR_BUDGET);
    else CHECK(read.range_shape(&context,&read,&first_cursor,0,0,&output)==XR_XIR_OK);
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);
}
int main(void) {
    cell_owned_run_literals();
    range_row_varying_types();
    for (uint32_t runs=1;runs<=5;++runs)
        for (uint32_t less=0;less<2;++less) shape_owned(runs,less);
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    fixture.init[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=1};
    fixture.init[1]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=2};
    fixture.init[2]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture.init_block.count=fixture.functions[3].instruction_count=3;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_check(&context,&fixture.module,&checked,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_effects_analyze(checked,&effects)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    EffectBodyMaterial *body=effects->invocations->body;
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    CHECK(effect_cell_body_owned(&context,body,&adapter,&read)==XR_XIR_OK);
    XrXirCellProofRunCursor cursor={.function=3};XrXirCellProofRun run={0};
    CHECK(read.range(&context,&read,&cursor,&run)==XR_XIR_OK && run.first==0 && run.count==2);
    XrXirInstruction row={0};
    XrCompileResourceStats before_row=rp_stats(&context);
    CHECK(read.range_row(&context,&read,&cursor,run.first,1,&row)==XR_XIR_OK);
    XrCompileResourceStats after_row=rp_stats(&context);
    CHECK(after_row.work>before_row.work && after_row.allocated_bytes==before_row.allocated_bytes &&
        after_row.live_bytes==before_row.live_bytes && after_row.peak_bytes==before_row.peak_bytes);
    CHECK(row.op==XR_XIR_CONST_INT && row.type==XR_XIR_I64 && row.immediate==2);
    range_row_rejections(&context,&read,&cursor,run.first);

    EffectBodyRun *actual=&body->functions[3].runs[0];XrXirInstruction saved=actual->base;
    actual->base.args[0]=UINT32_MAX;actual->base.args[1]=17;
    actual->base.targets[0]=31;actual->base.targets[1]=UINT32_MAX;
    actual->base.type_arguments[0]=41;actual->base.type_arguments[1]=UINT32_MAX;
    actual->base.immediate=INT64_MIN+51;
    CHECK(read.range_row(&context,&read,&cursor,run.first,1,&row)==XR_XIR_OK);
    CHECK(row.args[0]==UINT32_MAX && row.args[1]==17 && row.targets[0]==31 && row.targets[1]==UINT32_MAX &&
        row.type_arguments[0]==41 && row.type_arguments[1]==UINT32_MAX && row.immediate==INT64_MIN+52);
    CHECK(run.operation.immediate==1);actual->base=saved;
    XrXirInstruction sentinel={.op=XR_XIR_RETURN,.immediate=-7};
    for (unsigned damage=0;damage<3;++damage) {
        uint32_t count=actual->count,mask=actual->mask;
        if (damage==0) ++actual->count;
        else if (damage==1) actual->mask|=UINT32_C(1)<<10;
        else actual->base.op=XR_XIR_INVALID;
        row=sentinel;
        CHECK(read.range_row(&context,&read,&cursor,run.first,1,&row)==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&row,&sentinel,sizeof(row)));
        actual->count=count;actual->mask=mask;actual->base=saved;
    }
    CHECK(read.range_row(&context,&read,&cursor,run.first,1,&row)==XR_XIR_OK && row.immediate==2);
    XrXirCellProofBodyRead mixed=read;mixed.range_row=NULL;
    CHECK(xr_xir_compile_cell_provenance_body_matches(&context,body->cells,&mixed,NULL)==XR_XIR_BAD_STRUCTURE);
    mixed=read;mixed.range=NULL;
    CHECK(xr_xir_compile_cell_provenance_body_matches(&context,body->cells,&mixed,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirCellProofRunCursor tail_cursor=cursor;XrXirCellProofRun tail={0};
    CHECK(read.range(&context,&read,&tail_cursor,&tail)==XR_XIR_OK && tail.first==2 && tail.count==1);
    CHECK(read.range_row(&context,&read,&tail_cursor,tail.first,2,&row)==XR_XIR_OK && row.op==XR_XIR_RETURN);
    ++body->functions[3].instruction_count;row=sentinel;
    CHECK(read.range_row(&context,&read,&tail_cursor,tail.first,2,&row)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&row,&sentinel,sizeof(row)));--body->functions[3].instruction_count;
    XrCompileResourceStats stats=rp_stats(&context);uint64_t limit=rp_caps().work;
    CHECK(stats.work<limit && xr_compile_resources_work(context.resources,limit-stats.work-1)==XR_COMPILE_RESOURCE_OK);
    row=sentinel;
    CHECK(read.range_row(&context,&read,&cursor,run.first,1,&row)==XR_XIR_BUDGET);
    CHECK(!memcmp(&row,&sentinel,sizeof(row)));
    xr_xir_compile_effects_free(effects);rp_owner_free(&context,baseline);rp_balanced(physical);
    CHECK(!rp_live && !rp_live_bytes && !rp_allocation_observer);
    cell_edge_cases();
    puts("CELL_RANGE_ROW_AND_SHAPE producer dead, current words, all10 deltas, paired owner/domain/range, EOF, exact/minus1, unchanged failure output, physical0 PASS");
    return 0;
}
