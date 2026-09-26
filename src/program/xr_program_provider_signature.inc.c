/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_provider_signature.inc.c - Exact logical provider type admission
 *
 * KEY CONCEPT:
 *   Program checks declarations, not an executor's list of host call shapes.
 *   Each recursive step consumes bounded logical bytes; resources end at their
 *   stable identity and never expose their physical payload representation.
 */

static bool provider_type_matches(const XrValidatedProgram *program, uint16_t type_id,
                                  XrProviderLogicalTypeView logical, size_t *offset) {
    if (*offset >= logical.size)
        return false;
    uint8_t token = logical.bytes[(*offset)++];
    switch (token) {
        case XR_PROVIDER_TYPE_UNIT: return type_id == XR_CORE_TYPE_VOID;
        case XR_PROVIDER_TYPE_BOOL: return type_id == XR_CORE_TYPE_BOOL;
        case XR_PROVIDER_TYPE_I64: return type_id == XR_CORE_TYPE_I64;
        case XR_PROVIDER_TYPE_BYTES: return type_id == XR_CORE_TYPE_STRING;
        default: break;
    }
    const XrValidatedType *type = xr_validated_program_type(program, type_id);
    if (!type || type->nominal_kind != XR_CORE_IR_NOMINAL_NONE)
        return false;
    if (token == XR_PROVIDER_TYPE_RESOURCE) {
        if (type->kind != XR_CORE_IR_TYPE_PROVIDER_RESOURCE ||
            logical.size - *offset < XR_STABLE_ID_BYTES ||
            memcmp(type->resource_id.bytes, logical.bytes + *offset, XR_STABLE_ID_BYTES) != 0)
            return false;
        *offset += XR_STABLE_ID_BYTES;
        return true;
    }
    if (token == XR_PROVIDER_TYPE_OPTIONAL) {
        return type->kind == XR_CORE_IR_TYPE_VARIANT && type->variant_count == 2u &&
               type->variants[0].payload_count == 0u &&
               type->variants[1].payload_count == 1u && type->variants[1].payload_types &&
               provider_type_matches(program, type->variants[1].payload_types[0], logical, offset);
    }
    if (token != XR_PROVIDER_TYPE_TUPLE || type->kind != XR_CORE_IR_TYPE_AGGREGATE ||
        *offset >= logical.size)
        return false;
    uint8_t arity = logical.bytes[(*offset)++];
    if (arity == 0u || type->field_count != arity || !type->field_types)
        return false;
    for (uint32_t field = 0u; field < type->field_count; ++field) {
        if (!provider_type_matches(program, type->field_types[field], logical, offset))
            return false;
    }
    return true;
}

bool xr_validated_program_provider_type_matches(const XrValidatedProgram *program,
                                                uint16_t type_id,
                                                XrProviderLogicalTypeView logical) {
    if (!program || !logical.bytes || logical.size == 0u ||
        logical.size > XR_PROVIDER_LOGICAL_MAX_TYPE_BYTES)
        return false;
    size_t offset = 0u;
    return provider_type_matches(program, type_id, logical, &offset) && offset == logical.size;
}

static const XrProviderLogicalContract *provider_instruction_contract(
    const XrValidatedProgram *program, const XrValidatedInstruction *instruction) {
    if (!instruction || instruction->immediate_kind != XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION ||
        instruction->immediate.provider_operation.requirement_index >= program->provider_requirement_count)
        return NULL;
    const XrValidatedProviderRequirement *requirement =
        &program->provider_requirements[instruction->immediate.provider_operation.requirement_index];
    uint32_t operation = instruction->immediate.provider_operation.operation_index;
    return operation < requirement->operation_count
               ? &requirement->operations[operation].logical_contract : NULL;
}

static bool provider_synchronous_policy(const XrProviderLogicalContract *logical) {
    bool mutable_borrow = false;
    if (logical) {
        for (uint8_t i = 0u; i < logical->parameter_count; ++i)
            mutable_borrow |= logical->parameter_modes[i] == XR_PROVIDER_MODE_REF;
    }
    if (!logical ||
        (logical->effects & (XR_PROVIDER_EFFECT_MAY_ERROR | XR_PROVIDER_EFFECT_MAY_PANIC |
                             XR_PROVIDER_EFFECT_MAY_SUSPEND)) != 0u ||
        logical->error_owner != XR_PROVIDER_OWNER_TRIVIAL ||
        logical->threads != XR_PROVIDER_THREADS_ANY ||
        logical->reentry != (mutable_borrow ? XR_PROVIDER_REENTRY_FORBIDDEN : XR_PROVIDER_REENTRY_ALLOWED) ||
        logical->callbacks != XR_PROVIDER_CALLBACK_NONE ||
        logical->refusal != XR_PROVIDER_REFUSAL_TRAP)
        return false;
    XrProviderLogicalTypeView error;
    return xr_provider_logical_contract_type(logical, logical->parameter_count + 1u, &error) &&
           error.size == 1u && error.bytes[0] == XR_PROVIDER_TYPE_UNIT;
}

static bool verify_provider_signature(VerifyContext *context,
                                      const XrValidatedFunction *function,
                                      const XrValidatedInstruction *instruction,
                                      uint32_t data_count) {
    const XrValidatedProgram *program = context->program;
    const XrProviderLogicalContract *logical = provider_instruction_contract(program, instruction);
    if (!provider_synchronous_policy(logical) || logical->parameter_count != data_count ||
        instruction->result_category != XR_CORE_IR_VALUE ||
        instruction->result_ownership != ownership_for_type(program, instruction->result_type_id) ||
        ((instruction->result_type_id == XR_CORE_TYPE_VOID) !=
         (instruction->result_id == XR_PROGRAM_LOCATION_NONE)))
        return false;
    XrProviderLogicalTypeView result;
    bool owned = xr_validated_program_type_ownership(program, instruction->result_type_id) ==
                 XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    if (logical->result_owner != (owned ? XR_PROVIDER_OWNER_OWNED : XR_PROVIDER_OWNER_TRIVIAL) ||
        !xr_provider_logical_contract_type(logical, logical->parameter_count, &result) ||
        !xr_validated_program_provider_type_matches(program, instruction->result_type_id, result))
        return false;
    for (uint32_t parameter = 0u; parameter < data_count; ++parameter) {
        uint32_t value = instruction->operands[parameter];
        uint16_t type_id = function->value_types[value];
        XrProviderLogicalTypeView type;
        bool affine = xr_validated_program_type_ownership(program, type_id) ==
                      XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
        if (logical->parameter_modes[parameter] == XR_PROVIDER_MODE_REF) {
            const XrValidatedType *array = xr_validated_program_type(program, type_id);
            uint32_t root = scoped_place_root(function, value, function->value_blocks[value]);
            if (!array || array->kind != XR_CORE_IR_TYPE_ARRAY || array->array_element_type != XR_CORE_TYPE_U8 ||
                !affine || function->value_categories[value] != XR_CORE_IR_PLACE ||
                function->value_ownerships[value] != XR_CORE_IR_NON_OWNER ||
                logical->parameter_owners[parameter] != XR_PROVIDER_OWNER_BORROWED ||
                logical->reentry != XR_PROVIDER_REENTRY_FORBIDDEN ||
                instruction->result_type_id != XR_CORE_TYPE_VOID ||
                !xr_provider_logical_contract_type(logical, (uint8_t)parameter, &type) ||
                type.size != 1u || type.bytes[0] != XR_PROVIDER_TYPE_U8_ARRAY ||
                root == XR_PROGRAM_LOCATION_NONE ||
                (function->value_categories[root] == XR_CORE_IR_VALUE &&
                 function->value_ownerships[root] != XR_CORE_IR_OWNER))
                return false;
            /* Distinct owned value roots prove disjoint storage. Reference and
             * module roots need an address proof before simultaneous loans. */
            for (uint32_t previous = 0u; previous < parameter; ++previous) {
                if (logical->parameter_modes[previous] != XR_PROVIDER_MODE_REF)
                    continue;
                uint32_t other = instruction->operands[previous];
                uint32_t other_root = scoped_place_root(function, other, function->value_blocks[other]);
                if (other_root == XR_PROGRAM_LOCATION_NONE || other_root == root ||
                    function->value_categories[root] != XR_CORE_IR_VALUE ||
                    function->value_categories[other_root] != XR_CORE_IR_VALUE ||
                    function->value_ownerships[other_root] != XR_CORE_IR_OWNER)
                    return false;
                uint32_t identity = scoped_reference_identity(context, function, root);
                uint32_t other_identity = scoped_reference_identity(context, function, other_root);
                if (identity == XR_PROGRAM_LOCATION_NONE || other_identity == XR_PROGRAM_LOCATION_NONE ||
                    identity == other_identity)
                    return false;
            }
            continue;
        }
        /* Resource handles and byte views remain borrowed through synchronous
         * host calls. Managed transfer and OUT are not inferred from physical
         * pointer arguments. */
        if (logical->parameter_modes[parameter] != XR_PROVIDER_MODE_IN ||
            logical->parameter_owners[parameter] !=
                (affine ? XR_PROVIDER_OWNER_BORROWED : XR_PROVIDER_OWNER_TRIVIAL) ||
            function->value_categories[value] != XR_CORE_IR_VALUE ||
            (!affine && function->value_ownerships[value] != XR_CORE_IR_NON_OWNER) ||
            !xr_provider_logical_contract_type(logical, (uint8_t) parameter, &type) ||
            !xr_validated_program_provider_type_matches(program, type_id, type))
            return false;
    }
    return true;
}

static bool output_provider_signature(const XrProviderLogicalContract *logical) {
    static const uint8_t types[] = {XR_PROVIDER_TYPE_BYTES, XR_PROVIDER_TYPE_UNIT, XR_PROVIDER_TYPE_UNIT};
    return provider_synchronous_policy(logical) && logical->result_owner == XR_PROVIDER_OWNER_TRIVIAL &&
           logical->parameter_count == 1u && logical->parameter_modes[0] == XR_PROVIDER_MODE_IN &&
           logical->parameter_owners[0] == XR_PROVIDER_OWNER_BORROWED &&
           logical->type_byte_count == sizeof(types) && memcmp(logical->types, types, sizeof(types)) == 0;
}
