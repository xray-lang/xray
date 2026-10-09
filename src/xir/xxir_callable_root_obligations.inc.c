/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_callable_root_obligations.inc.c - Authentic body bounds on shared facts
 *
 * KEY CONCEPT:
 *   A value's public execution bound must cover its body even before invocation.
 *   Constructing a value adds no body-execution edge to its creator.
 */
#include "xxir_implementation.h"
static XrXirStatus declaration_callable_root_verify(const XrXirModule *module,
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
            XrXirStatus status=xir_effects_reference_root(remaining,effects,module,f,i,&mask);
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
            const XrXirRootEffects *actual = xr_xir_effects_root(effects, binding->function);
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
