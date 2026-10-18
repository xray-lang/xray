/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_reference_root_scalar_cases.h - Independent complete scalar Source and ROOT checks
 */
#ifndef XIR_REFERENCE_ROOT_SCALAR_CASES_H
#define XIR_REFERENCE_ROOT_SCALAR_CASES_H
static XrXirStatus root_scalar_reference(const XrXirCompileContext *context,
    const XrXirEffects *effects, const XrXirModule *module,
    uint32_t function, uint32_t instruction, uint32_t *output) {
    if (!output || !module || !xir_effects_context_matches(context,effects,module->function_count) ||
        function>=module->function_count || instruction>=module->functions[function].instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&module->functions[function].instructions[instruction];
    if (op->op!=XR_XIR_FUNCTION_REF || op->immediate<0 || (uint64_t)op->immediate>=effects->count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t mask=0;
    if (effect_contexts_read(effects) && (!module->generics || !module->generics[function].parameter_count)) {
        XrXirOrigin origin={.function=function};
        XrXirOrigin *owners=NULL;uint32_t owner_count=0;
        XrXirStatus status=effect_context_base_owner_origins(context,module,function,&owners,&owner_count);
        XirEffectContextInput input={module,module->types,&origin,instruction,owners,owner_count};
        XirEffectContextView selected={0};
        if (status==XR_XIR_OK) status=xir_effects_context_select(context,effects,&input,&selected);
        xr_compile_resources_free(owners);
        if (status!=XR_XIR_OK) return status;
        mask=(selected.requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
            (selected.unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    } else if (effect_contracts_read(effects) && !effect_contexts_read(effects)) {
        XrXirStatus status=effect_invocation_effect_edge_mask(context,effects,
            function,instruction,(uint32_t)op->immediate,&mask);
        if (status!=XR_XIR_OK) return status;
    } else {
        const XrXirRootEffects *facts=&effect_roots_read(effects)->root[op->immediate];
        mask=(facts->requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
            (facts->unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    }
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    *output=mask;return XR_XIR_OK;
}

static XrXirStatus root_scalar_declaration(const XrXirModule *module,
    const XrXirEffects *effects, XrXirCompileContext *remaining, XrXirDiagnostic *diagnostic) {
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_FUNCTION_REF) continue;
            const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, op->type);
            uint32_t mask=0;
            if (!signature) return XR_XIR_BAD_STRUCTURE;
            XrXirStatus status=root_scalar_reference(remaining,effects,module,f,i,&mask);
            if (status!=XR_XIR_OK) return status;
            if (!xr_xir_callable_root_accepts(!!(mask&XR_XIR_CALLABLE_ROOT_REQUIRED),
                !!(mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED),signature->flags)) {
                *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, f, UINT32_MAX, i, XR_XIR_DIAGNOSTIC_NONE};
                return XR_XIR_BAD_TYPE;
            }
        }
    }
    const XrXirImplementationTable *table = module->declarations->implementations;
    for (uint32_t r = 0; table && r < table->count; ++r) {
        const XrXirImplementation *record = &table->records[r];
        for (uint32_t b = 0; b < record->binding_count; ++b) {
            if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
            const XrXirImplementationBinding *binding = &record->bindings[b];
            const XrXirInterfaceTable *interfaces = module->types ? module->types->interfaces : NULL;
            if (!interfaces || binding->requirement.declaration >= interfaces->count ||
                binding->member >= interfaces->declarations[binding->requirement.declaration].method_count)
                return XR_XIR_BAD_STRUCTURE;
            const XrXirInterfaceMethod *method =
                &interfaces->declarations[binding->requirement.declaration].methods[binding->member];
            const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, method->signature);
            XrXirEffectFacts actual_read=xr_xir_effects_read(effects, binding->function);
            const XrXirRootEffects *actual=(actual_read.status==XR_XIR_OK?&actual_read.root:NULL);
            if (!signature || !actual) return XR_XIR_BAD_STRUCTURE;
            if (!xr_xir_callable_root_accepts(actual->requires_root, actual->unresolved, signature->flags)) {
                *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, binding->function, UINT32_MAX, UINT32_MAX,
                    XR_XIR_DIAGNOSTIC_NONE};
                return XR_XIR_BAD_TYPE;
            }
        }
    }
    return XR_XIR_OK;
}

#endif // XIR_REFERENCE_ROOT_SCALAR_CASES_H
