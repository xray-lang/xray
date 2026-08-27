/* Builder-side projection of the sealed i64 overflow predicate authority. */

#include "xr_i64_overflow_target_instruction.h"
#include "../semantic/xr_i64_overflow_predicate_semantics.h"
#include "../semantic/xr_semantic_ids.h"
#include "../semantic/xr_semantic_plan_internal.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

static bool stable_id_zero(XrStableId id) {
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

static bool row_identity(const XrTargetI64OverflowPredicateRecord *row, XrStableId *out) {
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
        call, callsite, caller, builtin, row->function, row->semantic_operation, row->program_row,
        row->result_slot, row->receiver_slot, row->argument_slot, (unsigned) row->kind);
    return written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

XR_FUNC bool xr_i64_overflow_target_predicate_project(
    const XrSemanticPlan *semantic, uint32_t operation_index, uint32_t target_function,
    uint32_t result_slot, uint32_t receiver_slot, uint32_t argument_slot,
    uint32_t row_id, XrTargetI64OverflowPredicateRecord *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    const XrSemanticProgramProvenance *program =
        semantic ? xr_semantic_plan_program_provenance(semantic) : NULL;
    const XrSemanticOperationRecord *operation =
        semantic ? xr_semantic_plan_operation(semantic, operation_index) : NULL;
    const XrSemanticProgramCallBinding *binding =
        semantic ? xr_semantic_plan_program_call_for_operation(semantic, operation_index) : NULL;
    const XrSemanticProgramFunctionBinding *function_binding =
        operation ? xr_semantic_plan_program_function_for_semantic_function(
                        semantic, operation->function)
                  : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    if (!out || !program || !operation || !binding || !function_binding || !operands ||
        program->program_family != XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE ||
        function_binding->semantic_function != operation->function ||
        target_function == XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(binding->caller_program_function,
                            function_binding->program_function) ||
        operation->opcode != XI_CALL_METHOD || operation->operand_count != 2 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->constant != XR_SEMANTIC_INDEX_NONE || operation->allocation_key ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ || operation->parameter_ownership != XI_OWN_NONE ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_provenance != XR_SEM_RETURN_NONE || operation->return_complete != 0 ||
        binding->operation != operation_index || binding->target_function != XR_SEMANTIC_INDEX_NONE ||
        binding->program_dependency != XR_SEMANTIC_INDEX_NONE ||
        !stable_id_zero(binding->resolver_binding))
        return false;

    XrI64OverflowPredicateKind kind = XR_I64_OVERFLOW_PREDICATE_INVALID;
    uint32_t method_symbol = 0;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    if (!xr_i64_overflow_predicate_kind_from_builtin_identity(
            binding->callee_program_function, &kind) ||
        !xr_i64_overflow_predicate_method_symbol(kind, &method_symbol) ||
        operation->semantic_immediate != ((int64_t) method_symbol << 1) ||
        !exact_bool(xr_semantic_plan_type(semantic, operation->result_type)) ||
        !exact_i64(xr_semantic_plan_type(semantic, receiver->type)) ||
        !exact_i64(xr_semantic_plan_type(semantic, argument->type)) ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != XR_PARAM_READ || receiver->access != XR_CALL_ARG_PLAIN ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->transfer_mode != XR_TRANSFER_SHARE ||
        argument->ownership_action != XR_SEM_OPERAND_BORROW ||
        argument->parameter_mode != XR_PARAM_READ || argument->access != XR_CALL_ARG_PLAIN ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;

    *out = (XrTargetI64OverflowPredicateRecord) {
        .program_call = binding->program_call,
        .callsite = binding->callsite,
        .caller_identity = function_binding->program_function,
        .builtin_identity = binding->callee_program_function,
        .id = row_id,
        .function = target_function,
        .semantic_operation = operation_index,
        .program_row = binding->program_row,
        .result_slot = result_slot,
        .receiver_slot = receiver_slot,
        .argument_slot = argument_slot,
        .kind = (uint8_t) kind,
    };
    return row_identity(out, &out->identity);
}
