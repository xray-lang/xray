/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_class_view_data_cases.h - Complete physical result field controls
 *
 * KEY CONCEPT:
 *   Literal field combinations exercise storage, never proof admission.
 *   Existing real Class and Cell fixtures retain their semantic checks.
 */
#ifndef XIR_CELL_CLASS_VIEW_DATA_CASES_H
#define XIR_CELL_CLASS_VIEW_DATA_CASES_H
static XrXirCellClassOriginView class_view_literal(uint32_t fields) {
    static const uint64_t cells[]={1,2},classes[]={2,4};
    static const uint32_t parameters[]={0,2};
    static const XrXirCellClassRoot roots[]={{1,0},{3,2}};
    return (XrXirCellClassOriginView){
        .cells={.intrinsic_mask=fields&1?3:0,.dependencies=fields&2?cells:NULL,
            .word_count=fields&4?2:0,.parameters=fields&8?parameters:NULL,.parameter_count=fields&16?2:0},
        .residual_intrinsic_mask=fields&32?3:0,.dependencies=fields&64?classes:NULL,
        .word_count=fields&128?2:0,.roots=fields&256?roots:NULL,.root_count=fields&512?2:0};
}
static bool class_view_same(const XrXirCellClassOriginView *a,const XrXirCellClassOriginView *b) {
    return a->cells.intrinsic_mask==b->cells.intrinsic_mask && a->cells.dependencies==b->cells.dependencies &&
        a->cells.word_count==b->cells.word_count && a->cells.parameters==b->cells.parameters &&
        a->cells.parameter_count==b->cells.parameter_count && a->residual_intrinsic_mask==b->residual_intrinsic_mask &&
        a->dependencies==b->dependencies && a->word_count==b->word_count &&
        a->roots==b->roots && a->root_count==b->root_count;
}
static void class_view_work_bounds(const XrXirCompileContext *context,
    const XrXirCellClassOriginView *source) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirCellClassOriginView output={0};
    CHECK(cell_class_view_publish(context,source,&output)==XR_XIR_OK && class_view_same(&output,source));
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    uint64_t exact=after.work-before.work+1;
    for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits caps={8388608,4194304,exact-minus};XrXirCompileContext finite={0};
        CHECK(xr_compile_resources_new(&caps,&finite.resources)==XR_COMPILE_RESOURCE_OK);
        finite.limits=xr_xir_compile_default_limits();XrCompileResources *identity=finite.resources;
        output=(XrXirCellClassOriginView){0};XrXirCellClassOriginView empty={0};
        CHECK(cell_class_view_publish(&finite,source,&output)==(minus?XR_XIR_BUDGET:XR_XIR_OK) &&
            class_view_same(&output,minus?&empty:source) && finite.resources==identity);
        if(minus) {
            CHECK(cell_class_view_publish(&finite,source,&output)==XR_XIR_BUDGET &&
                class_view_same(&output,&empty) && finite.resources==identity);
        }
        xr_compile_resources_release(finite.resources);
    }
}
static void class_view_data_controls(const XrXirCompileContext *context) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t fields=0;fields<1024;++fields) {
        XrXirCellClassOriginView source=class_view_literal(fields),held=source,output={0};
        CHECK(cell_class_view_publish(context,&source,&output)==XR_XIR_OK &&
            class_view_same(&output,&held) && class_view_same(&source,&held));
        if(!fields || fields==1 || fields==2 || fields==256 || fields==1023)
            class_view_work_bounds(context,&source);
    }
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK &&
        before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
    fprintf(stderr,"Class view data1024 field combinations: independent full-field oracle, source unchanged, no allocation, five exact/minusone work cases, atomic empty output and same finite-ledger repeated refusal PASS\n");
}
#endif // XIR_CELL_CLASS_VIEW_DATA_CASES_H
