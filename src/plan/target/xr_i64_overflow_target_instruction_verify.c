/* Independent proof of the frozen i64 overflow TargetPlan authority. */

#include "xr_i64_overflow_target_instruction.h"
#include "../semantic/xr_i64_overflow_predicate_semantics.h"
#include "../semantic/xr_semantic_ids.h"
#include "../semantic/xr_semantic_plan_internal.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>

static bool reject(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_TARGET_1003: %s", detail);
    return false;
}

static bool zero_id(XrStableId id) {
    static const XrStableId zero = {{0}};
    return xr_stable_id_equal(id, zero);
}

static bool exact_i64(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_INT && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 &&
           type->aggregate_align == 0 && type->scalar_rep == XR_NATIVE_I64 &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->enum_layout_id == 0 && type->enum_member_count == 0 &&
           type->enum_flags == 0 && type->reserved_enum == 0 && type->flags == 0;
}

static bool exact_bool(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_BOOL && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 &&
           type->aggregate_align == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->enum_layout_id == 0 && type->enum_member_count == 0 &&
           type->enum_flags == 0 && type->reserved_enum == 0 && type->flags == 0;
}

/* Deliberately independent from the builder-side identity routine. */
static bool reconstruct_identity(const XrTargetI64OverflowPredicateRecord *row,
                                 XrStableId *out) {
    char call[XR_STABLE_ID_BYTES * 2 + 1];
    char callsite[XR_STABLE_ID_BYTES * 2 + 1];
    char caller[XR_STABLE_ID_BYTES * 2 + 1];
    char builtin[XR_STABLE_ID_BYTES * 2 + 1];
    char key[384];
    XrFingerprint digest;
    if (!row || !out)
        return false;
    xr_stable_id_hex(row->program_call, call);
    xr_stable_id_hex(row->callsite, callsite);
    xr_stable_id_hex(row->caller_identity, caller);
    xr_stable_id_hex(row->builtin_identity, builtin);
    int written = snprintf(
        key, sizeof(key),
        "xray-target-i64-overflow-predicate-v1:call=%s:callsite=%s:caller=%s:builtin=%s:"
        "function=%u:operation=%u:program-row=%u:result=%u:receiver=%u:argument=%u:kind=%u",
        call, callsite, caller, builtin, row->function, row->semantic_operation,
        row->program_row, row->result_slot, row->receiver_slot, row->argument_slot,
        (unsigned) row->kind);
    return written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static bool verifier_kind(XrStableId builtin, XrI64OverflowPredicateKind *kind,
                          uint32_t *symbol) {
    for (uint32_t raw = XR_I64_OVERFLOW_PREDICATE_ADD;
         raw < XR_I64_OVERFLOW_PREDICATE_COUNT; raw++) {
        XrStableId expected = {{0}};
        uint32_t expected_symbol = raw == XR_I64_OVERFLOW_PREDICATE_ADD
                                       ? XR_I64_OVERFLOW_METHOD_SYMBOL_ADD
                                   : raw == XR_I64_OVERFLOW_PREDICATE_SUB
                                       ? XR_I64_OVERFLOW_METHOD_SYMBOL_SUB
                                       : XR_I64_OVERFLOW_METHOD_SYMBOL_MUL;
        if (xr_i64_overflow_predicate_builtin_identity(
                (XrI64OverflowPredicateKind) raw, &expected) &&
            xr_stable_id_equal(expected, builtin)) {
            if (kind)
                *kind = (XrI64OverflowPredicateKind) raw;
            if (symbol)
                *symbol = expected_symbol;
            return true;
        }
    }
    return false;
}

static bool slot_matches(const XrTargetSlotRecord *slot, uint32_t id,
                         uint32_t function, uint32_t semantic_value,
                         uint32_t semantic_operation, bool result) {
    return slot && slot->id == id && slot->function == function &&
           slot->semantic_value == semantic_value &&
           (!result || slot->semantic_operation == semantic_operation) &&
           slot->root_kind == XR_TARGET_ROOT_NONE &&
           slot->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
}

static bool row_matches_binding(const XrTargetPlan *plan,
                                const XrTargetI64OverflowPredicateRecord *row,
                                const XrSemanticProgramCallBinding *binding,
                                const XrSemanticProgramFunctionBinding *function_binding,
                                uint32_t target_function,
                                const XrTargetInstructionRecord *instruction) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    const XrSemanticOperationRecord *operation =
        semantic ? xr_semantic_plan_operation(semantic, binding->operation) : NULL;
    uint32_t operand_count = 0, slot_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    XrStableId expected_identity = {{0}};
    XrI64OverflowPredicateKind kind = XR_I64_OVERFLOW_PREDICATE_INVALID;
    uint32_t symbol = 0;
    if (!operation || !operands || !slots || !function_binding || !instruction ||
        !verifier_kind(binding->callee_program_function, &kind, &symbol) ||
        !reconstruct_identity(row, &expected_identity))
        return false;
    if (row->id != binding->program_row || row->function != target_function ||
        row->semantic_operation != binding->operation || row->program_row != binding->program_row ||
        row->kind != (uint8_t) kind || row->reserved[0] || row->reserved[1] || row->reserved[2] ||
        !xr_stable_id_equal(row->identity, expected_identity) ||
        !xr_stable_id_equal(row->program_call, binding->program_call) ||
        !xr_stable_id_equal(row->callsite, binding->callsite) ||
        !xr_stable_id_equal(row->caller_identity, function_binding->program_function) ||
        !xr_stable_id_equal(row->builtin_identity, binding->callee_program_function) ||
        !xr_stable_id_equal(binding->caller_program_function,
                            function_binding->program_function) ||
        binding->target_function != XR_SEMANTIC_INDEX_NONE ||
        binding->program_dependency != XR_SEMANTIC_INDEX_NONE ||
        !zero_id(binding->resolver_binding) || operation->function != function_binding->semantic_function ||
        operation->opcode != XI_CALL_METHOD || operation->operand_count != 2 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->semantic_immediate != ((int64_t) symbol << 1) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE || operation->allocation_key ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ || operation->parameter_ownership != XI_OWN_NONE ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_provenance != XR_SEM_RETURN_NONE || operation->return_complete != 0 ||
        !exact_bool(xr_semantic_plan_type(semantic, operation->result_type)) ||
        !exact_i64(xr_semantic_plan_type(semantic, operands[operation->operand_begin].type)) ||
        !exact_i64(xr_semantic_plan_type(semantic, operands[operation->operand_begin + 1u].type)) ||
        row->result_slot >= slot_count || row->receiver_slot >= slot_count ||
        row->argument_slot >= slot_count ||
        !slot_matches(&slots[row->result_slot], row->result_slot, target_function,
                      operation->result_value, binding->operation, true) ||
        !slot_matches(&slots[row->receiver_slot], row->receiver_slot, target_function,
                      operands[operation->operand_begin].value, 0, false) ||
        !slot_matches(&slots[row->argument_slot], row->argument_slot, target_function,
                      operands[operation->operand_begin + 1u].value, 0, false) ||
        instruction->opcode != XR_TARGET_INSTRUCTION_I64_OVERFLOW_PREDICATE ||
        instruction->immediate_bits != row->id ||
        instruction->function != row->function ||
        instruction->result_slot != row->result_slot || instruction->operand_count != 2 ||
        instruction->reserved != 0 || instruction->operand_slots[0] != row->receiver_slot ||
        instruction->operand_slots[1] != row->argument_slot)
        return false;
    return true;
}

XR_FUNC bool xr_i64_overflow_target_program_verify(const XrTargetPlan *plan,
                                                   char *error, size_t error_size) {
    const XrSemanticPlan *semantic = plan ? xr_target_plan_semantic_plan(plan) : NULL;
    const XrSemanticProgramProvenance *program =
        semantic ? xr_semantic_plan_program_provenance(semantic) : NULL;
    uint32_t row_count = 0, instruction_count = 0;
    const XrTargetI64OverflowPredicateRecord *rows =
        xr_target_plan_i64_overflow_predicates(plan, &row_count);
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(plan, &instruction_count);
    bool required = program &&
                    program->program_family == XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE;
    if (!required)
        return row_count == 0 || reject(error, error_size,
                                        "overflow rows lack exact program authority");
    if (!plan || !xr_target_plan_is_frozen(plan) || !rows || !instructions ||
        program->module_count != 1 || program->function_count != 1 || program->call_count == 0 ||
        row_count != program->call_count ||
        xr_semantic_plan_program_function_binding_count(semantic) != 1 ||
        xr_semantic_plan_program_call_binding_count(semantic) != row_count)
        return reject(error, error_size, "overflow TargetPlan provenance is incomplete");
    const XrSemanticProgramFunctionBinding *function_binding =
        xr_semantic_plan_program_function_binding(semantic, 0);
    if (!function_binding ||
        function_binding->flags != XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY)
        return reject(error, error_size, "overflow entry function binding is invalid");
    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions = xr_target_plan_functions(plan, &function_count);
    uint32_t target_function = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; functions && i < function_count; i++) {
        if (functions[i].semantic_function != function_binding->semantic_function)
            continue;
        if (target_function != XR_SEMANTIC_INDEX_NONE)
            return reject(error, error_size, "overflow target function join is ambiguous");
        target_function = i;
    }
    if (target_function == XR_SEMANTIC_INDEX_NONE ||
        functions[target_function].id != target_function)
        return reject(error, error_size, "overflow target function join is not unique");
    uint32_t overflow_count = 0;
    uint32_t previous_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < instruction_count; i++) {
        if (instructions[i].opcode != XR_TARGET_INSTRUCTION_I64_OVERFLOW_PREDICATE)
            continue;
        if (instructions[i].immediate_bits >= row_count)
            return reject(error, error_size,
                          "overflow instruction predicate index is out of range");
        uint32_t row_index = (uint32_t) instructions[i].immediate_bits;
        const XrTargetI64OverflowPredicateRecord *row = &rows[row_index];
        const XrSemanticProgramCallBinding *binding =
            xr_semantic_plan_program_call_for_operation(semantic, row->semantic_operation);
        if (!binding || binding->program_row != row_index || row->id != row_index ||
            (overflow_count != 0 && row->semantic_operation <= previous_operation) ||
            !row_matches_binding(plan, row, binding, function_binding,
                                 target_function, &instructions[i]))
            return reject(error, error_size, "overflow predicate row is not exact");
        previous_operation = row->semantic_operation;
        overflow_count++;
    }
    return overflow_count == row_count ||
           reject(error, error_size, "overflow instruction coverage is not exact");
}
