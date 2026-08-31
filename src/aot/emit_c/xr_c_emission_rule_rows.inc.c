/*
 * Structural projection for generated C emission rules.
 *
 * This locator does not decide which recipe is legal. It only joins immutable
 * SemanticPlan and TargetPlan rows by numeric identity. The generated builder
 * and verifier independently decide whether the resulting facts match a rule.
 */

typedef struct XrCEmissionRuleLocation {
    XrCEmissionRuleFacts facts;
    uint32_t receiver_value;
    uint32_t element_value;
} XrCEmissionRuleLocation;

static XrCEmissionRuleMatch xr_c_emission_rule_locate(
    const XrTargetPlan *target_plan, const XrTargetValueRepRecord *binding,
    XrCEmissionRuleLocation *out) {
    if (!target_plan || !binding || !out)
        return XR_C_EMISSION_RULE_MALFORMED;
    memset(out, 0, sizeof(*out));
    out->receiver_value = UINT32_MAX;
    out->element_value = UINT32_MAX;

    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    if (!semantic)
        return XR_C_EMISSION_RULE_MALFORMED;
    const XrSemanticOperationRecord *operation = NULL;
    uint32_t operation_index = UINT32_MAX;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != binding->semantic_value)
            continue;
        if (operation)
            return XR_C_EMISSION_RULE_MALFORMED;
        operation = candidate;
        operation_index = i;
    }
    if (!operation || operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & INT64_C(1)) != 0 ||
        (uint64_t) operation->semantic_immediate > ((uint64_t) UINT16_MAX << 1u))
        return XR_C_EMISSION_RULE_NOT_APPLICABLE;

    XrCEmissionRuleFacts *facts = &out->facts;
    facts->member = (uint16_t) ((uint64_t) operation->semantic_immediate >> 1u);
    facts->opcode = operation->opcode;
    facts->intrinsic = operation->intrinsic_kind;
    facts->operand_count = operation->operand_count;
    facts->operation_result_bound = operation->result_value == binding->semantic_value;
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(semantic, operation->result_type);
    facts->result_kind = result_type ? (uint16_t) result_type->kind : UINT16_MAX;

    uint32_t operand_count = 0, metadata_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    bool operand_range = operands && operation->operand_begin <= operand_count &&
                         operation->operand_count <= operand_count - operation->operand_begin;
    const XrSemanticOperandRecord *receiver = operand_range && operation->operand_count > 0
                                                  ? &operands[operation->operand_begin]
                                                  : NULL;
    const XrSemanticOperandRecord *element = operand_range && operation->operand_count > 1
                                                 ? receiver + 1
                                                 : NULL;
    if (receiver)
        out->receiver_value = receiver->value;
    if (element)
        out->element_value = element->value;

    const XrArrayMemberShape *shape =
        metadata && operation->metadata_count == 1 && operation->metadata_begin < metadata_count
            ? xr_array_member_shape(metadata[operation->metadata_begin], operation->operand_count)
            : NULL;
    if (shape) {
        facts->element_access = shape->element_access;
        facts->reference_action = shape->reference_action;
        facts->reference_drop = shape->reference_drop;
    }

    const XrSemanticTypeRecord *receiver_type =
        receiver ? xr_semantic_plan_type(semantic, receiver->type) : NULL;
    const XrSemanticTypeRecord *element_type = NULL;
    if (receiver_type && children && receiver_type->child_count == 1 &&
        receiver_type->child_begin < child_count)
        element_type =
            xr_semantic_plan_type(semantic, children[receiver_type->child_begin]);
    facts->element_managed_reference =
        element_type &&
        xr_semantic_array_member_owned_reference_type_is_exact(semantic, element_type);

    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(target_plan, &layout_count);
    const XrTargetLayoutRecord *layout = NULL;
    for (uint32_t i = 0; layouts && receiver && i < layout_count; i++) {
        if (layouts[i].semantic_type != receiver->type)
            continue;
        if (layout) {
            layout = NULL;
            break;
        }
        layout = &layouts[i];
    }
    if (layout) {
        facts->layout_kind = layout->kind;
        facts->layout_storage = layout->array_element_storage;
    }

    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        if (calls[i].semantic_operation != operation_index)
            continue;
        if (call) {
            call = NULL;
            break;
        }
        call = &calls[i];
    }
    if (!call)
        return XR_C_EMISSION_RULE_EXACT;
    facts->call_convention = call->calling_convention;
    facts->target_kind = call->target_kind;
    facts->call_storage = call->array_element_storage;
    facts->call_result_bound = call->result_value == binding->semantic_value;

    uint32_t argument_count = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target_plan, &argument_count);
    bool argument_range = arguments && call->argument_begin <= argument_count &&
                          call->argument_count <= argument_count - call->argument_begin;
    if (!argument_range || call->argument_count != 2 || !receiver || !element)
        return XR_C_EMISSION_RULE_EXACT;
    const XrTargetCallArgumentRecord *receiver_argument = &arguments[call->argument_begin];
    const XrTargetCallArgumentRecord *element_argument = receiver_argument + 1;
    const XrTargetCallArgumentRecord *ordered[2] = {receiver_argument, element_argument};
    for (uint16_t i = 0; i < 2; i++) {
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, ordered[i]->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, ordered[i]->memory_rep);
        facts->argument_ownership[i] = ordered[i]->ownership;
        facts->argument_storage[i] = ordered[i]->array_element_storage;
        facts->caller_register_kind[i] = register_rep ? register_rep->kind : UINT16_MAX;
        facts->caller_memory_kind[i] = memory_rep ? memory_rep->kind : UINT16_MAX;
    }
    facts->arguments_structurally_exact =
        receiver_argument->call == call->id && element_argument->call == call->id &&
        receiver_argument->ordinal == 0 && element_argument->ordinal == 1 &&
        receiver_argument->semantic_operand == operation->operand_begin &&
        element_argument->semantic_operand == operation->operand_begin + 1u &&
        receiver_argument->semantic_value == receiver->value &&
        element_argument->semantic_value == element->value;
    return XR_C_EMISSION_RULE_EXACT;
}
