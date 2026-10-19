/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_record_source.inc.c - Complete physical data lifetime input
 */
static XrXirStatus effect_body_material_retain(const XrXirCompileContext *work,EffectBodyMaterial *body);

/* The result owns no fact about the new semantic generation. All records,
 * opcodes and physical words are compared afresh; only an exact immutable
 * array can retain its existing releasing owner instead of another copy. */
static XrXirStatus effect_body_records_source(const XrXirCompileContext *work,
    const XrXirModule *source,EffectBodyMaterial *candidate,EffectBodyMaterial **output) {
    if (!xir_compile_context_valid(work) || !source || !output || *output ||
        (source->function_count && !source->functions)) return XR_XIR_BAD_STRUCTURE;
    if (!candidate) return XR_XIR_OK;
    if (!effect_body_material_shape(candidate) || candidate->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,8+sizeof(EffectBodyMaterial *))) return XR_XIR_BUDGET;
    if (candidate->backbone || effect_body_count(candidate)!=source->function_count || !source->function_count)
        return XR_XIR_OK;
    EffectBodyMaterial *root=candidate->storage_kind==EFFECT_BODY_RECORDS_SHARED?candidate->records_owner:candidate;
    if (!effect_body_material_shape(root) || root->backbone || root->storage_kind!=EFFECT_BODY_RECORDS_OWNED ||
        effect_body_count(root)!=source->function_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRead read=effect_body_compile_read(work);bool same=true;
    for (uint32_t f=0;f<source->function_count;++f) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        XrXirStatus status=effect_body_records_compare(&read,&root->functions[f],&source->functions[f],&same);
        if (status!=XR_XIR_OK) return status;
    }
    if (same) {
        if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
        *output=root;
    }
    return XR_XIR_OK;
}
