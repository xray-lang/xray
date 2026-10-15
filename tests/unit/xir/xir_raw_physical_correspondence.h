/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_raw_physical_correspondence.h - Raw classification fixture checks
 */
#ifndef XIR_RAW_PHYSICAL_CORRESPONDENCE_H
#define XIR_RAW_PHYSICAL_CORRESPONDENCE_H

/* This test-only projection checks classification storage. Complete raw
 * receiving correspondence and executable permission use the real gates. */
static XrXirStatus invocation_raw_physical_match(const XrXirCompileContext *work,
    const EffectPhysicalDomain *domain,const XrXirModule *module) {
    if (!xir_compile_context_valid(work) || !domain || domain->resources!=work->resources ||
        !module || domain->count!=module->function_count ||
        (domain->count && (!domain->functions || !module->functions))) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<domain->count;++f) {
        const EffectPhysicalFunction *body=&domain->functions[f];
        const XrXirFunction *function=&module->functions[f];
        if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
        if (body->parameters!=function->parameter_count || body->instructions!=function->instruction_count ||
            body->values!=(uint64_t)function->parameter_count+function->instruction_count ||
            body->rows>body->values || body->classes>body->values || body->count>body->capacity || body->capacity>body->values ||
            !!body->records!=!!body->capacity ||
            (function->parameter_count && !function->parameters) ||
            (function->instruction_count && !function->instructions)) return XR_XIR_BAD_STRUCTURE;
        uint32_t row=0,classes=0,at=0;
        for (uint32_t v=0;v<body->values;++v) {
            if (!xir_compile_work(work,v<body->parameters?2:3)) return XR_XIR_BUDGET;
            uint32_t mask=0;
            if (v>=body->parameters) {
                XrXirOp op=function->instructions[v-body->parameters].op;
                if (op<=XR_XIR_INVALID || op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
                mask=effect_physical_opcode(op);
            }
            if (xr_xir_callable_signature(module->types,xr_xir_operand_type(function,v)))
                mask|=EFFECT_PHYSICAL_CALLABLE;
            if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            if (xr_xir_type_is_class(module->types,xr_xir_operand_type(function,v))) {
                mask|=EFFECT_PHYSICAL_CLASS; ++classes;
            }
            if (mask&EFFECT_PHYSICAL_CALLABLE) ++row;
            if (!mask) continue;
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if (at>=body->count || body->records[at].value!=v || body->records[at].mask!=mask)
                return XR_XIR_BAD_STRUCTURE;
            ++at;
        }
        if (at!=body->count || row!=body->rows || classes!=body->classes) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

#endif // XIR_RAW_PHYSICAL_CORRESPONDENCE_H
