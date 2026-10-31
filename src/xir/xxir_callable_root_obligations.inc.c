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
#include "xxir_effect_reference_internal.h"
typedef struct DeclarationCallableRootCheck {
    XirEffectInput input;
    const XrXirEffects *effects;
    XrXirCompileContext *remaining;
    XrXirDiagnostic *diagnostic;
} DeclarationCallableRootCheck;
static XrXirStatus declaration_callable_root_read(void *data,XirEffectContextScope *scope) {
    DeclarationCallableRootCheck *check=data;const XrXirModule *module=check->input.module;
    const XrXirEffects *effects=check->effects;XrXirCompileContext *remaining=check->remaining;
    XrXirDiagnostic *diagnostic=check->diagnostic;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {

            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_FUNCTION_REF) continue;
            const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, op->type);
            uint32_t mask=0;
            if (!signature) return XR_XIR_BAD_STRUCTURE;
            XrXirStatus status=scope ? xir_effects_reference_root_scoped(scope,f,i,&mask) :
                xir_effects_reference_root_input(remaining,effects,check->input,f,i,&mask);
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

/* This one read-only proof retains neither the scope nor an input mutation. */
static XrXirStatus declaration_callable_root_verify(XirEffectInput input,
    const XrXirEffects *effects,XrXirCompileContext *remaining,XrXirDiagnostic *diagnostic) {
    const XrXirModule *module=input.module;
    DeclarationCallableRootCheck check={input,effects,remaining,diagnostic};

    return xir_effects_context_available(remaining,effects) ?
        xir_effects_context_operation_input(remaining,effects,(XirEffectInput){module,input.construction,input.control},module->types,
            declaration_callable_root_read,&check) : declaration_callable_root_read(&check,NULL);
}
