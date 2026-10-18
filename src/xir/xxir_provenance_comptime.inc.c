/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_provenance_comptime.inc.c - Independent current closed-key correspondence
 *
 * KEY CONCEPT:
 *   Instance calls independently select the current Source binding and original provider.
 */
static XrXirStatus provenance_comptime_match(ProvenanceMatch *c,const XrXirOrigin *origin,
    uint32_t instruction,const XrXirInstruction *actual,bool *matched) {
    *matched=false;
    XirComptimeSelection selection={c->source,c->destination->types,origin->function,instruction,origin->arguments,origin->argument_count};
    const XrXirComptimeBinding *binding=NULL;
    XrXirStatus status=xir_comptime_select(c->remaining,c->instance->source->construction,&selection,&binding);
    if (status==XR_XIR_BAD_TYPE && c->instance->kind==XR_XIR_EVIDENCE_EVALUATION) return XR_XIR_OK;
    if (status!=XR_XIR_OK || !binding) return status;
    if (actual->immediate<0 || (uint64_t)actual->immediate>=c->count || actual->type_arguments[0] || actual->type_arguments[1] || actual->args[1])
        return XR_XIR_BAD_STRUCTURE;
    const XrXirOrigin *target=&c->origins[actual->immediate];
    if (target->function!=binding->provider || target->argument_count || target->effect_argument_count ||
        c->destination->functions[actual->immediate].parameter_count) return XR_XIR_BAD_STRUCTURE;
    *matched=true;return XR_XIR_OK;
}
/* Full body, witness, constraint and reachability proof precedes this root
 * observation. A missing key grants only bounded compiler evaluation. */
static XrXirStatus provenance_evaluation_root(ProvenanceMatch *c) {
    if (c->instance->kind!=XR_XIR_EVIDENCE_EVALUATION) return XR_XIR_OK;
    const XirEvaluationRoot *root=&c->instance->evaluation;
    if (!xir_compile_work(c->remaining,sizeof(*root)+8)) return XR_XIR_BUDGET;
    if (root->caller>=c->count || root->callee>=c->count ||
        root->instruction>=c->destination->functions[root->caller].instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirOrigin *caller=&c->origins[root->caller],*callee=&c->origins[root->callee];
    const XrXirInstruction *op=&c->destination->functions[root->caller].instructions[root->instruction];
    const XrXirComptimeSite *site=xir_comptime_site(c->instance->source->construction,caller->function,root->instruction);
    if (!site || site->helper!=callee->function || !callee->argument_count ||
        c->destination->functions[root->callee].parameter_count ||
        (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE) || op->immediate!=root->callee || op->args[1])
        return XR_XIR_BAD_STRUCTURE;
    const XrXirComptimeBinding *binding=NULL;
    XirComptimeSelection selection={c->source,c->destination->types,caller->function,root->instruction,
        caller->arguments,caller->argument_count};
    XrXirStatus status=xir_comptime_select(c->remaining,c->instance->source->construction,&selection,&binding);
    return status==XR_XIR_BAD_TYPE?XR_XIR_OK:status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:status;
}
