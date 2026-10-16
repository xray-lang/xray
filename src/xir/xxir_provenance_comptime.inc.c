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
    if (status!=XR_XIR_OK || !binding) return status;
    if (actual->immediate<0 || (uint64_t)actual->immediate>=c->count || actual->type_arguments[0] || actual->type_arguments[1] || actual->args[1])
        return XR_XIR_BAD_STRUCTURE;
    const XrXirOrigin *target=&c->origins[actual->immediate];
    if (target->function!=binding->provider || target->argument_count || target->effect_argument_count ||
        c->destination->functions[actual->immediate].parameter_count) return XR_XIR_BAD_STRUCTURE;
    *matched=true;return XR_XIR_OK;
}
