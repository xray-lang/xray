/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_type_projection_cases.h - Independent type fields for a fresh Cell replay
 */
#ifndef XIR_CELL_TYPE_PROJECTION_CASES_H
#define XIR_CELL_TYPE_PROJECTION_CASES_H
static unsigned cell_projection_delegated_reads;
static XrXirStatus cell_projection_delegated(const EffectBodyRequest *request,
    uint32_t f,uint32_t value,XrXirType *output) {
    (void)request;(void)f;(void)value;(void)output;
    ++cell_projection_delegated_reads;return XR_XIR_BAD_VALUE;
}

static void cell_type_projection_literals(void) {
    RootParameterMark initial=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    context.limits.instructions=CI_MAX_OPS;uint64_t baseline=rp_stats(&context).live_bytes;
    CellIndexFixture fixture;ci_fixture(&fixture,CI_CALL,false,true);
    CHECK(xir_fixture_verify(&context,&fixture.module,NULL)==XR_XIR_OK);
    EffectBodyMaterial *material=NULL;
    CHECK(effect_body_material_create(&context,&fixture.module,&material)==XR_XIR_OK);
    size_t values=0;
    for(uint32_t f=0;f<fixture.module.function_count;++f)
        values+=(size_t)fixture.module.functions[f].parameter_count+fixture.module.functions[f].instruction_count;
    XrXirStatus allocated=XR_XIR_OK;
    XrXirType *expected=xir_compile_alloc(&context,values*sizeof(*expected),&allocated);
    CHECK(allocated==XR_XIR_OK && expected);size_t at=0;
    for(uint32_t f=0;f<fixture.module.function_count;++f){const XrXirFunction *fn=&fixture.module.functions[f];
        for(uint32_t v=0;v<(uint64_t)fn->parameter_count+fn->instruction_count;++v)
            expected[at++]=xr_xir_operand_type(fn,v);
    }
    CHECK(at==values);memset(&fixture,0xce,sizeof(fixture));
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    CHECK(effect_cell_body_owned(&context,material,&adapter,&read)==XR_XIR_OK);
    RootParameterMark held=rp_mark();uint64_t held_bytes=rp_stats(&context).live_bytes;
    rp_fail_at=rp_attempts;rp_injected=false;
    CHECK(effect_cell_body_project(&adapter)==XR_XIR_OUT_OF_MEMORY && rp_injected &&
        !adapter.projection_body && !adapter.projection_offsets && !adapter.projection_types &&
        !adapter.projection_values && adapter.request.owner==material);
    rp_fail_at=SIZE_MAX;rp_balanced(held);CHECK(rp_stats(&context).live_bytes==held_bytes);
    CHECK(effect_cell_body_project(&adapter)==XR_XIR_OK && adapter.projection_body==material);
    uint64_t *occupied=adapter.projection_offsets;XrCompileResourceStats before=rp_stats(&context);
    CHECK(effect_cell_body_project(&adapter)==XR_XIR_BAD_STRUCTURE && adapter.projection_offsets==occupied &&
        rp_stats(&context).work==before.work && rp_stats(&context).allocation_count==before.allocation_count);
    CHECK(effect_cell_body_owned(&context,material,&adapter,&read)==XR_XIR_BAD_STRUCTURE &&
        adapter.projection_offsets==occupied && rp_stats(&context).work==before.work);
    at=0;
    for(uint32_t f=0;f<effect_body_count(material);++f){const EffectBodyFunction *fn=&material->functions[f];
        for(uint32_t v=0;v<(uint64_t)fn->parameter_count+fn->instruction_count;++v){XrXirType type=XR_XIR_U8;
            CHECK(at<values && read.type(&context,read.owner,f,v,&type)==XR_XIR_OK && type==expected[at++]);}
    }
    CHECK(at==values && xr_xir_compile_cell_provenance_body_matches(&context,material->cells,&read,NULL)==XR_XIR_OK);
    XrXirType sentinel=XR_XIR_U8;adapter.request.type=cell_projection_delegated;cell_projection_delegated_reads=0;
    CHECK(read.type(&context,read.owner,0,0,&sentinel)==XR_XIR_BAD_VALUE && sentinel==XR_XIR_U8 &&
        cell_projection_delegated_reads==1);adapter.request.type=effect_body_owned_type;
    uint64_t first=adapter.projection_offsets[0];adapter.projection_offsets[0]=adapter.projection_values+1;
    CHECK(read.type(&context,read.owner,0,0,&sentinel)==XR_XIR_BAD_STRUCTURE && sentinel==XR_XIR_U8);
    adapter.projection_offsets[0]=first;effect_cell_body_projection_drop(&adapter);
    CHECK(read.type(&context,read.owner,0,0,&sentinel)==XR_XIR_BAD_STRUCTURE && sentinel==XR_XIR_U8);
    CHECK(effect_cell_body_owned(&context,material,&adapter,&read)==XR_XIR_OK &&
        effect_cell_body_project(&adapter)==XR_XIR_OK);
    CHECK(xr_xir_compile_cell_provenance_body_matches(&context,material->cells,&read,NULL)==XR_XIR_OK);
    effect_cell_body_projection_drop(&adapter);xr_compile_resources_free(expected);effect_body_material_free(material);
    rp_owner_free(&context,baseline);rp_balanced(initial);
}
#endif // XIR_CELL_TYPE_PROJECTION_CASES_H
