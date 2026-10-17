/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_snapshot_literal_pair_cases.h - Complete literal pair storage controls
 */
#ifndef XIR_SNAPSHOT_LITERAL_PAIR_CASES_H
#define XIR_SNAPSHOT_LITERAL_PAIR_CASES_H
static void literal_pair_work_bounds(const XrXirCompileContext *context,
    const EffectBodyFunction *body,const XrXirInstruction *expected) {
    XrCompileResourceStats before={0},after={0};XrXirInstruction output={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    EffectBodyRead read=effect_body_compile_read(context);
    CHECK(effect_body_instruction_get(&read,body,1,&output)==XR_XIR_OK && mask_same(&output,expected));
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    uint64_t exact=after.work-before.work+1;
    for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits caps={8388608,4194304,exact-minus};XrXirCompileContext finite={0};
        CHECK(xr_compile_resources_new(&caps,&finite.resources)==XR_COMPILE_RESOURCE_OK);
        finite.limits=xr_xir_compile_default_limits();read=effect_body_compile_read(&finite);
        const XrXirInstruction held={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};output=held;
        CHECK(effect_body_instruction_get(&read,body,1,&output)==(minus?XR_XIR_BUDGET:XR_XIR_OK) &&
            mask_same(&output,minus?&held:expected));
        if(minus)CHECK(effect_body_instruction_get(&read,body,1,&output)==XR_XIR_BUDGET && mask_same(&output,&held));
        xr_compile_resources_release(finite.resources);
    }
}
static void literal_pair_controls(void) {
    const XrCompileResourceLimits caps={8388608,4194304,4000000};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();EffectBodyRead read=effect_body_compile_read(&context);
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    struct LiteralRunExtent {uint32_t first,count,mask;XrXirInstruction base,tail[7];};
    _Static_assert(sizeof(EffectBodyRun)==sizeof(struct LiteralRunExtent),"Literal blocks have an exact bounded inline extent");
    for(uint32_t mask=1;mask<1024;++mask) {
        XrXirInstruction first=mask_expected(0),second=mask_expected(mask),source[2]={first,second};
        XrXirFunction input={.instructions=source,.instruction_count=2};
        EffectBodyFunction body={.instruction_count=2};
        CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_OK && body.run_count==1 &&
            body.runs[0].mask==EFFECT_BODY_LITERAL_ROWS && mask_same(&body.runs[0].base,&first) &&
            mask_same(&body.runs[0].literal_tail[0],&second) && effect_body_records_match(&read,&body,&input)==XR_XIR_OK);
        memset(source,0xce,sizeof(source));EffectBodyRun *pair=&body.runs[0];
        const XrXirInstruction sentinel={.op=XR_XIR_RETURN,.type=XR_XIR_BOOL,.immediate=INT64_MIN};
        XrXirInstruction storage=sentinel;const XrXirInstruction *output=&sentinel;
        CHECK(effect_body_instruction_ref_read(&read,pair,0,&storage,&output)==XR_XIR_OK &&
            output==&pair->base && mask_same(output,&first) && mask_same(&storage,&sentinel));
        CHECK(effect_body_instruction_ref_read(&read,pair,1,&storage,&output)==XR_XIR_OK &&
            output==&pair->literal_tail[0] && mask_same(output,&second) && mask_same(&storage,&sentinel));
        CHECK(effect_body_instruction_get(&read,&body,1,&storage)==XR_XIR_OK && mask_same(&storage,&second));
        XrXirCellProofRun cell={.first=0,.count=2,.operation=&pair->base,.literal_tail=&pair->literal_tail[0]};
        CellProofBuild build={.context=&context,.status=XR_XIR_OK};storage=sentinel;
        CHECK(cell_proof_run_operation(&build,0,&cell,1,&storage)==&pair->literal_tail[0] &&
            mask_same(&pair->literal_tail[0],&second) && mask_same(&storage,&sentinel));
        for(uint32_t w=0;w<10;++w) {
            pair->mask^=UINT32_C(1)<<w;output=&sentinel;storage=sentinel;
            CHECK(effect_body_instruction_ref_read(&read,pair,1,&storage,&output)==XR_XIR_BAD_STRUCTURE &&
                output==&sentinel && mask_same(&storage,&sentinel));pair->mask^=UINT32_C(1)<<w;
            cell.varying^=UINT32_C(1)<<w;build.status=XR_XIR_OK;
            CHECK(!cell_proof_run_operation(&build,0,&cell,1,&storage) && build.status==XR_XIR_BAD_STRUCTURE &&
                mask_same(&storage,&sentinel));cell.varying^=UINT32_C(1)<<w;
        }
        uint32_t saved=pair->mask;pair->mask=mask;output=&sentinel;
        CHECK(effect_body_instruction_ref_read(&read,pair,1,&storage,&output)==XR_XIR_BAD_STRUCTURE && output==&sentinel);
        pair->mask=saved;
        if(mask==1 || mask==513 || mask==1023) {
            mask_prefixes(&context,&body,1,&second);
            mask_view_prefixes(&context,pair,0,&first);mask_view_prefixes(&context,pair,1,&second);
            literal_pair_work_bounds(&context,&body,&second);
        }
        xr_compile_resources_free(body.runs);
    }
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    fprintf(stderr,"Literal pairs1023 exact ten-field cases after producer death, exact bounded inline extent, both real row references, all10230 forged mask bits, old changing two-row encoding refused, every failure prefix and three exact/minusone finite work bounds PASS\n");
}
#endif // XIR_SNAPSHOT_LITERAL_PAIR_CASES_H
