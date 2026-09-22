/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_vm_provider.inc.c - Program-typed provider values in private VM storage
 */

static bool vm_pack_provider_value(XrVmContext *context, XrVmValue value, uint16_t type_id,
                                    XrProviderValuePack *pack) {
    if (pack->count >= XR_PROVIDER_VALUE_MAX_NODES ||
        !value_matches_type(context->code->program, value, type_id))
        return false;
    XrProviderValueNode *node = &pack->nodes[pack->count++];
    switch (type_id) {
        case XR_CORE_TYPE_VOID: node->token = XR_PROVIDER_TYPE_UNIT; return true;
        case XR_CORE_TYPE_BOOL:
            node->token = XR_PROVIDER_TYPE_BOOL; node->as.boolean = value.as.boolean; return true;
        case XR_CORE_TYPE_I64:
            node->token = XR_PROVIDER_TYPE_I64; node->as.i64 = value.as.i64; return true;
        case XR_CORE_TYPE_STRING:
            node->token = XR_PROVIDER_TYPE_BYTES;
            return vm_string_view(value, &node->as.bytes.data, &node->as.bytes.size);
        default: break;
    }
    const XrValidatedType *type = xr_validated_program_type(context->code->program, type_id);
    if (!type)
        return false;
    if (type->kind == XR_CORE_IR_TYPE_PROVIDER_RESOURCE) {
        const XrVmResourceValue *resource = value.as.resource;
        node->token = XR_PROVIDER_TYPE_RESOURCE;
        node->as.resource.id = type->resource_id;
        node->as.resource.owner = resource->owner;
        return true;
    }
    if (value.kind != XR_VM_VALUE_AGGREGATE || !value.as.aggregate)
        return false;
    const XrVmAggregateValue *aggregate = value.as.aggregate;
    const uint16_t *fields = type->field_types;
    uint32_t count = type->field_count;
    if (type->kind == XR_CORE_IR_TYPE_VARIANT) {
        if (type->variant_count != 2u || aggregate->variant_ordinal >= 2u)
            return false;
        node->token = XR_PROVIDER_TYPE_OPTIONAL;
        fields = type->variants[aggregate->variant_ordinal].payload_types;
        count = type->variants[aggregate->variant_ordinal].payload_count;
    } else if (type->kind == XR_CORE_IR_TYPE_AGGREGATE) {
        node->token = XR_PROVIDER_TYPE_TUPLE;
    } else {
        return false;
    }
    if (count != aggregate->field_count || count > UINT8_MAX)
        return false;
    node->child_count = (uint8_t) count;
    for (uint32_t field = 0u; field < count; ++field) {
        if (!vm_pack_provider_value(context, aggregate->fields[field], fields[field], pack))
            return false;
    }
    return true;
}

static bool vm_take_provider_value(XrVmContext *context, uint16_t type_id,
                                    XrProviderValuePack *pack, uint32_t *offset, XrVmValue *out) {
    if (*offset >= pack->count)
        return false;
    XrProviderValueNode *node = &pack->nodes[(*offset)++];
    if (type_id == XR_CORE_TYPE_VOID) {
        *out = void_value(); return node->token == XR_PROVIDER_TYPE_UNIT;
    }
    if (type_id == XR_CORE_TYPE_I64) {
        *out = (XrVmValue) {.kind = XR_VM_VALUE_I64, .as.i64 = node->as.i64};
        return node->token == XR_PROVIDER_TYPE_I64;
    }
    if (type_id == XR_CORE_TYPE_BOOL) {
        *out = (XrVmValue) {.kind = XR_VM_VALUE_BOOL, .as.boolean = node->as.boolean};
        return node->token == XR_PROVIDER_TYPE_BOOL;
    }
    const XrValidatedType *type = xr_validated_program_type(context->code->program, type_id);
    if (!type)
        return false;
    if (type->kind == XR_CORE_IR_TYPE_PROVIDER_RESOURCE) {
        if (node->token != XR_PROVIDER_TYPE_RESOURCE || !node->as.resource.owner ||
            context->storage->aggregate_cell_count >= context->code->options.max_value_cells)
            return false;
        XrVmResourceValue *value = xr_calloc(1u, sizeof(*value));
        if (!value)
            return false;
        value->type_id = type_id;
        value->owner = node->as.resource.owner;
        node->as.resource.owner = NULL;
        vm_value_cell_link(context->storage, &value->cell, XR_VM_VALUE_RESOURCE, 1u);
        *out = (XrVmValue) {.kind = XR_VM_VALUE_RESOURCE, .as.resource = value};
        return true;
    }
    const uint16_t *fields = type->field_types;
    uint32_t count = type->field_count;
    uint32_t ordinal = UINT32_MAX;
    if (type->kind == XR_CORE_IR_TYPE_VARIANT && node->token == XR_PROVIDER_TYPE_OPTIONAL) {
        ordinal = node->child_count ? 1u : 0u;
        fields = type->variants[ordinal].payload_types;
        count = type->variants[ordinal].payload_count;
    } else if (type->kind != XR_CORE_IR_TYPE_AGGREGATE || node->token != XR_PROVIDER_TYPE_TUPLE) {
        return false;
    }
    XrVmAggregateValue *aggregate = allocate_aggregate(context, type_id, ordinal, count);
    if (!aggregate)
        return false;
    for (uint32_t field = 0u; field < count; ++field) {
        if (!vm_take_provider_value(context, fields[field], pack, offset, &aggregate->fields[field]))
            return false;
    }
    *out = (XrVmValue) {.kind = XR_VM_VALUE_AGGREGATE, .as.aggregate = aggregate};
    return true;
}

static XrVmOutcome vm_provider_call(XrVmContext *context, const XrValidatedFunction *function,
                                    const XrVmInstructionView *instruction,
                                    const XrVmRuntimeValue *values, uint32_t operand_count) {
    XrProviderValuePack arguments = {0}, returned = {0};
    for (uint32_t index = 0u; index < operand_count; ++index) {
        uint32_t value = instruction->operands[index];
        if (!vm_pack_provider_value(context, values[value].as.value, function->value_types[value], &arguments))
            return vm_trap(XR_VM_TRAP_PROVIDER_CALL_FAILED, context);
    }
    XrExecutionProviderCallResult status = xr_execution_lease_provider_call_typed(
        context->lease, instruction->immediate.provider_operation.requirement_index,
        instruction->immediate.provider_operation.operation_index, &arguments, &returned);
    if (status != XR_EXECUTION_PROVIDER_CALL_OK)
        return status == XR_EXECUTION_PROVIDER_CALL_OUT_OF_MEMORY
                   ? vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context)
                   : vm_trap(XR_VM_TRAP_PROVIDER_CALL_FAILED, context);
    XrVmOutcome outcome = vm_outcome(XR_VM_OUTCOME_RETURN, context);
    uint32_t offset = 0u;
    if (!vm_take_provider_value(context, instruction->result_type_id, &returned, &offset, &outcome.value) ||
        offset != returned.count)
        outcome = vm_outcome(XR_VM_OUTCOME_RESOURCE_LIMIT, context);
    xr_execution_provider_result_dispose(&returned);
    return outcome;
}
