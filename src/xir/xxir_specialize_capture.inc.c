/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_specialize_capture.inc.c - Final scalar capture identities before calls
 *
 * KEY CONCEPT:
 *   An unresolved value keeps its declared bound. A dependent callee is interned
 *   only after this finite solver has finalized its actual operand types.
 */
/* Final scalar structures are copied from the same authentic context owner
 * that selected every dense vector. No second scalar solver grants precision. */
static bool spec_capture_flow(SpecContext *c, uint32_t index) {
    const SpecInstance *instance=&c->instances[index];
    XrXirOrigin origin={instance->declaration,instance->arguments,instance->count,
        instance->effects,instance->effect_count};
    XrXirOrigin *owners=NULL;uint32_t owner_count=0;
    if (!spec_effect_owners(c,index,&owners,&owner_count)) return false;
    XirEffectContextInput input={c->source,&c->types,&origin,UINT32_MAX,owners,owner_count};
    XirEffectContextView selected={0};
    c->diagnostic.status=xir_effects_context_select(&c->remaining,c->effects,&input,&selected);
    if (c->diagnostic.status!=XR_XIR_OK) return false;
    XrXirFunction *function=&c->functions[index];
    if (selected.declaration!=instance->declaration || selected.parameter_count!=function->parameter_count ||
        selected.function->instruction_count!=function->instruction_count) {
        c->diagnostic.status=XR_XIR_BAD_STRUCTURE;return false;
    }
    const XrXirType *cache=spec_effect_import(c,selected.types);
    if (selected.types->count && !cache) return false;
    XrXirInstruction *ops=(XrXirInstruction *)function->instructions;
    for (uint32_t i=0;i<function->instruction_count;++i) {
        if (!spec_work(c,2)) return false;
        ops[i].type=spec_effect_child(c,cache,selected.types->count,selected.function->instructions[i].type);
        if (c->diagnostic.status!=XR_XIR_OK) return false;
    }
    return true;
}
