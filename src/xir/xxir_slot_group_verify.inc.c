/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_slot_group_verify.inc.c - One exact contiguous group and compact non-Unit operands
 */
static XrXirStatus slot_group_shape(const XrXirFunction *function,const XrXirInstruction *op,
    const XrXirModule *module,const XrXirCompileContext *remaining) {
    const XrXirDeclarations *d=module->declarations;
    uint64_t packed=(uint64_t)op->immediate;uint32_t first=(uint32_t)(packed>>32),count=(uint32_t)packed;
    if(!d || !count || first>d->slot_count || count>d->slot_count-first)return XR_XIR_BAD_STRUCTURE;
    uint32_t caller=(uint32_t)(function-module->functions),owner=d->functions[caller].module,payloads=0;
    if(d->modules[owner].initializer!=caller)return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(remaining,count))return XR_XIR_BUDGET;
    for(uint32_t i=0;i<count;++i) {
        const XrXirSlot *slot=&d->slots[first+i];
        if(slot->module!=owner)return XR_XIR_BAD_STRUCTURE;
        if(slot->type!=XR_XIR_UNIT)++payloads;
    }
    return payloads==op->args[1] ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
