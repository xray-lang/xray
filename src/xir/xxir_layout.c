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
#include "xxir_callable.h"
#include "../base/xmalloc.h"

XrXirStatus xr_xir_layout(XrXirType type, const XrXirTarget *target,
                        XrXirLayoutContext context, XrXirLayout *layout) {
    if (!layout)
        return XR_XIR_BAD_LAYOUT;
    *layout = (XrXirLayout) {0, 0};
    if (!target || target->architecture != XR_XIR_ARCH_X86_64 ||
        target->abi_version != XR_XIR_VALUE_ABI_VERSION ||
        context < XR_XIR_LAYOUT_STORAGE || context > XR_XIR_LAYOUT_FRAME ||
        (type != XR_XIR_UNIT && type != XR_XIR_BOOL && type != XR_XIR_I64 && !xr_xir_type_is_owned(type)) ||
        (type == XR_XIR_UNIT && context == XR_XIR_LAYOUT_PARAMETER))
        return XR_XIR_BAD_LAYOUT;
    if (context == XR_XIR_LAYOUT_PARAMETER || context == XR_XIR_LAYOUT_RESULT ||
        context == XR_XIR_LAYOUT_BOXED)
        *layout = (XrXirLayout) {16, 8};
    else if (type == XR_XIR_UNIT)
        *layout = (XrXirLayout) {0, 1};
    else if (type == XR_XIR_BOOL && context == XR_XIR_LAYOUT_STORAGE)
        *layout = (XrXirLayout) {1, 1};
    else
        *layout = (XrXirLayout) {8, 8};
    return XR_XIR_OK;
}

static bool layout_equal(XrXirLayout left, XrXirLayout right) {
    return left.size == right.size && left.alignment == right.alignment;
}

static XrXirType slot_type(const XrXirFunction *function, uint32_t slot) {
    return slot < function->parameter_count ? function->parameters[slot] :
        function->instructions[slot - function->parameter_count].type;
}

static bool subtract_bytes(uint64_t *remaining, uint64_t amount) {
    if (amount > *remaining)
        return false;
    *remaining -= amount;
    return true;
}

static XrXirStatus layout_budget(const XrXirModule *module, const XrXirBudget *budget) {
    uint64_t bytes = budget->metadata_bytes, work = budget->work;
    if (!subtract_bytes(&bytes, sizeof(XrXirArtifact)))
        return XR_XIR_BUDGET;
    XrXirStatus declaration_status = xr_xir_declarations_verify(module->declarations,
        module->function_count, &bytes, &work);
    if (declaration_status != XR_XIR_OK) return declaration_status;
    XrXirBudget signature_budget = *budget;
    signature_budget.metadata_bytes = bytes; signature_budget.work = work;
    XrXirStatus signature_status = xr_xir_callable_types_verify(module->callables, &signature_budget);
    if (signature_status != XR_XIR_OK) return signature_status;
    bytes = signature_budget.metadata_bytes; work = signature_budget.work;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
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
        XrXirLayout physical;
        XrXirType type = slot_type(function, slot);
        XrXirStatus status = xr_xir_layout(type, &artifact->target, XR_XIR_LAYOUT_FRAME, &physical);
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
        if (xr_xir_type_is_owned(type)) {
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
            status = xr_xir_layout(type, &artifact->target, XR_XIR_LAYOUT_PARAMETER, &physical);
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
            op->op == XR_XIR_CALL || op->op == XR_XIR_FUNCTION_REF || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_PRINT ? op->args[1] : 0;
        if (count > outgoing) outgoing = count;
    }
    uint64_t physical_bytes = bytes + (uint64_t) outgoing * sizeof(XrXirValue);
    if (physical_bytes > UINT32_MAX || physical_bytes > budget->frame_bytes) return XR_XIR_BUDGET;
    XrXirStatus status = xr_xir_layout(function->result, &artifact->target, XR_XIR_LAYOUT_RESULT, &result);
    if (status != XR_XIR_OK)
        return status;
    if (create) {
        layout->result = result;
        layout->frame_bytes = bytes;
        layout->owned_count = owned;
        layout->outgoing_count = outgoing;
    } else if (!layout_equal(layout->result, result) || layout->frame_bytes != bytes ||
               layout->owned_count != owned || layout->outgoing_count != outgoing) {
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
