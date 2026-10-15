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

int main(void) {
    cell_owned_run_literals();
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
    puts("CELL_RANGE_ROW producer dead, fresh words, owner/boundary/budget rejection, unchanged failure output, physical0 PASS");
    return 0;
}
