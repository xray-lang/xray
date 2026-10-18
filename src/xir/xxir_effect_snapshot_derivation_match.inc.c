/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_derivation_match.inc.c - Complete owned derivation metadata correspondence
 */
#include "xxir_derivation_internal.h"

static XrXirStatus effect_body_derivation_origin_match(const XrXirCompileContext *work,
    const XrXirGeneratedOrigin *left,const XrXirGeneratedOrigin *right,bool *same) {
    if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
    if (left->line!=right->line || left->column!=right->column ||
        left->module.length!=right->module.length || left->file.length!=right->file.length ||
        left->field_path.length!=right->field_path.length) return effect_body_difference(same);
    XrXirStatus status=effect_body_bytes_match(work,left->module.bytes,right->module.bytes,
        left->module.length,1,same);
    if (status==XR_XIR_OK) status=effect_body_bytes_match(work,left->file.bytes,right->file.bytes,
        left->file.length,1,same);
    if (status==XR_XIR_OK) status=effect_body_bytes_match(work,left->field_path.bytes,right->field_path.bytes,
        left->field_path.length,1,same);
    return status;
}

static XrXirStatus effect_body_derivation_policy_match(const XrXirCompileContext *work,
    const XrXirDerivationPolicy *left,const XrXirDerivationPolicy *right,bool *same) {
    if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
    if (left->grantor_module!=right->grantor_module || left->target!=right->target ||
        left->generator!=right->generator || left->constructor!=right->constructor ||
        left->member_count!=right->member_count) return effect_body_difference(same);
    XrXirStatus status=effect_body_application_match(work,&left->protocol,&right->protocol,same);
    if (status==XR_XIR_OK) status=effect_body_bytes_match(work,left->members,right->members,
        left->member_count,sizeof(*left->members),same);
    if (status==XR_XIR_OK) status=effect_body_derivation_origin_match(work,&left->origin,&right->origin,same);
    return status;
}

static XrXirStatus effect_body_derivation_grant_match(const XrXirCompileContext *work,
    const XrXirDerivationGrant *left,const XrXirDerivationGrant *right,bool *same) {
    if (!xir_compile_work(work,6)) return XR_XIR_BUDGET;
    if (left->function!=right->function || left->policy!=right->policy || left->source_row!=right->source_row ||
        left->root!=right->root || left->target!=right->target ||
        left->origins.instruction_count!=right->origins.instruction_count) return effect_body_difference(same);
    XrXirStatus status=effect_body_derivation_origin_match(work,&left->origins.entry,&right->origins.entry,same);
    for (uint32_t i=0;i<left->origins.instruction_count && status==XR_XIR_OK;++i)
        status=effect_body_derivation_origin_match(work,&left->origins.instructions[i],&right->origins.instructions[i],same);
    return status;
}

/* Policies and grants are data owned by this snapshot. Their equality does not
 * replace the ordinary module's independent derivation or operation proof. */
static XrXirStatus effect_body_derivations_match(const XrXirCompileContext *work,
    const XrXirDerivationTable *left,const XrXirDerivationTable *right,bool *same) {
    if (!left && !right) return XR_XIR_OK;
    XrXirStatus status=xir_derivations_structure_verify(work,left);
    if (status==XR_XIR_OK) status=xir_derivations_structure_verify(work,right);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
    if (!!left!=!!right || (left && (left->count!=right->count || left->policy_count!=right->policy_count)))
        return effect_body_difference(same);
    for (uint32_t i=0;i<left->policy_count && status==XR_XIR_OK;++i)
        status=effect_body_derivation_policy_match(work,&left->policies[i],&right->policies[i],same);
    for (uint32_t i=0;i<left->count && status==XR_XIR_OK;++i)
        status=effect_body_derivation_grant_match(work,&left->records[i],&right->records[i],same);
    return status;
}
