/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_module_export_internal.h - Owned numeric module effect projection
 */
#ifndef XXIR_EFFECT_MODULE_EXPORT_INTERNAL_H
#define XXIR_EFFECT_MODULE_EXPORT_INTERNAL_H
#include "xxir_effects.h"
#include "xxir_effect_input_internal.h"

typedef enum XirModuleEffectTermKind {
    XIR_MODULE_EFFECT_SLOT, XIR_MODULE_EFFECT_CALLBACK, XIR_MODULE_EFFECT_CAPTURE,
    XIR_MODULE_EFFECT_UNKNOWN, XIR_MODULE_EFFECT_FIELD, XIR_MODULE_EFFECT_PARAMETER
} XirModuleEffectTermKind;
typedef struct XirModuleEffectSpan { size_t first, count; } XirModuleEffectSpan;

/* Calling CALLBACK(callback_parameter) while an entry loan from PARAMETER or
 * CAPTURE(loan_parameter) is live requires its actual environment's writes to
 * be apart from that loan. Both ordinals include the physical capture prefix.
 * This symbolic relation is a caller obligation, never an empty WRITE fact or
 * a grant to invoke an unknown callable. Only readonly Slice loans and
 * nonescaping callable parameters participate in the initial protocol. */
typedef struct XirModuleCallbackLoan {
    uint32_t callback_parameter;
    XirModuleEffectTermKind loan_kind;
    uint32_t loan_parameter;
    uint32_t witness_function, witness_instruction;
} XirModuleCallbackLoan;
typedef struct XirModuleEffectTerm {
    XirModuleEffectTermKind kind;
    uint32_t identity, witness_function, witness_instruction;
} XirModuleEffectTerm;
typedef struct XirModuleEffectFunction {
    uint32_t module, parameter_count, capture_count;
    size_t first_value, value_count;
    XirModuleEffectSpan access, write, result_origin, callback_loans;
} XirModuleEffectFunction;
typedef struct XirModuleEffectValue { XirModuleEffectSpan origin; } XirModuleEffectValue;
typedef struct XirModuleEffectDependency { size_t first, count; } XirModuleEffectDependency;
typedef struct XirModuleEffectSlot { uint32_t module; } XirModuleEffectSlot;
typedef struct XirModuleEffectExportView {
    const XirModuleEffectFunction *functions;
    const XirModuleEffectValue *values;
    const XirModuleEffectTerm *terms;
    const XirModuleEffectDependency *modules;
    const uint32_t *dependencies;
    const XirModuleEffectSlot *slots;
    const XirModuleCallbackLoan *callback_loans;
    uint32_t function_count, module_count, slot_count;
    size_t value_count, term_count, dependency_count, callback_loan_count;
    /* False means this input did not request module equations. Empty spans in
     * that case are unavailable facts, never proof of an empty WRITE set. */
    bool module_access_complete;
} XirModuleEffectExportView;
typedef struct XirModuleEffectExport XirModuleEffectExport;

/* Copy only completed equations from this exact immutable input. Terms stay
 * symbolic, including unknowns; absence is never a substitute for an unknown
 * write. This projection grants no call, origin, loan or mutation permission.
 * Function rows use the actual Artifact directory, including specialized
 * instances relocated from their retained Source derivation. Parameter and
 * State identities keep that producer's physical ordinals. Diagnostic witnesses
 * relocate to an actual instance or UINT32_MAX when no instance exists; an
 * unavailable witness never removes its symbolic dependency. Lower performs
 * its actual module/slot/function relocation before publishing receiving facts.
 * No Effects, descriptor-pool, construction or AST pointer survives the call.
 * Failure preserves an empty output; the independent owner survives all
 * producer retirement, with its charged allocations retaining their ledger. */
XR_FUNC XrXirStatus xir_effects_module_export_copy(const XrXirArtifact *, XirModuleEffectExport **empty);
XR_FUNC XrXirStatus xir_effects_module_export_view(const XirModuleEffectExport *, XirModuleEffectExportView *);
XR_FUNC void xir_effects_module_export_free(XirModuleEffectExport *);
#endif
