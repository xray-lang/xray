/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_callable_bounds.h - Fixed barriers and authentic callback arguments
 */
#ifndef XIR_SOURCE_CALLABLE_BOUNDS_H
#define XIR_SOURCE_CALLABLE_BOUNDS_H
#include "xir/xxir_effect_contract_internal.h"

typedef struct SourceCallableWeakenings { uint32_t identity, root, suspend; } SourceCallableWeakenings;
static const XrXirRootValueIdentity *source_callable_identity(
    const XrXirFunctionEffectContract *contract, uint32_t instruction) {
    const XrXirRootValueIdentity *found = NULL;
    for (uint32_t v = 0; v < contract->value_count; ++v) {
        if (contract->values[v].instruction != instruction) continue;
        CHECK(!found); found = &contract->values[v];
    }
    CHECK(found); return found;
}
static SourceCallableWeakenings source_callable_weakenings(const XrXirArtifact *checked) {
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    const XrXirProvenance *evidence = module->provenance;
    CHECK(evidence && evidence->kind == XR_XIR_EVIDENCE_TEMPLATE);
    CHECK(evidence->contract_count == module->function_count);
    SourceCallableWeakenings counts = {0};
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *body = &module->functions[f];
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            const XrXirInstruction *op = &body->instructions[i];
            if (op->op != XR_XIR_FUNCTION_WEAKEN) continue;
            CHECK(op->args[0] < body->parameter_count + body->instruction_count);
            XrXirType actual = xr_xir_operand_type(body, op->args[0]);
            const XrXirTypeNode *from = xr_xir_callable_signature(module->types, actual);
            const XrXirTypeNode *to = xr_xir_callable_signature(module->types, op->type);
            CHECK(from && to && from->result == to->result && from->parameter_count == to->parameter_count);
            for (uint32_t p = 0; p < from->parameter_count; ++p) {
                CHECK(from->parameters[p].type == to->parameters[p].type);
                CHECK(from->parameters[p].mode == to->parameters[p].mode);
            }
            const XrXirRootValueIdentity *value = source_callable_identity(&evidence->contracts[f], i);
            CHECK(value->mode == XR_XIR_EFFECT_VALUE_FIXED && value->declared_type == op->type);
            if (actual == op->type) { ++counts.identity; continue; }
            uint32_t from_root = from->flags & XR_XIR_CALLABLE_ROOT_MASK;
            uint32_t to_root = to->flags & XR_XIR_CALLABLE_ROOT_MASK;
            uint32_t from_suspend = from->flags & XR_XIR_CALLABLE_NO_SUSPEND;
            uint32_t to_suspend = to->flags & XR_XIR_CALLABLE_NO_SUSPEND;
            CHECK(!to_suspend || from_suspend);
            CHECK(from_root == to_root ||
                (from_root == XR_XIR_CALLABLE_ROOT_NONE && to_root == XR_XIR_CALLABLE_ROOT_UNRESOLVED));
            CHECK(from_root != to_root || from_suspend != to_suspend);
            if (from_root != to_root) ++counts.root;
            if (from_suspend != to_suspend) ++counts.suspend;
        }
    }
    return counts;
}
/* An ordinary formal is a bound, not an instruction to erase the real input.
 * The independently checked variable role keeps both original argument edges. */
static void source_callable_promise_bindings(const XrXirArtifact *checked) {
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    const XrXirProvenance *evidence = module->provenance;
    CHECK(evidence && evidence->kind == XR_XIR_EVIDENCE_TEMPLATE);
    CHECK(evidence->contract_count == module->function_count);
    uint32_t found = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *body = &module->functions[f];
        if (body->name_length != 6 || memcmp(body->name, "weaken", 6)) continue;
        CHECK(!found++ && body->parameter_count == 1);
        const XrXirTypeNode *parameter = xr_xir_callable_signature(module->types, body->parameters[0]);
        CHECK(parameter && parameter->flags == (XR_XIR_CALLABLE_ROOT_UNRESOLVED | XR_XIR_CALLABLE_NO_SUSPEND));
        const XrXirFunctionEffectContract *contract = &evidence->contracts[f];
        CHECK(contract->parameter_count == 1 && contract->parameters[0].kind == XR_XIR_EFFECT_PARAMETER_VARIABLE);
        CHECK(contract->binding_count == 2 && contract->value_count == 2);
        uint32_t targets = 0;
        for (uint32_t b = 0; b < contract->binding_count; ++b) {
            const XrXirEffectCallBinding *binding = &contract->bindings[b];
            CHECK(binding->family == XR_XIR_EFFECT_BINDING_DIRECT && binding->parameter == 0);
            CHECK(binding->instruction < body->instruction_count);
            const XrXirInstruction *call = &body->instructions[binding->instruction];
            CHECK(call->op == XR_XIR_CALL && call->args[1] == 1);
            CHECK(call->immediate >= 0 && (uint64_t)call->immediate < module->function_count);
            CHECK(call->args[0] < body->operand_count);
            CHECK(body->operands[call->args[0]] == binding->value);
            const XrXirFunction *callee = &module->functions[call->immediate];
            bool dynamic = callee->name_length == 7 && !memcmp(callee->name, "dynamic", 7);
            bool handled = callee->name_length == 14 && !memcmp(callee->name, "dynamicHandled", 14);
            CHECK(dynamic || handled); uint32_t target = dynamic ? 1u : 2u;
            CHECK(!(targets & target)); targets |= target;
            CHECK(callee->parameter_count == 1);
            const XrXirFunctionEffectContract *callee_contract = &evidence->contracts[call->immediate];
            CHECK(callee_contract->parameter_count == 1);
            CHECK(callee_contract->parameters[0].kind == XR_XIR_EFFECT_PARAMETER_VARIABLE);
            CHECK(binding->value >= body->parameter_count &&
                binding->value - body->parameter_count < body->instruction_count);
            uint32_t instruction = binding->value - body->parameter_count;
            const XrXirInstruction *copy = &body->instructions[instruction];
            CHECK(copy->op == XR_XIR_COPY && copy->args[0] == 0 && copy->type == body->parameters[0]);
            const XrXirRootValueIdentity *value = source_callable_identity(contract, instruction);
            CHECK(value->mode == XR_XIR_EFFECT_VALUE_CALL_BIND && value->declared_type == callee->parameters[0]);
            const XrXirTypeNode *bound = xr_xir_callable_signature(module->types, value->declared_type);
            CHECK(bound && bound->flags == (XR_XIR_CALLABLE_ROOT_UNRESOLVED |
                (dynamic ? XR_XIR_CALLABLE_NO_SUSPEND : 0)));
        }
        CHECK(targets == 3);
    }
    CHECK(found == 1);
}
#endif // XIR_SOURCE_CALLABLE_BOUNDS_H
