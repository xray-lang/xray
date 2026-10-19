/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_derivation_match.inc.c - Exact current declaration data correspondence
 *
 * KEY CONCEPT:
 *   Matching data supplies no body, module or execution permission.
 */
#include <string.h>
static XrXirStatus derivation_match_difference(bool *same) {
    if(!same)return XR_XIR_BAD_STRUCTURE;
    *same=false;return XR_XIR_OK;
}
static XrXirStatus derivation_match_text(const XrXirCompileContext *context,
    XrXirGeneratedText left,XrXirGeneratedText right,bool *same) {
    if(!xir_compile_work(context,1))return XR_XIR_BUDGET;
    if(left.length!=right.length)return derivation_match_difference(same);
    if(!xir_compile_work(context,left.length))return XR_XIR_BUDGET;
    if(left.length && memcmp(left.bytes,right.bytes,left.length))
        return derivation_match_difference(same);
    return XR_XIR_OK;
}
static XrXirStatus derivation_match_origin(const XrXirCompileContext *context,
    const XrXirGeneratedOrigin *left,const XrXirGeneratedOrigin *right,bool *same) {
    if(!xir_compile_work(context,2))return XR_XIR_BUDGET;
    if(left->line!=right->line || left->column!=right->column)
        return derivation_match_difference(same);
    XrXirStatus status=derivation_match_text(context,left->module,right->module,same);
    if(status==XR_XIR_OK)status=derivation_match_text(context,left->file,right->file,same);
    if(status==XR_XIR_OK)status=derivation_match_text(context,left->field_path,right->field_path,same);
    return status;
}
static XrXirStatus derivation_match_policy(const XrXirCompileContext *context,
    const XrXirDerivationPolicy *left,const XrXirDerivationPolicy *right,bool *same) {
    if(!xir_compile_work(context,7))return XR_XIR_BUDGET;
    if(left->grantor_module!=right->grantor_module || left->target!=right->target ||
        left->protocol.declaration!=right->protocol.declaration ||
        left->protocol.argument_count!=right->protocol.argument_count ||
        left->generator!=right->generator || left->constructor!=right->constructor ||
        left->member_count!=right->member_count)return derivation_match_difference(same);
    XrXirStatus status=derivation_match_origin(context,&left->origin,&right->origin,same);
    for(uint32_t a=0;status==XR_XIR_OK && a<left->protocol.argument_count;++a) {
        if(!xir_compile_work(context,1))return XR_XIR_BUDGET;
        if(left->protocol.arguments[a]!=right->protocol.arguments[a])status=derivation_match_difference(same);
    }
    for(uint32_t m=0;status==XR_XIR_OK && m<left->member_count;++m) {
        if(!xir_compile_work(context,2))return XR_XIR_BUDGET;
        if(left->members[m].field!=right->members[m].field ||
            left->members[m].operations!=right->members[m].operations)
            status=derivation_match_difference(same);
    }
    return status;
}
static XrXirStatus derivation_match_binding(const XrXirCompileContext *context,
    const XrXirDerivationGrant *left,const XrXirDerivationGrant *right,bool *same) {
    if(!xir_compile_work(context,6))return XR_XIR_BUDGET;
    if(left->function!=right->function || left->policy!=right->policy ||
        left->source_row!=right->source_row || left->root!=right->root || left->target!=right->target ||
        left->origins.instruction_count!=right->origins.instruction_count)
        return derivation_match_difference(same);
    XrXirStatus status=derivation_match_origin(context,&left->origins.entry,&right->origins.entry,same);
    for(uint32_t n=0;status==XR_XIR_OK && n<left->origins.instruction_count;++n)
        status=derivation_match_origin(context,&left->origins.instructions[n],&right->origins.instructions[n],same);
    return status;
}
/* Both complete initialized shapes are read before reporting a data change.
 * Private change detection must never swallow malformed storage or a budget
 * failure, even if an earlier field already differed. */
XR_FUNC XrXirStatus xir_derivations_match(const XrXirCompileContext *context,
    const XrXirDerivationTable *left,const XrXirDerivationTable *right,bool *same) {
    XrXirStatus status=xir_derivations_structure_verify(context,left);
    if(status==XR_XIR_OK)status=xir_derivations_structure_verify(context,right);
    if(status!=XR_XIR_OK)return status;
    if(!xir_compile_work(context,1))return XR_XIR_BUDGET;
    if(!!left!=!!right)return derivation_match_difference(same);
    if(!left)return XR_XIR_OK;
    if(!xir_compile_work(context,2))return XR_XIR_BUDGET;
    if(left->policy_count!=right->policy_count || left->count!=right->count)
        return derivation_match_difference(same);
    for(uint32_t p=0;status==XR_XIR_OK && p<left->policy_count;++p)
        status=derivation_match_policy(context,&left->policies[p],&right->policies[p],same);
    for(uint32_t n=0;status==XR_XIR_OK && n<left->count;++n)
        status=derivation_match_binding(context,&left->records[n],&right->records[n],same);
    return status;
}
