/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_layout.c - Target-bound scalar layout authority
 *
 * KEY CONCEPT:
 *   Physical consumers read frozen offsets; only this service chooses layouts.
 */

#include "xxir_internal.h"
#include "xxir_types.h"
#include "xxir_operand_roles.h"
#include "xxir_value_place.h"
#include "../base/xmalloc.h"

static bool layout_equal(XrXirLayout left, XrXirLayout right) {
    return left.size == right.size && left.alignment == right.alignment;
}

static XrXirType slot_type(const XrXirFunction *function, uint32_t slot) {
    if (slot < function->parameter_count) return function->parameters[slot];
    const XrXirInstruction *op = &function->instructions[slot - function->parameter_count];
    return op->op == XR_XIR_INVOKE || op->op == XR_XIR_INVOKE_INDIRECT ? XR_XIR_UNIT : op->type;
}

static bool subtract_bytes(uint64_t *remaining, uint64_t amount) {
    if (amount > *remaining)
        return false;
    *remaining -= amount;
    return true;
}

static XrXirStatus path_capacity(const XrXirFunction *function, uint64_t *work, uint32_t *capacity) {
    *capacity = 0;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        if (!subtract_bytes(work, 1)) return XR_XIR_BUDGET;
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op == XR_XIR_FIELD_PLACE || op->op == XR_XIR_INDEX_PLACE ||
            xr_xir_operand_role(op->op, 0) == XR_XIR_OPERAND_VALUE) continue;
        uint32_t id = xr_xir_op_uses_operand_table(op->op) ? function->operands[op->args[0]] : op->args[0];
        uint32_t depth = 0;
        for (;;) {
            XrXirPlaceKind kind = xr_xir_place_kind(function, id);
            if (kind != XR_XIR_PLACE_FIELD && kind != XR_XIR_PLACE_INDEX) break;
            if (!subtract_bytes(work, 1)) return XR_XIR_BUDGET;
            if (depth == function->instruction_count) return XR_XIR_BAD_VALUE;
            ++depth; id = function->instructions[id - function->parameter_count].args[0];
        }
        if (depth && (op->op == XR_XIR_ARRAY_GET || op->op == XR_XIR_ARRAY_SET || op->op == XR_XIR_STRUCT_SET)) {
            if (depth == UINT32_MAX) return XR_XIR_BUDGET;
            ++depth;
        }
        if (depth > *capacity) *capacity = depth;
    }
    return XR_XIR_OK;
}
static XrXirStatus layout_budget(const XrXirModule *module, const XrXirBudget *budget) {
    uint64_t bytes = budget->metadata_bytes, work = budget->work;
    if (!subtract_bytes(&bytes, sizeof(XrXirArtifact)))
        return XR_XIR_BUDGET;
    XrXirBudget signature_budget = *budget;
    signature_budget.metadata_bytes = bytes; signature_budget.work = work;
    XrXirStatus signature_status = xr_xir_types_structure_verify(module->types, &signature_budget);
    if (signature_status != XR_XIR_OK) return signature_status;
    XrXirStatus declaration_status = xr_xir_declarations_verify(module->declarations, module->types,
        module->function_count, &signature_budget);
    if (declaration_status != XR_XIR_OK) return declaration_status;
    bytes = signature_budget.metadata_bytes; work = signature_budget.work;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        uint32_t paths = 0;
        XrXirStatus path_status = path_capacity(function, &work, &paths);
        if (path_status != XR_XIR_OK) return path_status;
        uint64_t slots = (uint64_t) function->parameter_count + function->instruction_count;
        uint64_t required = sizeof(*function) + sizeof(XrXirFunctionLayout) +
            function->name_length +
            (uint64_t) function->parameter_count * (sizeof(XrXirType) + sizeof(XrXirLayout)) +
            (uint64_t) function->block_count * sizeof(XrXirBlock) +
            (uint64_t) function->instruction_count * sizeof(XrXirInstruction) +
            (uint64_t) function->operand_count * sizeof(uint32_t) +
            slots * sizeof(uint32_t) * 3;
        if (slots > UINT32_MAX / 2 || required > SIZE_MAX ||
            !subtract_bytes(&bytes, required) || !subtract_bytes(&work, slots + 1))
            return XR_XIR_BUDGET;
    }
    return XR_XIR_OK;
}

static XrXirStatus function_layout(XrXirArtifact *artifact, uint32_t index,
                                   const XrXirBudget *budget, bool create) {
    const XrXirFunction *function = &artifact->module.functions[index];
    XrXirFunctionLayout *layout = &artifact->layouts[index];
    uint32_t slots = function->parameter_count + function->instruction_count;
    if (create) {
        layout->slot_count = slots;
        layout->offsets = xr_calloc(slots, sizeof(*layout->offsets));
        layout->owned_offsets = xr_calloc((size_t) slots * 2, sizeof(*layout->owned_offsets));
        if (!layout->offsets || !layout->owned_offsets)
            return XR_XIR_OUT_OF_MEMORY;
        if (function->parameter_count) {
            layout->parameters = xr_calloc(function->parameter_count, sizeof(*layout->parameters));
            if (!layout->parameters)
                return XR_XIR_OUT_OF_MEMORY;
        }
    } else if (layout->slot_count != slots || !layout->offsets || !layout->owned_offsets ||
               (function->parameter_count && !layout->parameters) ||
               (!function->parameter_count && layout->parameters)) {
        return XR_XIR_BAD_LAYOUT;
    }
    uint32_t bytes = 0, owned = 0;
    for (uint32_t slot = 0; slot < slots; ++slot) {
        XrXirPlaceKind place = xr_xir_place_kind(function, slot);
        if (place == XR_XIR_PLACE_CELL || place == XR_XIR_PLACE_SLOT ||
            place == XR_XIR_PLACE_FIELD || place == XR_XIR_PLACE_INDEX) {
            if (create) ((uint32_t *) layout->offsets)[slot] = UINT32_MAX;
            else if (layout->offsets[slot] != UINT32_MAX) return XR_XIR_BAD_LAYOUT;
            continue;
        }
        XrXirLayout physical;
        XrXirType type = slot_type(function, slot);
        XrXirStatus status = xr_xir_layout(artifact->module.types, type, &artifact->target, XR_XIR_LAYOUT_FRAME, &physical);
        if (status != XR_XIR_OK)
            return status;
        bool phi = slot >= function->parameter_count &&
            function->instructions[slot - function->parameter_count].op == XR_XIR_PHI;
        uint32_t offset = physical.size ? bytes : UINT32_MAX;
        uint32_t stride = physical.size;
        if (phi) physical.size *= 2;
        if (physical.size > UINT32_MAX - bytes ||
            (uint64_t) bytes + physical.size > budget->frame_bytes)
            return XR_XIR_BUDGET;
        bytes += physical.size;
        if (create)
            ((uint32_t *) layout->offsets)[slot] = offset;
        else if (layout->offsets[slot] != offset)
            return XR_XIR_BAD_LAYOUT;
        if (xr_xir_type_is_owned(artifact->module.types, type)) {
            if (create) ((uint32_t *) layout->owned_offsets)[owned] = offset;
            else if (owned >= layout->owned_count || layout->owned_offsets[owned] != offset)
                return XR_XIR_BAD_LAYOUT;
            ++owned;
            if (phi) {
                if (create) ((uint32_t *) layout->owned_offsets)[owned] = offset + stride;
                else if (owned >= layout->owned_count || layout->owned_offsets[owned] != offset + stride)
                    return XR_XIR_BAD_LAYOUT;
                ++owned;
            }
        }
        if (slot < function->parameter_count) {
            status = xr_xir_layout(artifact->module.types, type, &artifact->target, XR_XIR_LAYOUT_PARAMETER, &physical);
            if (status != XR_XIR_OK)
                return status;
            if (create)
                ((XrXirLayout *) layout->parameters)[slot] = physical;
            else if (!layout_equal(layout->parameters[slot], physical))
                return XR_XIR_BAD_LAYOUT;
        }
    }
    XrXirLayout result;
    uint32_t outgoing = 0;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        const XrXirInstruction *op = &function->instructions[i];
        uint32_t count = op->op == XR_XIR_OUTPUT || op->op == XR_XIR_WRITE_STREAM ? 1 :
            xr_xir_op_references_function(op->op) || op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_CALL_INDIRECT ||
            op->op == XR_XIR_PRINT || op->op == XR_XIR_ARRAY_NEW || op->op == XR_XIR_STRUCT_NEW ||
            op->op == XR_XIR_ENUM_NEW ? op->args[1] : 0;
        if (count > outgoing) outgoing = count;
    }
    uint64_t path_work = budget->work; uint32_t paths = 0;
    XrXirStatus path_status = path_capacity(function, &path_work, &paths);
    if (path_status != XR_XIR_OK) return path_status;
    uint64_t physical_bytes = bytes + (uint64_t) outgoing * sizeof(XrXirValue) +
        (uint64_t) paths * sizeof(XrXirValuePathStep);
    if (physical_bytes > UINT32_MAX || physical_bytes > budget->frame_bytes) return XR_XIR_BUDGET;
    XrXirStatus status = xr_xir_layout(artifact->module.types, function->result, &artifact->target, XR_XIR_LAYOUT_RESULT, &result);
    if (status != XR_XIR_OK)
        return status;
    if (create) {
        layout->result = result;
        layout->frame_bytes = bytes;
        layout->owned_count = owned;
        layout->outgoing_count = outgoing;
        layout->path_count = paths;
    } else if (!layout_equal(layout->result, result) || layout->frame_bytes != bytes ||
               layout->owned_count != owned || layout->outgoing_count != outgoing || layout->path_count != paths) {
        return XR_XIR_BAD_LAYOUT;
    }
    return XR_XIR_OK;
}

XrXirStatus xr_xir_layout_build(XrXirArtifact *artifact, const XrXirBudget *budget) {
    XrXirStatus status = layout_budget(&artifact->module, budget);
    if (status != XR_XIR_OK)
        return status;
    artifact->layouts = xr_calloc(artifact->module.function_count, sizeof(*artifact->layouts));
    if (!artifact->layouts)
        return XR_XIR_OUT_OF_MEMORY;
    for (uint32_t f = 0; f < artifact->module.function_count; ++f) {
        status = function_layout(artifact, f, budget, true);
        if (status != XR_XIR_OK)
            return status;
    }
    return XR_XIR_OK;
}

XrXirStatus xr_xir_layout_verify(const XrXirArtifact *artifact, const XrXirBudget *budget) {
    if (artifact->module.stage != XR_XIR_LOWERED)
        return (!artifact->layouts && !artifact->target.architecture && !artifact->target.abi_version)
            ? XR_XIR_OK : XR_XIR_BAD_LAYOUT;
    if (!artifact->layouts)
        return XR_XIR_BAD_LAYOUT;
    XrXirStatus status = layout_budget(&artifact->module, budget);
    if (status != XR_XIR_OK)
        return status;
    for (uint32_t f = 0; f < artifact->module.function_count; ++f) {
        status = function_layout((XrXirArtifact *) artifact, f, budget, false);
        if (status != XR_XIR_OK)
            return status;
    }
    return XR_XIR_OK;
}
