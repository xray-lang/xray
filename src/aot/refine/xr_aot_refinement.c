/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_refinement.c - Immutable TargetPlan-native AOT refinement protocol
 */

#include "xr_aot_refinement.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../plan/semantic/xr_semantic_type_admission_shape.h"
#include "../../plan/target/xr_target_verify.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../runtime/value/xtype.h"
#include "../../shared/xr_param_mode.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XrAotTargetIndex {
    uint32_t semantic_value_count;
    uint32_t *operation_by_value;
    uint32_t *parameter_by_value;
} XrAotTargetIndex;

struct XrAotRefinementBuilder {
    XrAotBaselineRef baseline;
    XrAotInvariantState initial_state;
    XrAotInvariantState current_state;
    XrAotTransformationRecord *records;
    uint32_t record_count;
    uint32_t record_capacity;
    XrAotTargetIndex target_index;
    bool frozen;
};

struct XrAotRefinementPlan {
    XrAotRefinementPlanView view;
    XrAotTransformationRecord *records;
};

static void clear_diag(XrAotRefinementDiagnostic *diag) {
    if (diag)
        memset(diag, 0, sizeof(*diag));
}

static void write_diag(XrAotRefinementDiagnostic *diag, uint32_t issue,
                       uint32_t record_index, uint32_t pass_id,
                       uint32_t target_call_index) {
    if (!diag)
        return;
    *diag = (XrAotRefinementDiagnostic) {
        .issue = issue,
        .record_index = record_index,
        .pass_id = pass_id,
        .target_call_index = target_call_index,
    };
}

static bool fail_diag(XrAotRefinementDiagnostic *diag, uint32_t issue,
                      uint32_t record_index, uint32_t pass_id,
                      uint32_t target_call_index) {
    write_diag(diag, issue, record_index, pass_id, target_call_index);
    return false;
}

static bool fingerprint_is_zero(XrFingerprint fingerprint) {
    static const XrFingerprint zero = {{0}};
    return xr_fingerprint_equal(fingerprint, zero);
}

static bool baseline_valid(const XrAotBaselineRef *baseline) {
    return baseline && baseline->completed_family_mask != 0 &&
           !fingerprint_is_zero(baseline->semantic_fingerprint) &&
           !fingerprint_is_zero(baseline->target_plan_fingerprint) &&
           !fingerprint_is_zero(baseline->target_profile_fingerprint);
}

static bool baseline_equal(const XrAotBaselineRef *left,
                           const XrAotBaselineRef *right) {
    return baseline_valid(left) && baseline_valid(right) &&
           left->completed_family_mask == right->completed_family_mask &&
           xr_fingerprint_equal(left->semantic_fingerprint,
                                right->semantic_fingerprint) &&
           xr_fingerprint_equal(left->target_plan_fingerprint,
                                right->target_plan_fingerprint) &&
           xr_fingerprint_equal(left->target_profile_fingerprint,
                                right->target_profile_fingerprint);
}

static bool state_valid(const XrAotInvariantState *state) {
    if (!state || (state->available & ~XR_AOT_INV_ALL) != 0)
        return false;
    for (uint32_t i = 0; i < XR_AOT_INV_COUNT; i++) {
        if (state->generation[i] == 0)
            return false;
    }
    return true;
}

static bool state_equal(const XrAotInvariantState *left,
                        const XrAotInvariantState *right) {
    return left && right && left->available == right->available &&
           memcmp(left->generation, right->generation,
                  sizeof(left->generation)) == 0;
}

static bool protocol_valid(const XrAotPassProtocol *protocol) {
    XrAotInvariantMask declared;
    if (!protocol ||
        protocol->schema_version != XR_AOT_REFINEMENT_SCHEMA_VERSION ||
        protocol->pass_id == 0 || protocol->transform_kind == 0 ||
        protocol->transform_kind >= XR_AOT_TRANSFORM_COUNT)
        return false;
    declared = protocol->requires | protocol->produces |
               protocol->invalidates | protocol->preserves;
    if ((declared & ~XR_AOT_INV_ALL) != 0 || protocol->requires == 0 ||
        (protocol->invalidates & protocol->preserves) != 0)
        return false;
    if ((protocol->invalidates | protocol->preserves) != XR_AOT_INV_ALL)
        return false;
    XrAotPassProtocol expected =
        protocol->transform_kind == XR_AOT_TRANSFORM_DIRECT_CALL
            ? xr_aot_refinement_direct_call_protocol(protocol->pass_id)
            : xr_aot_refinement_representation_protocol(protocol->pass_id);
    return protocol->requires == expected.requires &&
           protocol->produces == expected.produces &&
           protocol->invalidates == expected.invalidates &&
           protocol->preserves == expected.preserves;
}

static void target_index_dispose(XrAotTargetIndex *index) {
    if (!index)
        return;
    xr_free(index->operation_by_value);
    xr_free(index->parameter_by_value);
    memset(index, 0, sizeof(*index));
}

static bool target_index_init(const XrTargetPlan *target_plan,
                              XrAotTargetIndex *index) {
    if (!target_plan || !index)
        return false;
    memset(index, 0, sizeof(*index));
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    if (!semantic)
        return false;
    uint32_t function_count =
        (uint32_t) xr_semantic_plan_function_count(semantic);
    uint64_t value_count = 0;
    for (uint32_t i = 0; i < function_count; i++) {
        const XrSemanticFunctionRecord *function =
            xr_semantic_plan_function(semantic, i);
        if (function && (uint64_t) function->value_begin +
                                function->value_count >
                            value_count)
            value_count = (uint64_t) function->value_begin +
                          function->value_count;
    }
    if (value_count > XR_AOT_REFINEMENT_MAX_RECORDS)
        return false;
    index->semantic_value_count = (uint32_t) value_count;
    size_t allocation_count = index->semantic_value_count
                                  ? index->semantic_value_count
                                  : 1u;
    index->operation_by_value =
        (uint32_t *) xr_malloc(allocation_count * sizeof(uint32_t));
    index->parameter_by_value =
        (uint32_t *) xr_malloc(allocation_count * sizeof(uint32_t));
    if (!index->operation_by_value || !index->parameter_by_value) {
        target_index_dispose(index);
        return false;
    }
    for (uint32_t i = 0; i < index->semantic_value_count; i++) {
        index->operation_by_value[i] = XR_SEMANTIC_INDEX_NONE;
        index->parameter_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    }
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(semantic);
    if (operation_count > XR_AOT_REFINEMENT_MAX_RECORDS)
        goto invalid;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->result_value >=
                              index->semantic_value_count ||
            index->operation_by_value[operation->result_value] !=
                XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        index->operation_by_value[operation->result_value] = i;
    }
    uint32_t parameter_count =
        (uint32_t) xr_semantic_plan_parameter_count(semantic);
    if (parameter_count > XR_AOT_REFINEMENT_MAX_RECORDS)
        goto invalid;
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, i);
        if (!parameter || parameter->value >= index->semantic_value_count ||
            index->parameter_by_value[parameter->value] !=
                XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        index->parameter_by_value[parameter->value] = i;
    }
    return true;
invalid:
    target_index_dispose(index);
    return false;
}

static void hash_u32(XrSHA256Context *ctx, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < 4; i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < 8; i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void machine_rep_fingerprint(const XrTargetMachineRepRecord *machine,
                                    XrFingerprint *out) {
    static const uint8_t domain[] = "xray-target-machine-rep-row-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    hash_u32(&ctx, machine->id);
    hash_u32(&ctx, machine->kind);
    hash_u32(&ctx, machine->register_bits);
    hash_u32(&ctx, machine->memory_size);
    hash_u32(&ctx, machine->memory_align);
    hash_u32(&ctx, machine->signedness);
    hash_u32(&ctx, machine->root_kind);
    hash_u32(&ctx, machine->ownership);
    hash_u32(&ctx, machine->null_encoding);
    hash_u32(&ctx, machine->detail);
    hash_u32(&ctx, machine->lane_count);
    for (uint32_t i = 0; i < 4; i++)
        hash_u64(&ctx, machine->legal_conversion_mask[i]);
    xr_sha256_final(&ctx, out->bytes);
}

static void representation_record_fingerprint(
    const XrAotBaselineRef *baseline,
    const XrAotRepresentationAdapterRecord *record,
    XrFingerprint *out) {
    static const uint8_t domain[] = "xray-aot-representation-adapter-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    xr_sha256_update(&ctx, baseline->semantic_fingerprint.bytes,
                     sizeof(baseline->semantic_fingerprint.bytes));
    xr_sha256_update(&ctx, baseline->target_plan_fingerprint.bytes,
                     sizeof(baseline->target_plan_fingerprint.bytes));
    xr_sha256_update(&ctx, baseline->target_profile_fingerprint.bytes,
                     sizeof(baseline->target_profile_fingerprint.bytes));
    hash_u32(&ctx, record->source_function);
    hash_u32(&ctx, record->source_value);
    hash_u32(&ctx, record->source_operation);
    hash_u32(&ctx, record->source_type);
    hash_u32(&ctx, record->source_kind);
    hash_u32(&ctx, record->reserved);
    hash_u32(&ctx, record->use_operation);
    hash_u32(&ctx, record->use_block);
    hash_u32(&ctx, record->use_operand);
    hash_u32(&ctx, record->use_kind);
    hash_u32(&ctx, record->adapter_kind);
    hash_u32(&ctx, record->recipe);
    hash_u32(&ctx, record->input_rep_kind);
    hash_u32(&ctx, record->output_rep_kind);
    hash_u32(&ctx, record->target_register_rep);
    hash_u32(&ctx, record->target_memory_rep);
    hash_u32(&ctx, record->target_slot);
    hash_u32(&ctx, record->layout);
    hash_u32(&ctx, record->source_auxiliary_kind);
    hash_u32(&ctx, record->source_flags);
    hash_u32(&ctx, record->use_auxiliary_kind);
    hash_u32(&ctx, record->use_flags);
    hash_u64(&ctx, (uint64_t) record->source_semantic_immediate);
    hash_u64(&ctx, (uint64_t) record->use_semantic_immediate);
    xr_sha256_update(&ctx, record->source_operation_id.bytes,
                     sizeof(record->source_operation_id.bytes));
    xr_sha256_update(&ctx, record->source_type_id.bytes,
                     sizeof(record->source_type_id.bytes));
    xr_sha256_update(&ctx, record->use_operation_id.bytes,
                     sizeof(record->use_operation_id.bytes));
    xr_sha256_update(&ctx, record->policy_fingerprint.bytes,
                     sizeof(record->policy_fingerprint.bytes));
    xr_sha256_update(&ctx, record->machine_rep_fingerprint.bytes,
                     sizeof(record->machine_rep_fingerprint.bytes));
    xr_sha256_update(&ctx, record->layout_fingerprint.bytes,
                     sizeof(record->layout_fingerprint.bytes));
    xr_sha256_final(&ctx, out->bytes);
}

static uint16_t representation_recipe(uint16_t adapter, uint16_t machine) {
    bool box = adapter == XR_AOT_REP_ADAPTER_BOX;
    switch ((XrMachineRepKind) machine) {
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
            return box ? XR_AOT_REP_RECIPE_BOX_FLOAT
                       : XR_AOT_REP_RECIPE_UNBOX_FLOAT;
        case XR_MACHINE_REP_OBJECT_REF:
        case XR_MACHINE_REP_RAW_PTR:
            return box ? XR_AOT_REP_RECIPE_BOX_REFERENCE
                       : XR_AOT_REP_RECIPE_UNBOX_REFERENCE;
        case XR_MACHINE_REP_I1:
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_ENUM_ORDINAL:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
        case XR_MACHINE_REP_RUNE:
            return box ? XR_AOT_REP_RECIPE_BOX_INTEGER
                       : XR_AOT_REP_RECIPE_UNBOX_INTEGER;
        default: return XR_AOT_REP_RECIPE_NONE;
    }
}

static uint32_t derive_representation_record(
    const XrAotBaselineRef *baseline, const XrTargetPlan *target_plan,
    const XrAotTargetIndex *index,
    const XrAotRepresentationAdapterRequest *request,
    XrAotRepresentationAdapterRecord *out) {
    if (!baseline || !target_plan || !index || !request || !out ||
        request->adapter_kind < XR_AOT_REP_ADAPTER_BOX ||
        request->adapter_kind >= XR_AOT_REP_ADAPTER_COUNT ||
        request->use_kind < XR_AOT_REP_USE_OPERATION ||
        request->use_kind > XR_AOT_REP_USE_BLOCK_CONTROL ||
        request->input_rep_kind >= XR_MACHINE_REP_COUNT ||
        request->output_rep_kind >= XR_MACHINE_REP_COUNT ||
        fingerprint_is_zero(request->policy_fingerprint))
        return XR_AOT_REFINEMENT_REPRESENTATION;
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    if (!semantic)
        return XR_AOT_REFINEMENT_SOURCE_IDENTITY;

    uint32_t source_operation = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_type_index = XR_SEMANTIC_INDEX_NONE;
    uint16_t source_kind = 0;
    XrStableId source_id = {{0}};
    uint8_t source_auxiliary_kind = 0;
    uint8_t source_flags = 0;
    int64_t source_immediate = 0;
    if (request->source_value >= index->semantic_value_count)
        return XR_AOT_REFINEMENT_SOURCE_IDENTITY;
    uint32_t parameter_index =
        index->parameter_by_value[request->source_value];
    if (parameter_index != XR_SEMANTIC_INDEX_NONE) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, parameter_index);
        if (!parameter || parameter->value != request->source_value)
            return XR_AOT_REFINEMENT_SOURCE_IDENTITY;
        source_kind = XR_AOT_REP_SOURCE_PARAMETER;
        source_function = parameter->function;
        source_type_index = parameter->type;
        source_id = parameter->id;
        source_flags = parameter->flags;
        source_immediate = parameter->ordinal;
    }
    uint32_t operation_index =
        index->operation_by_value[request->source_value];
    if (operation_index != XR_SEMANTIC_INDEX_NONE) {
        const XrSemanticOperationRecord *candidate =
            xr_semantic_plan_operation(semantic, operation_index);
        if (!candidate || candidate->result_value != request->source_value)
            return XR_AOT_REFINEMENT_SOURCE_IDENTITY;
        source_operation = operation_index;
        if (source_kind == XR_AOT_REP_SOURCE_PARAMETER) {
            if (candidate->function != source_function ||
                candidate->result_type != source_type_index ||
                candidate->semantic_immediate != source_immediate)
                return XR_AOT_REFINEMENT_SOURCE_IDENTITY;
        } else {
            source_kind = XR_AOT_REP_SOURCE_OPERATION;
            source_function = candidate->function;
            source_type_index = candidate->result_type;
            source_id = candidate->id;
            source_auxiliary_kind = candidate->auxiliary_kind;
            source_flags = candidate->flags;
            source_immediate = candidate->semantic_immediate;
        }
    }
    if (source_kind == 0)
        return XR_AOT_REFINEMENT_SOURCE_IDENTITY;
    const XrSemanticTypeRecord *source_type =
        xr_semantic_plan_type(semantic, source_type_index);
    if (!source_type)
        return XR_AOT_REFINEMENT_SOURCE_TYPE;

    XrStableId use_id = {{0}};
    uint8_t use_auxiliary_kind = 0;
    uint8_t use_flags = 0;
    int64_t use_immediate = 0;
    if (request->use_kind == XR_AOT_REP_USE_OPERATION) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(semantic, request->use_operation);
        uint32_t operand_count = 0;
        const XrSemanticOperandRecord *operands =
            xr_semantic_plan_operands(semantic, &operand_count);
        if (!use || request->use_block != use->block ||
            request->use_operand >= use->operand_count ||
            use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin ||
            operands[use->operand_begin + request->use_operand].value !=
                request->source_value)
            return XR_AOT_REFINEMENT_USE_SITE;
        use_id = use->id;
        use_auxiliary_kind = use->auxiliary_kind;
        use_flags = use->flags;
        use_immediate = use->semantic_immediate;
    } else {
        const XrSemanticBlockRecord *block =
            xr_semantic_plan_block(semantic, request->use_block);
        if (!block || request->use_operation != XR_SEMANTIC_INDEX_NONE ||
            request->use_operand != 0 ||
            block->control_value != request->source_value)
            return XR_AOT_REFINEMENT_USE_SITE;
        use_id = block->id;
        use_flags = (uint8_t) block->kind;
    }

    *out = (XrAotRepresentationAdapterRecord) {
        .source_function = source_function,
        .source_value = request->source_value,
        .source_operation = source_operation,
        .source_type = source_type_index,
        .source_kind = source_kind,
        .use_operation = request->use_operation,
        .use_block = request->use_block,
        .use_operand = request->use_operand,
        .use_kind = request->use_kind,
        .adapter_kind = request->adapter_kind,
        .input_rep_kind = request->input_rep_kind,
        .output_rep_kind = request->output_rep_kind,
        .target_register_rep = UINT16_MAX,
        .target_memory_rep = UINT16_MAX,
        .target_slot = XR_SEMANTIC_INDEX_NONE,
        .layout = request->layout,
        .source_auxiliary_kind = source_auxiliary_kind,
        .source_flags = source_flags,
        .use_auxiliary_kind = use_auxiliary_kind,
        .use_flags = use_flags,
        .source_semantic_immediate = source_immediate,
        .use_semantic_immediate = use_immediate,
        .source_operation_id = source_id,
        .source_type_id = source_type->id,
        .use_operation_id = use_id,
        .policy_fingerprint = request->policy_fingerprint,
    };

    const XrTargetValueRepRecord *value_rep =
        xr_target_plan_value_rep(target_plan, request->source_value);
    const XrTargetMachineRepRecord *machine =
        value_rep
            ? xr_target_plan_machine_rep(target_plan, value_rep->register_rep)
            : NULL;
    bool enum_descriptor =
        request->adapter_kind == XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_BOX ||
        request->adapter_kind == XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_UNBOX;
    if (!machine || enum_descriptor) {
        if (request->layout != XR_SEMANTIC_INDEX_NONE)
            return XR_AOT_REFINEMENT_LAYOUT;
        representation_record_fingerprint(baseline, out, &out->fingerprint);
        return XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE;
    }

    bool boxes = request->adapter_kind == XR_AOT_REP_ADAPTER_BOX;
    uint16_t expected_input = boxes ? machine->kind : XR_MACHINE_REP_DYN_VALUE;
    uint16_t expected_output = boxes ? XR_MACHINE_REP_DYN_VALUE : machine->kind;
    uint16_t recipe = representation_recipe(request->adapter_kind, machine->kind);
    if (request->input_rep_kind != expected_input ||
        request->output_rep_kind != expected_output ||
        machine->kind == XR_MACHINE_REP_VOID ||
        machine->kind == XR_MACHINE_REP_DYN_VALUE ||
        recipe == XR_AOT_REP_RECIPE_NONE)
        return XR_AOT_REFINEMENT_REPRESENTATION;

    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts =
        xr_target_plan_layouts(target_plan, &layout_count);
    if (!layouts || request->layout >= layout_count ||
        layouts[request->layout].semantic_type != source_type_index ||
        fingerprint_is_zero(layouts[request->layout].fingerprint))
        return XR_AOT_REFINEMENT_LAYOUT;

    out->recipe = recipe;
    out->target_register_rep = value_rep->register_rep;
    out->target_memory_rep = value_rep->memory_rep;
    out->target_slot = value_rep->slot;
    machine_rep_fingerprint(machine, &out->machine_rep_fingerprint);
    out->layout_fingerprint = layouts[request->layout].fingerprint;
    representation_record_fingerprint(baseline, out, &out->fingerprint);
    return XR_AOT_REFINEMENT_OK;
}

/* Compare a recorded binding against the independently derived one and name
 * the obligation that disagrees, so a mismatch points at the field group that
 * drifted instead of at the record as a whole. */
static uint32_t direct_call_record_mismatch(
    const XrAotDirectCallRecord *actual,
    const XrAotDirectCallRecord *derived) {
    if (actual->target_call_index != derived->target_call_index ||
        actual->target_instruction != derived->target_instruction ||
        actual->caller_function != derived->caller_function ||
        actual->callee_function != derived->callee_function ||
        actual->semantic_call_target != derived->semantic_call_target ||
        actual->semantic_operation != derived->semantic_operation ||
        actual->target_kind != derived->target_kind ||
        actual->calling_convention != derived->calling_convention ||
        actual->semantic_target_kind != derived->semantic_target_kind ||
        !xr_stable_id_equal(actual->caller_identity, derived->caller_identity) ||
        !xr_stable_id_equal(actual->callee_identity, derived->callee_identity) ||
        !xr_stable_id_equal(actual->operation_id, derived->operation_id))
        return XR_AOT_REFINEMENT_DIRECT_CALL_CALLEE_IDENTITY;
    if (actual->argument_begin != derived->argument_begin ||
        actual->argument_count != derived->argument_count ||
        actual->parameter_count != derived->parameter_count ||
        !xr_fingerprint_equal(actual->argument_map_fingerprint,
                              derived->argument_map_fingerprint))
        return XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING;
    if (actual->result_value != derived->result_value ||
        actual->result_slot != derived->result_slot ||
        actual->result_mode != derived->result_mode ||
        actual->result_ownership != derived->result_ownership ||
        actual->result_register_rep != derived->result_register_rep ||
        actual->result_memory_rep != derived->result_memory_rep ||
        actual->native_abi != derived->native_abi)
        return XR_AOT_REFINEMENT_DIRECT_CALL_RESULT_MAPPING;
    if (actual->error_slot != derived->error_slot ||
        actual->error_mode != derived->error_mode)
        return XR_AOT_REFINEMENT_DIRECT_CALL_ERROR_MAPPING;
    if (actual->environment_required != derived->environment_required)
        return XR_AOT_REFINEMENT_DIRECT_CALL_ENVIRONMENT_MAPPING;
    if (actual->generation_required != derived->generation_required)
        return XR_AOT_REFINEMENT_DIRECT_CALL_GENERATION_MAPPING;
    if (actual->call_flags != derived->call_flags)
        return XR_AOT_REFINEMENT_DIRECT_CALL_EFFECT_MAPPING;
    if (!xr_fingerprint_equal(actual->fingerprint, derived->fingerprint))
        return XR_AOT_REFINEMENT_RECORD_FINGERPRINT;
    /* Any residue the groups above do not name still fails closed. */
    return memcmp(actual, derived, sizeof(*actual)) == 0
               ? XR_AOT_REFINEMENT_OK
               : XR_AOT_REFINEMENT_PLAN_STATE;
}

static void direct_call_record_fingerprint(const XrAotBaselineRef *baseline,
                                           XrAotDirectCallRecord *record) {
    static const uint8_t domain[] = "xray-aot-direct-call-record-v2\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    xr_sha256_update(&ctx, baseline->target_plan_fingerprint.bytes,
                     sizeof(baseline->target_plan_fingerprint.bytes));
    xr_sha256_update(&ctx, baseline->semantic_fingerprint.bytes,
                     sizeof(baseline->semantic_fingerprint.bytes));
    hash_u32(&ctx, record->target_call_index);
    hash_u32(&ctx, record->target_instruction);
    hash_u32(&ctx, record->caller_function);
    hash_u32(&ctx, record->callee_function);
    hash_u32(&ctx, record->semantic_call_target);
    hash_u32(&ctx, record->semantic_operation);
    hash_u32(&ctx, record->argument_begin);
    hash_u32(&ctx, record->result_value);
    hash_u32(&ctx, record->result_slot);
    hash_u32(&ctx, record->error_slot);
    hash_u32(&ctx, record->argument_count);
    hash_u32(&ctx, record->parameter_count);
    hash_u32(&ctx, record->call_flags);
    hash_u32(&ctx, record->result_register_rep);
    hash_u32(&ctx, record->result_memory_rep);
    hash_u32(&ctx, record->native_abi);
    hash_u32(&ctx, record->target_kind);
    hash_u32(&ctx, record->calling_convention);
    hash_u32(&ctx, record->result_mode);
    hash_u32(&ctx, record->result_ownership);
    hash_u32(&ctx, record->error_mode);
    hash_u32(&ctx, record->semantic_target_kind);
    hash_u32(&ctx, record->environment_required);
    hash_u32(&ctx, record->generation_required);
    xr_sha256_update(&ctx, record->caller_identity.bytes,
                     sizeof(record->caller_identity.bytes));
    xr_sha256_update(&ctx, record->callee_identity.bytes,
                     sizeof(record->callee_identity.bytes));
    xr_sha256_update(&ctx, record->operation_id.bytes,
                     sizeof(record->operation_id.bytes));
    xr_sha256_update(&ctx, record->argument_map_fingerprint.bytes,
                     sizeof(record->argument_map_fingerprint.bytes));
    xr_sha256_final(&ctx, record->fingerprint.bytes);
}

/* Independently re-derive the argument mapping and hash it, so a later
 * comparison covers every argument row rather than the count alone. */
static bool refinement_direct_array_ref_type_is_exact(
    const XrSemanticPlan *semantic, uint32_t type_index, uint8_t *storage) {
    uint32_t child_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_count);
    const XrSemanticTypeRecord *array =
        xr_semantic_plan_type(semantic, type_index);
    if (!children || !array || !storage || array->kind != XR_KIND_ARRAY ||
        array->builtin_type != XR_TID_NULL || array->child_count != 1 ||
        array->child_begin >= child_count ||
        array->scalar_rep != XR_SCALAR_REP_NONE ||
        array->aggregate_extent != 0 || array->aggregate_align != 0 ||
        array->flags !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(semantic, children[array->child_begin]);
    if (!element || element->builtin_type != XR_TID_NULL ||
        element->child_count != 0 || element->aggregate_extent != 0 ||
        element->aggregate_align != 0 || element->flags != 0)
        return false;
    if (element->kind == XR_KIND_BOOL &&
        element->scalar_rep == XR_SCALAR_REP_NONE) {
        *storage = XR_TARGET_ARRAY_STORAGE_BOOL;
        return true;
    }
    if (element->kind == XR_KIND_RUNE &&
        element->scalar_rep == XR_SCALAR_REP_NONE) {
        *storage = XR_TARGET_ARRAY_STORAGE_RUNE;
        return true;
    }
    switch (element->scalar_rep) {
        case XR_NATIVE_I8: *storage = XR_TARGET_ARRAY_STORAGE_I8; return true;
        case XR_NATIVE_U8: *storage = XR_TARGET_ARRAY_STORAGE_U8; return true;
        case XR_NATIVE_I16: *storage = XR_TARGET_ARRAY_STORAGE_I16; return true;
        case XR_NATIVE_U16: *storage = XR_TARGET_ARRAY_STORAGE_U16; return true;
        case XR_NATIVE_I32: *storage = XR_TARGET_ARRAY_STORAGE_I32; return true;
        case XR_NATIVE_U32: *storage = XR_TARGET_ARRAY_STORAGE_U32; return true;
        case XR_NATIVE_I64: *storage = XR_TARGET_ARRAY_STORAGE_I64; return true;
        case XR_NATIVE_U64: *storage = XR_TARGET_ARRAY_STORAGE_U64; return true;
        case XR_NATIVE_F32: *storage = XR_TARGET_ARRAY_STORAGE_F32; return true;
        case XR_NATIVE_F64: *storage = XR_TARGET_ARRAY_STORAGE_F64; return true;
        default: return false;
    }
}

static uint32_t direct_call_argument_map(
    const XrTargetPlan *target_plan, const XrSemanticPlan *caller_semantic,
    const XrSemanticPlan *callee_semantic, uint32_t callee_semantic_function,
    const XrTargetCallRecord *call, const XrSemanticFunctionRecord *callee,
    XrFingerprint *out_fingerprint, uint32_t *out_argument) {
    uint32_t argument_total = 0;
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(target_plan, &argument_total);
    *out_argument = 0;
    if (!arguments && argument_total != 0)
        return XR_AOT_REFINEMENT_PLAN_STATE;
    if ((uint64_t) call->argument_begin + call->argument_count >
        (uint64_t) argument_total)
        return XR_AOT_REFINEMENT_PLAN_STATE;
    /* A direct binding is only complete when every declared parameter is
     * supplied exactly once, in ordinal order. Fewer, extra or reordered
     * argument rows leave the callee frame unproven. */
    if (call->argument_count != callee->parameter_count)
        return XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING;
    size_t parameter_total = xr_semantic_plan_parameter_count(callee_semantic);
    if ((uint64_t) callee->parameter_begin + callee->parameter_count >
        (uint64_t) parameter_total)
        return XR_AOT_REFINEMENT_PLAN_STATE;
    static const uint8_t domain[] = "xray-aot-direct-call-argument-map-v2\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    hash_u32(&ctx, call->argument_count);
    for (uint16_t i = 0; i < call->argument_count; i++) {
        uint32_t argument_index = call->argument_begin + i;
        const XrTargetCallArgumentRecord *argument = &arguments[argument_index];
        uint32_t parameter_index = callee->parameter_begin + i;
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(callee_semantic, parameter_index);
        *out_argument = argument_index;
        if (!parameter)
            return XR_AOT_REFINEMENT_PLAN_STATE;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(caller_semantic, call->semantic_operation);
        uint32_t operand_total = 0;
        const XrSemanticOperandRecord *operands =
            xr_semantic_plan_operands(caller_semantic, &operand_total);
        uint32_t semantic_operand =
            operation ? operation->operand_begin + i + 1u
                      : XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperandRecord *operand =
            operands && semantic_operand < operand_total
                ? &operands[semantic_operand]
                : NULL;
        uint8_t array_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        bool array_ref =
            caller_semantic == callee_semantic && operation && operand &&
            parameter->type == operand->type &&
            refinement_direct_array_ref_type_is_exact(
                callee_semantic, parameter->type, &array_storage) &&
            parameter->mode == XR_PARAM_REF &&
            parameter->ownership == XI_OWN_BORROWED &&
            parameter->transfer_mode == XR_TRANSFER_SHARE &&
            (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 &&
            parameter->reserved == 0 &&
            operand->role == XR_SEM_OPERAND_ARGUMENT &&
            operand->parameter == (int16_t) i &&
            operand->parameter_mode == XR_PARAM_REF &&
            operand->access == XR_CALL_ARG_REF &&
            operand->origin != XI_PLACE_ORIGIN_NONE &&
            operand->lifetime == XI_PLACE_LIFETIME_CALL_BOUND &&
            operand->escape == XI_PLACE_ESCAPE_NONE &&
            operand->ownership_action == XR_SEM_OPERAND_BORROW &&
            operand->transfer_mode == XR_TRANSFER_SHARE &&
            operand->flags == (XR_SEM_OPERAND_CALL_CONTRACT |
                               XR_SEM_OPERAND_ADDRESSABLE);
        /* The row must name this call and bind the parameter that shares its
         * ordinal; anything else is an unproven permutation of the frame. */
        if (argument->call != call->id || argument->ordinal != i ||
            argument->callee_parameter != parameter_index ||
            parameter->function != callee_semantic_function ||
            parameter->ordinal != i)
            return XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING;
        const XrSemanticTypeRecord *parameter_type =
            xr_semantic_plan_type(callee_semantic, parameter->type);
        const XrSemanticTypeRecord *argument_type =
            operand ? xr_semantic_plan_type(caller_semantic, operand->type) : NULL;
        if (!parameter_type || !argument_type ||
            !xr_semantic_parameter_type_admits_argument(
                callee_semantic, parameter_type, argument_type))
            return XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING;
        /* Transfer mode must match the callee's declared contract: a mismatch
         * is a lost or duplicated obligation at the callee boundary. The
         * ownership action must be one of the two a by-value read-mode
         * parameter can carry, and both sides must already own proven
         * storage. */
        if (argument->transfer_mode != parameter->transfer_mode ||
            (!array_ref && parameter->mode != XR_PARAM_READ) ||
            argument->mode != (array_ref ? XR_TARGET_CALL_REFERENCE
                                         : XR_TARGET_CALL_VALUE) ||
            (array_ref ? argument->ownership != XR_TARGET_CALL_BORROW
                       : (argument->ownership != XR_TARGET_CALL_READ &&
                          argument->ownership != XR_TARGET_CALL_CONSUME)) ||
            argument->flags !=
                (array_ref ? XR_TARGET_CALL_ARGUMENT_ADDRESSABLE : 0) ||
            argument->array_element_storage !=
                (array_ref ? array_storage : XR_TARGET_ARRAY_STORAGE_NONE) ||
            argument->reserved8[0] != 0 || argument->reserved8[1] != 0 ||
            argument->reserved8[2] != 0 ||
            argument->caller_slot == XR_SEMANTIC_INDEX_NONE ||
            argument->callee_slot == XR_SEMANTIC_INDEX_NONE)
            return XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING;
        hash_u32(&ctx, argument_index);
        hash_u32(&ctx, parameter_index);
        hash_u32(&ctx, argument->semantic_operand);
        hash_u32(&ctx, argument->semantic_value);
        hash_u32(&ctx, argument->caller_slot);
        hash_u32(&ctx, argument->callee_slot);
        hash_u32(&ctx, argument->register_rep);
        hash_u32(&ctx, argument->memory_rep);
        hash_u32(&ctx, argument->callee_register_rep);
        hash_u32(&ctx, argument->callee_memory_rep);
        hash_u32(&ctx, argument->ownership);
        hash_u32(&ctx, argument->transfer_mode);
        hash_u32(&ctx, argument->flags);
        hash_u32(&ctx, argument->array_element_storage);
        hash_u32(&ctx, parameter->type);
        hash_u32(&ctx, parameter->mode);
        xr_sha256_update(&ctx, parameter->id.bytes, sizeof(parameter->id.bytes));
    }
    *out_argument = 0;
    xr_sha256_final(&ctx, out_fingerprint->bytes);
    return XR_AOT_REFINEMENT_OK;
}

static uint32_t direct_call_instruction_row(
    const XrTargetPlan *target_plan, const XrTargetCallRecord *call,
    uint32_t *out_instruction) {
    if (out_instruction)
        *out_instruction = XR_SEMANTIC_INDEX_NONE;
    if (!target_plan || !call || !out_instruction)
        return XR_AOT_REFINEMENT_INVALID_ARGUMENT;
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(target_plan, &instruction_count);
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; instructions && i < instruction_count; i++) {
        const XrTargetInstructionRecord *instruction = &instructions[i];
        if (instruction->opcode != XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 ||
            instruction->immediate_bits != call->id)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE ||
            instruction->id != i ||
            instruction->function != call->caller_function ||
            instruction->result_slot != call->result_slot)
            return XR_AOT_REFINEMENT_PLAN_STATE;
        match = i;
    }
    if (match == XR_SEMANTIC_INDEX_NONE)
        return XR_AOT_REFINEMENT_PLAN_STATE;
    *out_instruction = match;
    return XR_AOT_REFINEMENT_OK;
}

/* Recompute a direct-call binding from the verified baseline alone.
 *
 * The return value separates the two outcomes the protocol must never blur:
 * a DIRECT_CALL_* issue is a refusal (the binding is not provable, so the
 * baseline lowering stands), while INVALID_ARGUMENT or PLAN_STATE report a
 * baseline the verifier should already have rejected and therefore fail. */
static uint32_t derive_direct_call_record(const XrAotBaselineRef *baseline,
                                          const XrTargetPlan *target_plan,
                                          const XrAotDirectCallRequest *request,
                                          XrAotDirectCallRecord *out_record,
                                          uint32_t *out_argument) {
    memset(out_record, 0, sizeof(*out_record));
    out_record->target_instruction = XR_SEMANTIC_INDEX_NONE;
    *out_argument = 0;
    uint32_t call_total = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(target_plan, &call_total);
    if (!calls || request->target_call_index >= call_total)
        return XR_AOT_REFINEMENT_INVALID_ARGUMENT;
    const XrTargetCallRecord *call = &calls[request->target_call_index];
    out_record->target_call_index = request->target_call_index;
    out_record->caller_function = call->caller_function;
    out_record->callee_function = call->callee_function;
    out_record->semantic_call_target = call->semantic_call_target;
    out_record->semantic_operation = call->semantic_operation;
    out_record->argument_begin = call->argument_begin;
    out_record->argument_count = call->argument_count;
    out_record->result_value = call->result_value;
    out_record->result_slot = call->result_slot;
    out_record->error_slot = call->error_slot;
    out_record->call_flags = call->flags;
    out_record->result_register_rep = call->result_register_rep;
    out_record->result_memory_rep = call->result_memory_rep;
    out_record->native_abi = call->native_abi;
    out_record->target_kind = call->target_kind;
    out_record->calling_convention = call->calling_convention;
    out_record->result_mode = call->result_mode;
    out_record->result_ownership = call->result_ownership;
    out_record->error_mode = call->error_mode;

    bool local_direct =
        call->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL &&
        call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL;
    bool program_direct =
        call->target_kind == XR_TARGET_CALL_TARGET_PROGRAM_DIRECT &&
        call->calling_convention == XR_TARGET_CALL_CONVENTION_PROGRAM_DIRECT;
    if (call->id != request->target_call_index ||
        (!local_direct && !program_direct))
        return XR_AOT_REFINEMENT_DIRECT_CALL_TARGET_NOT_CLOSED;

    const XrSemanticPlan *caller_semantic = NULL;
    const XrSemanticPlan *callee_semantic = NULL;
    uint32_t caller_semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t callee_semantic_function = XR_SEMANTIC_INDEX_NONE;
    if (!xr_target_plan_function_semantic_binding(
            target_plan, call->caller_function, &caller_semantic,
            &caller_semantic_function) ||
        !xr_target_plan_function_semantic_binding(
            target_plan, call->callee_function, &callee_semantic,
            &callee_semantic_function))
        return XR_AOT_REFINEMENT_PLAN_STATE;

    const XrTargetProgramGraphRecord *program_graph = NULL;
    if (local_direct) {
        if (caller_semantic != callee_semantic ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE)
            return XR_AOT_REFINEMENT_DIRECT_CALL_TARGET_NOT_CLOSED;
    } else {
        uint32_t graph_count = 0;
        const XrTargetProgramGraphRecord *graphs =
            xr_target_plan_program_graphs(target_plan, &graph_count);
        program_graph = graph_count == 1u && graphs ? &graphs[0] : NULL;
        if (!program_graph ||
            program_graph->target_call != request->target_call_index ||
            program_graph->entry_target_function != call->caller_function ||
            program_graph->producer_target_function != call->callee_function ||
            program_graph->entry_semantic_function != caller_semantic_function ||
            program_graph->producer_semantic_function != callee_semantic_function ||
            xr_target_plan_semantic_module(target_plan,
                                           program_graph->entry_partition) !=
                caller_semantic ||
            xr_target_plan_semantic_module(target_plan,
                                           program_graph->producer_partition) !=
                callee_semantic ||
            call->source_dependency !=
                program_graph->entry_semantic_dependency ||
            call->source_export != program_graph->producer_semantic_export)
            return XR_AOT_REFINEMENT_PLAN_STATE;
    }

    size_t call_target_total =
        xr_semantic_plan_call_target_count(caller_semantic);
    if (call->semantic_call_target >= (uint32_t) call_target_total)
        return XR_AOT_REFINEMENT_PLAN_STATE;
    const XrSemanticCallTargetRecord *semantic_target =
        xr_semantic_plan_call_target(caller_semantic,
                                     call->semantic_call_target);
    if (!semantic_target)
        return XR_AOT_REFINEMENT_PLAN_STATE;
    out_record->semantic_target_kind = semantic_target->kind;
    if (local_direct) {
        if (semantic_target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
            semantic_target->dependency != XR_SEMANTIC_INDEX_NONE ||
            semantic_target->source_export != XR_SEMANTIC_INDEX_NONE ||
            semantic_target->function != callee_semantic_function)
            return XR_AOT_REFINEMENT_DIRECT_CALL_TARGET_NOT_CLOSED;
    } else if (semantic_target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT ||
               semantic_target->dependency != call->source_dependency ||
               semantic_target->source_export != call->source_export ||
               !xr_stable_id_equal(semantic_target->export_identity,
                                   call->source_export_identity) ||
               !xr_stable_id_equal(semantic_target->callee_function,
                                   call->source_callee_identity) ||
               !xr_stable_id_equal(program_graph->export_identity,
                                   call->source_export_identity) ||
               !xr_stable_id_equal(program_graph->exported_function_identity,
                                   call->source_callee_identity)) {
        return XR_AOT_REFINEMENT_DIRECT_CALL_TARGET_NOT_CLOSED;
    }

    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(callee_semantic, callee_semantic_function);
    const XrSemanticFunctionRecord *caller =
        xr_semantic_plan_function(caller_semantic, caller_semantic_function);
    if (!caller || !callee)
        return XR_AOT_REFINEMENT_PLAN_STATE;
    static const XrStableId unset_callee = {{0}};
    if (local_direct &&
        (!xr_stable_id_equal(semantic_target->callee_function, unset_callee) &&
         !xr_stable_id_equal(semantic_target->callee_function, callee->id)))
        return XR_AOT_REFINEMENT_DIRECT_CALL_CALLEE_IDENTITY;
    out_record->caller_identity =
        program_direct ? program_graph->entry_function_identity : caller->id;
    out_record->callee_identity =
        program_direct ? program_graph->producer_function_identity : callee->id;
    out_record->parameter_count = callee->parameter_count;

    size_t operation_total =
        xr_semantic_plan_operation_count(caller_semantic);
    if (call->semantic_operation >= (uint32_t) operation_total)
        return XR_AOT_REFINEMENT_PLAN_STATE;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(caller_semantic,
                                   call->semantic_operation);
    if (!operation)
        return XR_AOT_REFINEMENT_PLAN_STATE;
    out_record->operation_id = operation->id;
    if (operation->function != caller_semantic_function ||
        semantic_target->operation != call->semantic_operation)
        return XR_AOT_REFINEMENT_DIRECT_CALL_CALLEE_IDENTITY;

    uint32_t issue = XR_AOT_REFINEMENT_OK;
    if (program_direct) {
        issue = direct_call_instruction_row(
            target_plan, call, &out_record->target_instruction);
        if (issue != XR_AOT_REFINEMENT_OK)
            return issue;
    }
    issue = direct_call_argument_map(
        target_plan, caller_semantic, callee_semantic,
        callee_semantic_function, call, callee,
        &out_record->argument_map_fingerprint, out_argument);
    if (issue != XR_AOT_REFINEMENT_OK)
        return issue;

    /* Return mapping: the call must land the operation's own result value, by
     * value, with an ownership action a direct return can produce. A caller
     * storage indirection carries a separate materialization obligation that
     * this binding does not discharge. */
    if (call->result_value != operation->result_value ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        (call->result_ownership != XR_TARGET_CALL_NONE &&
         call->result_ownership != XR_TARGET_CALL_RETURN_OWNED))
        return XR_AOT_REFINEMENT_DIRECT_CALL_RESULT_MAPPING;

    /* Effect mapping: a call that can suspend resumes into caller storage, so
     * a non-void suspending result must already name the slot it resumes
     * into. Re-derived here rather than taken from the planner's word. */
    bool call_suspends = (call->flags & XR_TARGET_CALL_SUSPEND) != 0;
    bool result_is_void = call->result_value == XR_SEMANTIC_INDEX_NONE;
    if (call_suspends && !result_is_void &&
        call->result_slot == XR_SEMANTIC_INDEX_NONE)
        return XR_AOT_REFINEMENT_DIRECT_CALL_EFFECT_MAPPING;

    /* Error mapping: this binding proves only calls with no error edge. An
     * error edge would need a landing slot plus the caller's cleanup mapping,
     * and neither is discharged here, so such a row is refused rather than
     * bound. */
    if ((call->flags & XR_TARGET_CALL_ERROR) != 0 ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE)
        return XR_AOT_REFINEMENT_DIRECT_CALL_ERROR_MAPPING;

    /* Environment mapping: a capturing callee needs its environment threaded
     * through the call, which is a separate proof. Only a capture-free callee
     * with no environment edge is bound here. */
    bool callee_captures = callee->capture_count != 0;
    if (callee_captures || (call->flags & XR_TARGET_CALL_ENVIRONMENT) != 0)
        return XR_AOT_REFINEMENT_DIRECT_CALL_ENVIRONMENT_MAPPING;
    out_record->environment_required = 0u;

    /* Generation mapping: a generation barrier exists to re-resolve a callee
     * that may be replaced. A closed local callee is fixed for this plan, so
     * a barrier on it would contradict the closed-target proof. */
    if ((call->flags & XR_TARGET_CALL_GENERATION) != 0)
        return XR_AOT_REFINEMENT_DIRECT_CALL_GENERATION_MAPPING;
    out_record->generation_required = 0u;

    direct_call_record_fingerprint(baseline, out_record);
    return XR_AOT_REFINEMENT_OK;
}

static bool append_record(XrAotRefinementBuilder *builder,
                          const XrAotTransformationRecord *record,
                          XrAotRefinementDiagnostic *diag) {
    if (builder->record_count >= XR_AOT_REFINEMENT_MAX_RECORDS)
        return fail_diag(diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET,
                         builder->record_count, record->protocol.pass_id, 0);
    if (builder->record_count == builder->record_capacity) {
        uint32_t next_capacity = builder->record_capacity
                                     ? builder->record_capacity * 2u
                                     : 4u;
        if (next_capacity < builder->record_capacity ||
            next_capacity > SIZE_MAX / sizeof(*builder->records))
            return fail_diag(diag, XR_AOT_REFINEMENT_OUT_OF_MEMORY,
                             builder->record_count, record->protocol.pass_id,
                             record->direct_call.target_call_index);
        void *next = xr_realloc(builder->records,
                                next_capacity * sizeof(*builder->records));
        if (!next)
            return fail_diag(diag, XR_AOT_REFINEMENT_OUT_OF_MEMORY,
                             builder->record_count, record->protocol.pass_id,
                             record->direct_call.target_call_index);
        builder->records = (XrAotTransformationRecord *) next;
        builder->record_capacity = next_capacity;
    }
    builder->records[builder->record_count++] = *record;
    return true;
}

static int compare_record_key(const void *left, const void *right) {
    const XrAotTransformationRecord *a =
        (const XrAotTransformationRecord *) left;
    const XrAotTransformationRecord *b =
        (const XrAotTransformationRecord *) right;
    if (a->transform_kind != b->transform_kind)
        return a->transform_kind < b->transform_kind ? -1 : 1;
    if (a->transform_kind == XR_AOT_TRANSFORM_DIRECT_CALL) {
        if (a->direct_call.target_call_index == b->direct_call.target_call_index)
            return 0;
        return a->direct_call.target_call_index < b->direct_call.target_call_index
                   ? -1
                   : 1;
    }
    const XrAotRepresentationAdapterRecord *x = &a->representation_adapter;
    const XrAotRepresentationAdapterRecord *y = &b->representation_adapter;
#define XR_COMPARE_FIELD(field)                                                                    \
    do {                                                                                           \
        if (x->field != y->field)                                                                  \
            return x->field < y->field ? -1 : 1;                                                   \
    } while (0)
    XR_COMPARE_FIELD(source_function);
    XR_COMPARE_FIELD(use_kind);
    XR_COMPARE_FIELD(use_block);
    XR_COMPARE_FIELD(use_operation);
    XR_COMPARE_FIELD(use_operand);
    XR_COMPARE_FIELD(source_value);
#undef XR_COMPARE_FIELD
    return 0;
}

static void transformation_record_fingerprint(
    const XrAotBaselineRef *baseline, XrAotTransformationRecord *record,
    uint32_t index, uint32_t count) {
    static const uint8_t domain[] = "xray-aot-transformation-record-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    xr_sha256_update(&ctx, baseline->target_plan_fingerprint.bytes,
                     sizeof(baseline->target_plan_fingerprint.bytes));
    hash_u32(&ctx, index);
    hash_u32(&ctx, count);
    hash_u32(&ctx, record->protocol.schema_version);
    hash_u32(&ctx, record->protocol.pass_id);
    hash_u32(&ctx, record->protocol.transform_kind);
    hash_u32(&ctx, record->protocol.requires);
    hash_u32(&ctx, record->protocol.produces);
    hash_u32(&ctx, record->protocol.invalidates);
    hash_u32(&ctx, record->protocol.preserves);
    hash_u32(&ctx, record->input_state.available);
    hash_u32(&ctx, record->output_state.available);
    for (uint32_t i = 0; i < XR_AOT_INV_COUNT; i++) {
        hash_u64(&ctx, record->input_state.generation[i]);
        hash_u64(&ctx, record->output_state.generation[i]);
    }
    hash_u32(&ctx, record->decision);
    hash_u32(&ctx, record->transform_kind);
    hash_u32(&ctx, record->diagnostic_issue);
    if (record->transform_kind == XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER) {
        xr_sha256_update(&ctx, record->representation_adapter.fingerprint.bytes,
                         sizeof(record->representation_adapter.fingerprint.bytes));
    } else {
        hash_u32(&ctx, record->direct_call.target_call_index);
        xr_sha256_update(&ctx, record->direct_call_binding.fingerprint.bytes,
                         sizeof(record->direct_call_binding.fingerprint.bytes));
    }
    xr_sha256_final(&ctx, record->fingerprint.bytes);
}

static void refinement_plan_fingerprint(XrAotRefinementPlanView *view) {
    static const uint8_t domain[] = "xray-aot-refinement-plan-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    hash_u32(&ctx, view->schema_version);
    xr_sha256_update(&ctx, view->baseline.semantic_fingerprint.bytes,
                     sizeof(view->baseline.semantic_fingerprint.bytes));
    xr_sha256_update(&ctx, view->baseline.target_plan_fingerprint.bytes,
                     sizeof(view->baseline.target_plan_fingerprint.bytes));
    xr_sha256_update(&ctx, view->baseline.target_profile_fingerprint.bytes,
                     sizeof(view->baseline.target_profile_fingerprint.bytes));
    hash_u64(&ctx, view->baseline.completed_family_mask);
    hash_u32(&ctx, view->initial_state.available);
    for (uint32_t i = 0; i < XR_AOT_INV_COUNT; i++)
        hash_u64(&ctx, view->initial_state.generation[i]);
    hash_u32(&ctx, view->record_count);
    for (uint32_t i = 0; i < view->record_count; i++)
        xr_sha256_update(&ctx, view->records[i].fingerprint.bytes,
                         sizeof(view->records[i].fingerprint.bytes));
    xr_sha256_final(&ctx, view->fingerprint.bytes);
}

const char *xr_aot_refinement_issue_name(uint32_t issue) {
    switch ((XrAotRefinementIssue) issue) {
        case XR_AOT_REFINEMENT_OK:
            return "XR_AOT_REFINEMENT_OK";
        case XR_AOT_REFINEMENT_INVALID_ARGUMENT:
            return "XR_AOT_REFINEMENT_INVALID_ARGUMENT";
        case XR_AOT_REFINEMENT_OUT_OF_MEMORY:
            return "XR_AOT_REFINEMENT_OUT_OF_MEMORY";
        case XR_AOT_REFINEMENT_BASELINE_FINGERPRINT:
            return "XR_AOT_REFINEMENT_BASELINE_FINGERPRINT";
        case XR_AOT_REFINEMENT_PASS_PROTOCOL:
            return "XR_AOT_REFINEMENT_PASS_PROTOCOL";
        case XR_AOT_REFINEMENT_STALE_EVIDENCE:
            return "XR_AOT_REFINEMENT_STALE_EVIDENCE";
        case XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE:
            return "XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE";
        case XR_AOT_REFINEMENT_SOURCE_IDENTITY:
            return "XR_AOT_REFINEMENT_SOURCE_IDENTITY";
        case XR_AOT_REFINEMENT_USE_SITE:
            return "XR_AOT_REFINEMENT_USE_SITE";
        case XR_AOT_REFINEMENT_SOURCE_TYPE:
            return "XR_AOT_REFINEMENT_SOURCE_TYPE";
        case XR_AOT_REFINEMENT_REPRESENTATION:
            return "XR_AOT_REFINEMENT_REPRESENTATION";
        case XR_AOT_REFINEMENT_LAYOUT:
            return "XR_AOT_REFINEMENT_LAYOUT";
        case XR_AOT_REFINEMENT_RECORD_FINGERPRINT:
            return "XR_AOT_REFINEMENT_RECORD_FINGERPRINT";
        case XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE:
            return "XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE";
        case XR_AOT_REFINEMENT_DUPLICATE_USE:
            return "XR_AOT_REFINEMENT_DUPLICATE_USE";
        case XR_AOT_REFINEMENT_NONCANONICAL_ORDER:
            return "XR_AOT_REFINEMENT_NONCANONICAL_ORDER";
        case XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE:
            return "XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE";
        case XR_AOT_REFINEMENT_RESOURCE_BUDGET:
            return "XR_AOT_REFINEMENT_RESOURCE_BUDGET";
        case XR_AOT_REFINEMENT_PLAN_FINGERPRINT:
            return "XR_AOT_REFINEMENT_PLAN_FINGERPRINT";
        case XR_AOT_REFINEMENT_PLAN_STATE:
            return "XR_AOT_REFINEMENT_PLAN_STATE";
        case XR_AOT_REFINEMENT_DIRECT_CALL_TARGET_NOT_CLOSED:
            return "XR_AOT_REFINEMENT_DIRECT_CALL_TARGET_NOT_CLOSED";
        case XR_AOT_REFINEMENT_DIRECT_CALL_CALLEE_IDENTITY:
            return "XR_AOT_REFINEMENT_DIRECT_CALL_CALLEE_IDENTITY";
        case XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING:
            return "XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING";
        case XR_AOT_REFINEMENT_DIRECT_CALL_RESULT_MAPPING:
            return "XR_AOT_REFINEMENT_DIRECT_CALL_RESULT_MAPPING";
        case XR_AOT_REFINEMENT_DIRECT_CALL_ERROR_MAPPING:
            return "XR_AOT_REFINEMENT_DIRECT_CALL_ERROR_MAPPING";
        case XR_AOT_REFINEMENT_DIRECT_CALL_ENVIRONMENT_MAPPING:
            return "XR_AOT_REFINEMENT_DIRECT_CALL_ENVIRONMENT_MAPPING";
        case XR_AOT_REFINEMENT_DIRECT_CALL_GENERATION_MAPPING:
            return "XR_AOT_REFINEMENT_DIRECT_CALL_GENERATION_MAPPING";
        case XR_AOT_REFINEMENT_DIRECT_CALL_EFFECT_MAPPING:
            return "XR_AOT_REFINEMENT_DIRECT_CALL_EFFECT_MAPPING";
        default:
            return "XR_AOT_REFINEMENT_UNKNOWN";
    }
}

bool xr_aot_refinement_baseline_from_target_plan(
    const XrTargetPlan *target_plan, XrAotBaselineRef *out_baseline,
    XrAotRefinementDiagnostic *diag) {
    clear_diag(diag);
    if (out_baseline)
        memset(out_baseline, 0, sizeof(*out_baseline));
    if (!target_plan || !out_baseline)
        return fail_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT, 0, 0, 0);
    char error[256] = {0};
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    if (!xr_target_plan_is_verified(target_plan) ||
        !xr_target_plan_verify(target_plan, error, sizeof(error)) || !profile ||
        !xr_target_profile_is_frozen(profile) ||
        !xr_target_profile_verify(profile, error, sizeof(error)))
        return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, 0, 0, 0);
    *out_baseline = (XrAotBaselineRef) {
        .semantic_fingerprint =
            xr_target_plan_semantic_fingerprint(target_plan),
        .target_plan_fingerprint = xr_target_plan_fingerprint(target_plan),
        .target_profile_fingerprint = xr_target_profile_fingerprint(profile),
        .completed_family_mask =
            xr_target_plan_completed_family_mask(target_plan),
    };
    if (!baseline_valid(out_baseline)) {
        memset(out_baseline, 0, sizeof(*out_baseline));
        return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, 0, 0, 0);
    }
    return true;
}

XrAotInvariantState xr_aot_refinement_initial_state(
    const XrAotBaselineRef *baseline) {
    XrAotInvariantState state = {0};
    for (uint32_t i = 0; i < XR_AOT_INV_COUNT; i++)
        state.generation[i] = 1;
    if (!baseline_valid(baseline))
        return state;
    if ((baseline->completed_family_mask & XR_TARGET_FAMILY_SCALAR) != 0) {
        state.available = XR_AOT_INV_BIT(XR_AOT_INV_CFG) |
                          XR_AOT_INV_BIT(XR_AOT_INV_VALUES) |
                          XR_AOT_INV_BIT(XR_AOT_INV_TYPES);
    }
    /* Call-shape evidence is published only for a baseline that completed
     * every required family. Such a plan had its call, argument, slot, root,
     * cleanup and capability tables verified as one unit, and those tables are
     * exactly what a call-shape refinement reads. A partial family mask keeps
     * the whole set withheld, so every consumer refuses instead of assuming. */
    if ((baseline->completed_family_mask & XR_TARGET_REQUIRED_FAMILIES) ==
        XR_TARGET_REQUIRED_FAMILIES) {
        state.available |= XR_AOT_INV_BIT(XR_AOT_INV_CALL_TARGET) |
                           XR_AOT_INV_BIT(XR_AOT_INV_CALL_ABI) |
                           XR_AOT_INV_BIT(XR_AOT_INV_EFFECT) |
                           XR_AOT_INV_BIT(XR_AOT_INV_OWNERSHIP) |
                           XR_AOT_INV_BIT(XR_AOT_INV_LIFETIME) |
                           XR_AOT_INV_BIT(XR_AOT_INV_ERROR) |
                           XR_AOT_INV_BIT(XR_AOT_INV_ENVIRONMENT) |
                           XR_AOT_INV_BIT(XR_AOT_INV_GENERATION);
    }
    return state;
}

XrAotPassProtocol xr_aot_refinement_direct_call_protocol(uint32_t pass_id) {
    const XrAotInvariantMask invalidates =
        XR_AOT_INV_BIT(XR_AOT_INV_CFG) |
        XR_AOT_INV_BIT(XR_AOT_INV_VALUES) |
        XR_AOT_INV_BIT(XR_AOT_INV_CALL_TARGET) |
        XR_AOT_INV_BIT(XR_AOT_INV_EFFECT) |
        XR_AOT_INV_BIT(XR_AOT_INV_ESCAPE) |
        XR_AOT_INV_BIT(XR_AOT_INV_OWNERSHIP) |
        XR_AOT_INV_BIT(XR_AOT_INV_LIFETIME) |
        XR_AOT_INV_BIT(XR_AOT_INV_DEBUG);
    return (XrAotPassProtocol) {
        .schema_version = XR_AOT_REFINEMENT_SCHEMA_VERSION,
        .pass_id = pass_id,
        .transform_kind = XR_AOT_TRANSFORM_DIRECT_CALL,
        .requires = XR_AOT_INV_BIT(XR_AOT_INV_CFG) |
                    XR_AOT_INV_BIT(XR_AOT_INV_VALUES) |
                    XR_AOT_INV_BIT(XR_AOT_INV_TYPES) |
                    XR_AOT_INV_BIT(XR_AOT_INV_CALL_TARGET) |
                    XR_AOT_INV_BIT(XR_AOT_INV_CALL_ABI) |
                    XR_AOT_INV_BIT(XR_AOT_INV_EFFECT) |
                    XR_AOT_INV_BIT(XR_AOT_INV_OWNERSHIP) |
                    XR_AOT_INV_BIT(XR_AOT_INV_LIFETIME) |
                    XR_AOT_INV_BIT(XR_AOT_INV_ERROR) |
                    XR_AOT_INV_BIT(XR_AOT_INV_ENVIRONMENT) |
                    XR_AOT_INV_BIT(XR_AOT_INV_GENERATION),
        .produces = XR_AOT_INV_BIT(XR_AOT_INV_CFG) |
                    XR_AOT_INV_BIT(XR_AOT_INV_VALUES),
        .invalidates = invalidates,
        .preserves = XR_AOT_INV_ALL & ~invalidates,
    };
}

XrAotPassProtocol xr_aot_refinement_representation_protocol(uint32_t pass_id) {
    return (XrAotPassProtocol) {
        .schema_version = XR_AOT_REFINEMENT_SCHEMA_VERSION,
        .pass_id = pass_id,
        .transform_kind = XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER,
        .requires = XR_AOT_INV_BIT(XR_AOT_INV_VALUES) |
                    XR_AOT_INV_BIT(XR_AOT_INV_TYPES),
        .produces = 0,
        .invalidates = 0,
        .preserves = XR_AOT_INV_ALL,
    };
}

XrAotRefinementBuilder *xr_aot_refinement_builder_create(
    const XrTargetPlan *target_plan, XrAotRefinementDiagnostic *diag) {
    XrAotBaselineRef baseline;
    if (!xr_aot_refinement_baseline_from_target_plan(target_plan, &baseline,
                                                      diag))
        return NULL;
    XrAotRefinementBuilder *builder =
        (XrAotRefinementBuilder *) xr_calloc(1, sizeof(*builder));
    if (!builder) {
        fail_diag(diag, XR_AOT_REFINEMENT_OUT_OF_MEMORY, 0, 0, 0);
        return NULL;
    }
    builder->baseline = baseline;
    builder->initial_state = xr_aot_refinement_initial_state(&baseline);
    builder->current_state = builder->initial_state;
    if (!target_index_init(target_plan, &builder->target_index)) {
        fail_diag(diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, 0, 0, 0);
        xr_aot_refinement_builder_free(builder);
        return NULL;
    }
    return builder;
}

void xr_aot_refinement_builder_free(XrAotRefinementBuilder *builder) {
    if (!builder)
        return;
    target_index_dispose(&builder->target_index);
    xr_free(builder->records);
    xr_free(builder);
}

bool xr_aot_refinement_try_direct_call(
    XrAotRefinementBuilder *builder, const XrAotPassProtocol *protocol,
    const XrTargetPlan *target_plan, const XrAotDirectCallRequest *request,
    uint32_t *out_decision, XrAotRefinementDiagnostic *diag) {
    clear_diag(diag);
    if (out_decision)
        *out_decision = 0;
    if (!builder || builder->frozen || !protocol_valid(protocol) ||
        protocol->transform_kind != XR_AOT_TRANSFORM_DIRECT_CALL ||
        !target_plan || !request || !out_decision)
        return fail_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT,
                         builder ? builder->record_count : 0,
                         protocol ? protocol->pass_id : 0,
                         request ? request->target_call_index : 0);
    XrAotBaselineRef current;
    if (!xr_aot_refinement_baseline_from_target_plan(target_plan, &current,
                                                      diag))
        return false;
    if (!baseline_equal(&builder->baseline, &current))
        return fail_diag(diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT,
                         builder->record_count, protocol->pass_id,
                         request->target_call_index);
    /* Evidence gate first: a pass may not read call-shape facts the current
     * state no longer publishes, whatever the row itself looks like. */
    if ((builder->current_state.available & protocol->requires) !=
        protocol->requires) {
        XrAotTransformationRecord stale = {
            .protocol = *protocol,
            .input_state = builder->current_state,
            .output_state = builder->current_state,
            .direct_call = *request,
            .decision = XR_AOT_REFINEMENT_REFUSED,
            .transform_kind = XR_AOT_TRANSFORM_DIRECT_CALL,
            .diagnostic_issue = XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE,
        };
        if (!append_record(builder, &stale, diag))
            return false;
        *out_decision = XR_AOT_REFINEMENT_REFUSED;
        write_diag(diag, XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE,
                   builder->record_count - 1u, protocol->pass_id,
                   request->target_call_index);
        return true;
    }
    XrAotDirectCallRecord binding = {0};
    uint32_t argument_index = 0;
    uint32_t issue = derive_direct_call_record(&builder->baseline, target_plan,
                                               request, &binding,
                                               &argument_index);
    uint32_t target_call_count = 0;
    const XrTargetCallRecord *target_calls =
        xr_target_plan_calls(target_plan, &target_call_count);
    bool program_direct =
        target_calls && request->target_call_index < target_call_count &&
        target_calls[request->target_call_index].target_kind ==
            XR_TARGET_CALL_TARGET_PROGRAM_DIRECT &&
        target_calls[request->target_call_index].calling_convention ==
            XR_TARGET_CALL_CONVENTION_PROGRAM_DIRECT;
    /* A structural fault means the verified baseline disagrees with itself,
     * which the checker must surface as a failure rather than a refusal. A
     * PROGRAM_DIRECT row has already committed the program graph to static
     * lowering, so this consumer must either prove its one binding or fail;
     * silently retaining another executable path would create dual authority. */
    if (issue == XR_AOT_REFINEMENT_INVALID_ARGUMENT ||
        issue == XR_AOT_REFINEMENT_PLAN_STATE ||
        (program_direct && issue != XR_AOT_REFINEMENT_OK)) {
        uint32_t failure = program_direct ? XR_AOT_REFINEMENT_PLAN_STATE
                                          : issue;
        fail_diag(diag, failure, builder->record_count, protocol->pass_id,
                  request->target_call_index);
        if (diag)
            diag->semantic_operation = argument_index;
        return false;
    }
    uint32_t decision = issue == XR_AOT_REFINEMENT_OK
                            ? XR_AOT_REFINEMENT_APPLIED
                            : XR_AOT_REFINEMENT_REFUSED;
    /* A refusal keeps no derived binding: nothing downstream may read facts
     * from a transformation that was not proved. */
    if (decision != XR_AOT_REFINEMENT_APPLIED) {
        uint32_t call_index = request->target_call_index;
        memset(&binding, 0, sizeof(binding));
        binding.target_call_index = call_index;
    }
    XrAotTransformationRecord record = {
        .protocol = *protocol,
        .input_state = builder->current_state,
        .output_state = builder->current_state,
        .direct_call = *request,
        .direct_call_binding = binding,
        .decision = (uint16_t) decision,
        .transform_kind = XR_AOT_TRANSFORM_DIRECT_CALL,
        .diagnostic_issue = issue,
    };
    if (!append_record(builder, &record, diag))
        return false;
    *out_decision = decision;
    write_diag(diag, issue, builder->record_count - 1u, protocol->pass_id,
               request->target_call_index);
    return true;
}

bool xr_aot_refinement_try_representation_adapter(
    XrAotRefinementBuilder *builder, const XrAotPassProtocol *protocol,
    const XrTargetPlan *target_plan,
    const XrAotRepresentationAdapterRequest *request,
    uint32_t *out_decision,
    XrAotRefinementDiagnostic *diag) {
    clear_diag(diag);
    if (out_decision)
        *out_decision = XR_AOT_REFINEMENT_REFUSED;
    if (!builder || builder->frozen || !protocol_valid(protocol) ||
        protocol->transform_kind != XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER ||
        !target_plan || !request || !out_decision)
        return fail_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT,
                         builder ? builder->record_count : 0,
                         protocol ? protocol->pass_id : 0, 0);
    XrAotBaselineRef current;
    if (!xr_aot_refinement_baseline_from_target_plan(target_plan, &current, diag))
        return false;
    if (!baseline_equal(&builder->baseline, &current))
        return fail_diag(diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT,
                         builder->record_count, protocol->pass_id, 0);
    if ((builder->current_state.available & protocol->requires) !=
        protocol->requires)
        return fail_diag(diag, XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE,
                         builder->record_count, protocol->pass_id, 0);
    XrAotRepresentationAdapterRecord adapter = {0};
    uint32_t issue = derive_representation_record(
        &builder->baseline, target_plan, &builder->target_index, request,
        &adapter);
    if (issue != XR_AOT_REFINEMENT_OK &&
        issue != XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE) {
        fail_diag(diag, issue, builder->record_count, protocol->pass_id, 0);
        if (diag) {
            diag->semantic_value = request->source_value;
            diag->semantic_operation = request->use_operation;
        }
        return false;
    }
    uint32_t decision = issue == XR_AOT_REFINEMENT_OK
                            ? XR_AOT_REFINEMENT_APPLIED
                            : XR_AOT_REFINEMENT_REFUSED;
    XrAotTransformationRecord record = {
        .protocol = *protocol,
        .input_state = builder->current_state,
        .output_state = builder->current_state,
        .representation_adapter = adapter,
        .decision = (uint16_t) decision,
        .transform_kind = XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER,
        .diagnostic_issue = issue,
    };
    if (!append_record(builder, &record, diag))
        return false;
    *out_decision = decision;
    if (issue != XR_AOT_REFINEMENT_OK) {
        fail_diag(diag, issue, builder->record_count - 1u,
                  protocol->pass_id, 0);
        if (diag) {
            diag->semantic_value = request->source_value;
            diag->semantic_operation = request->use_operation;
        }
    }
    return true;
}

static bool verify_view(const XrAotRefinementPlanView *view,
                        const XrAotBaselineRef *current,
                        const XrTargetPlan *target_plan,
                        const XrAotTargetIndex *target_index,
                        bool require_verified,
                        XrAotRefinementDiagnostic *diag) {
    if (!view)
        return fail_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT, 0, 0, 0);
    if (view->schema_version != XR_AOT_REFINEMENT_SCHEMA_VERSION ||
        !view->frozen || (require_verified && !view->verified) ||
        !state_valid(&view->initial_state) ||
        (view->record_count != 0 && !view->records))
        return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, 0, 0, 0);
    if (view->record_count > XR_AOT_REFINEMENT_MAX_RECORDS)
        return fail_diag(diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, 0, 0, 0);
    if (!baseline_equal(&view->baseline, current))
        return fail_diag(diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT,
                         0, 0, 0);
    XrAotInvariantState expected =
        xr_aot_refinement_initial_state(&view->baseline);
    if (!state_equal(&expected, &view->initial_state))
        return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, 0, 0, 0);
    static const XrAotDirectCallRequest empty_direct_call = {0};
    static const XrAotDirectCallRecord empty_binding = {0};
    static const XrAotRepresentationAdapterRecord empty_adapter = {0};
    for (uint32_t i = 0; i < view->record_count; i++) {
        const XrAotTransformationRecord *record = &view->records[i];
        if (!protocol_valid(&record->protocol) ||
            record->transform_kind != record->protocol.transform_kind)
            return fail_diag(diag, XR_AOT_REFINEMENT_PASS_PROTOCOL, i,
                             record->protocol.pass_id,
                             record->direct_call.target_call_index);
        /* Each transform kind owns exactly one payload; the other must stay
         * zeroed so no consumer can read a field this kind never proved. */
        if ((record->transform_kind == XR_AOT_TRANSFORM_DIRECT_CALL &&
             (memcmp(&record->representation_adapter, &empty_adapter,
                     sizeof(empty_adapter)) != 0 ||
              record->direct_call_binding.target_call_index !=
                  record->direct_call.target_call_index)) ||
            (record->transform_kind ==
                 XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER &&
             (memcmp(&record->direct_call, &empty_direct_call,
                     sizeof(empty_direct_call)) != 0 ||
              memcmp(&record->direct_call_binding, &empty_binding,
                     sizeof(empty_binding)) != 0 ||
              record->representation_adapter.reserved != 0)))
            return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, i,
                             record->protocol.pass_id, 0);
    }
    for (uint32_t i = 1; i < view->record_count; i++) {
        int order = compare_record_key(&view->records[i - 1u],
                                       &view->records[i]);
        if (order >= 0)
            return fail_diag(diag,
                             order == 0 ? XR_AOT_REFINEMENT_DUPLICATE_USE
                                        : XR_AOT_REFINEMENT_NONCANONICAL_ORDER,
                             i, view->records[i].protocol.pass_id, 0);
    }
    for (uint32_t i = 0; i < view->record_count; i++) {
        const XrAotTransformationRecord *record = &view->records[i];
        if (!protocol_valid(&record->protocol) ||
            record->transform_kind != record->protocol.transform_kind)
            return fail_diag(diag, XR_AOT_REFINEMENT_PASS_PROTOCOL, i,
                             record->protocol.pass_id,
                             record->direct_call.target_call_index);
        if (!state_equal(&record->input_state, &expected))
            return fail_diag(diag, XR_AOT_REFINEMENT_STALE_EVIDENCE, i,
                             record->protocol.pass_id,
                             record->direct_call.target_call_index);
        if (record->transform_kind == XR_AOT_TRANSFORM_DIRECT_CALL) {
            /* Recording a proven binding edits nothing, so the state must be
             * carried through unchanged; the protocol's invalidation mask
             * applies to a consumer that goes on to apply the binding. */
            if (!state_equal(&record->output_state, &record->input_state))
                return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, i,
                                 record->protocol.pass_id,
                                 record->direct_call.target_call_index);
            uint32_t issue;
            XrAotDirectCallRecord derived = {0};
            uint32_t argument_index = 0;
            if ((expected.available & record->protocol.requires) !=
                record->protocol.requires) {
                issue = XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE;
            } else {
                /* Re-derive the binding from the baseline instead of trusting
                 * the recorded one. A record survives only if this
                 * independent pass reaches the same verdict and the same
                 * facts. */
                issue = derive_direct_call_record(&view->baseline, target_plan,
                                                  &record->direct_call,
                                                  &derived, &argument_index);
                if (issue == XR_AOT_REFINEMENT_INVALID_ARGUMENT ||
                    issue == XR_AOT_REFINEMENT_PLAN_STATE) {
                    fail_diag(diag, issue, i, record->protocol.pass_id,
                              record->direct_call.target_call_index);
                    if (diag)
                        diag->semantic_operation = argument_index;
                    return false;
                }
            }
            uint32_t expected_decision = issue == XR_AOT_REFINEMENT_OK
                                             ? XR_AOT_REFINEMENT_APPLIED
                                             : XR_AOT_REFINEMENT_REFUSED;
            if (expected_decision != XR_AOT_REFINEMENT_APPLIED) {
                uint32_t call_index = record->direct_call.target_call_index;
                memset(&derived, 0, sizeof(derived));
                derived.target_call_index = call_index;
            }
            if (record->decision != expected_decision ||
                record->diagnostic_issue != issue)
                return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, i,
                                 record->protocol.pass_id,
                                 record->direct_call.target_call_index);
            uint32_t mismatch =
                direct_call_record_mismatch(&record->direct_call_binding,
                                            &derived);
            if (mismatch != XR_AOT_REFINEMENT_OK)
                return fail_diag(diag, mismatch, i, record->protocol.pass_id,
                                 record->direct_call.target_call_index);
            XrAotTransformationRecord fingerprint_record = *record;
            memset(&fingerprint_record.fingerprint, 0,
                   sizeof(fingerprint_record.fingerprint));
            transformation_record_fingerprint(&view->baseline,
                                               &fingerprint_record, i,
                                               view->record_count);
            if (!xr_fingerprint_equal(record->fingerprint,
                                      fingerprint_record.fingerprint))
                return fail_diag(diag, XR_AOT_REFINEMENT_RECORD_FINGERPRINT,
                                 i, record->protocol.pass_id, 0);
            continue;
        }
        if ((expected.available & record->protocol.requires) !=
            record->protocol.requires)
            return fail_diag(diag, XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE, i,
                             record->protocol.pass_id, 0);
        const XrAotRepresentationAdapterRecord *actual =
            &record->representation_adapter;
        XrAotRepresentationAdapterRequest request = {
            .source_value = actual->source_value,
            .use_operation = actual->use_operation,
            .use_block = actual->use_block,
            .use_operand = actual->use_operand,
            .use_kind = actual->use_kind,
            .adapter_kind = actual->adapter_kind,
            .input_rep_kind = actual->input_rep_kind,
            .output_rep_kind = actual->output_rep_kind,
            .layout = actual->layout,
            .policy_fingerprint = actual->policy_fingerprint,
        };
        XrAotRepresentationAdapterRecord derived = {0};
        uint32_t issue = derive_representation_record(
            &view->baseline, target_plan, target_index, &request, &derived);
        if (issue != XR_AOT_REFINEMENT_OK &&
            issue != XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE) {
            fail_diag(diag, issue, i, record->protocol.pass_id, 0);
            if (diag) {
                diag->semantic_value = actual->source_value;
                diag->semantic_operation = actual->use_operation;
            }
            return false;
        }
        uint32_t expected_decision = issue == XR_AOT_REFINEMENT_OK
                                         ? XR_AOT_REFINEMENT_APPLIED
                                         : XR_AOT_REFINEMENT_REFUSED;
        if (record->decision != expected_decision ||
            record->diagnostic_issue != issue ||
            !state_equal(&record->output_state, &record->input_state))
            return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, i,
                             record->protocol.pass_id, 0);
        if (actual->source_function != derived.source_function ||
            actual->source_value != derived.source_value ||
            actual->source_operation != derived.source_operation ||
            actual->source_kind != derived.source_kind ||
            actual->source_auxiliary_kind != derived.source_auxiliary_kind ||
            actual->source_flags != derived.source_flags ||
            actual->source_semantic_immediate !=
                derived.source_semantic_immediate ||
            !xr_stable_id_equal(actual->source_operation_id,
                                derived.source_operation_id))
            return fail_diag(diag, XR_AOT_REFINEMENT_SOURCE_IDENTITY, i,
                             record->protocol.pass_id, 0);
        if (actual->source_type != derived.source_type ||
            !xr_stable_id_equal(actual->source_type_id,
                                derived.source_type_id))
            return fail_diag(diag, XR_AOT_REFINEMENT_SOURCE_TYPE, i,
                             record->protocol.pass_id, 0);
        if (actual->use_operation != derived.use_operation ||
            actual->use_block != derived.use_block ||
            actual->use_operand != derived.use_operand ||
            actual->use_kind != derived.use_kind ||
            actual->use_auxiliary_kind != derived.use_auxiliary_kind ||
            actual->use_flags != derived.use_flags ||
            actual->use_semantic_immediate !=
                derived.use_semantic_immediate ||
            !xr_stable_id_equal(actual->use_operation_id,
                                derived.use_operation_id))
            return fail_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i,
                             record->protocol.pass_id, 0);
        if (actual->adapter_kind != derived.adapter_kind ||
            actual->recipe != derived.recipe ||
            actual->input_rep_kind != derived.input_rep_kind ||
            actual->output_rep_kind != derived.output_rep_kind ||
            actual->target_register_rep != derived.target_register_rep ||
            actual->target_memory_rep != derived.target_memory_rep ||
            actual->target_slot != derived.target_slot ||
            !xr_fingerprint_equal(actual->policy_fingerprint,
                                  derived.policy_fingerprint) ||
            !xr_fingerprint_equal(actual->machine_rep_fingerprint,
                                  derived.machine_rep_fingerprint))
            return fail_diag(diag, XR_AOT_REFINEMENT_REPRESENTATION, i,
                             record->protocol.pass_id, 0);
        if (actual->layout != derived.layout ||
            !xr_fingerprint_equal(actual->layout_fingerprint,
                                  derived.layout_fingerprint))
            return fail_diag(diag, XR_AOT_REFINEMENT_LAYOUT, i,
                             record->protocol.pass_id, 0);
        if (!xr_fingerprint_equal(actual->fingerprint, derived.fingerprint))
            return fail_diag(diag, XR_AOT_REFINEMENT_RECORD_FINGERPRINT, i,
                             record->protocol.pass_id, 0);
        XrAotTransformationRecord fingerprint_record = *record;
        memset(&fingerprint_record.fingerprint, 0,
               sizeof(fingerprint_record.fingerprint));
        transformation_record_fingerprint(&view->baseline,
                                           &fingerprint_record, i,
                                           view->record_count);
        if (!xr_fingerprint_equal(record->fingerprint,
                                  fingerprint_record.fingerprint))
            return fail_diag(diag, XR_AOT_REFINEMENT_RECORD_FINGERPRINT,
                             i, record->protocol.pass_id, 0);
    }
    XrAotRefinementPlanView fingerprint_view = *view;
    memset(&fingerprint_view.fingerprint, 0,
           sizeof(fingerprint_view.fingerprint));
    refinement_plan_fingerprint(&fingerprint_view);
    if (!xr_fingerprint_equal(view->fingerprint,
                              fingerprint_view.fingerprint))
        return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_FINGERPRINT,
                         view->record_count, 0, 0);
    clear_diag(diag);
    return true;
}

bool xr_aot_refinement_builder_freeze(
    XrAotRefinementBuilder *builder, const XrTargetPlan *target_plan,
    XrAotRefinementPlan **out_plan, XrAotRefinementDiagnostic *diag) {
    clear_diag(diag);
    if (out_plan)
        *out_plan = NULL;
    if (!builder || builder->frozen || !target_plan || !out_plan)
        return fail_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT, 0, 0, 0);
    XrAotBaselineRef current;
    if (!xr_aot_refinement_baseline_from_target_plan(target_plan, &current,
                                                      diag))
        return false;
    if (!baseline_equal(&builder->baseline, &current))
        return fail_diag(diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT,
                         builder->record_count, 0, 0);
    XrAotRefinementPlan *plan =
        (XrAotRefinementPlan *) xr_calloc(1, sizeof(*plan));
    if (!plan)
        return fail_diag(diag, XR_AOT_REFINEMENT_OUT_OF_MEMORY,
                         builder->record_count, 0, 0);
    if (builder->record_count) {
        plan->records = (XrAotTransformationRecord *) xr_calloc(
            builder->record_count, sizeof(*plan->records));
        if (!plan->records) {
            xr_aot_refinement_plan_free(plan);
            return fail_diag(diag, XR_AOT_REFINEMENT_OUT_OF_MEMORY,
                             builder->record_count, 0, 0);
        }
        memcpy(plan->records, builder->records,
               builder->record_count * sizeof(*plan->records));
        qsort(plan->records, builder->record_count, sizeof(*plan->records),
              compare_record_key);
        for (uint32_t i = 1; i < builder->record_count; i++) {
            if (compare_record_key(&plan->records[i - 1u],
                                   &plan->records[i]) == 0) {
                uint32_t pass_id = plan->records[i].protocol.pass_id;
                xr_aot_refinement_plan_free(plan);
                return fail_diag(diag, XR_AOT_REFINEMENT_DUPLICATE_USE,
                                 i, pass_id, 0);
            }
        }
        for (uint32_t i = 0; i < builder->record_count; i++)
            transformation_record_fingerprint(&builder->baseline,
                                               &plan->records[i], i,
                                               builder->record_count);
    }
    plan->view = (XrAotRefinementPlanView) {
        .schema_version = XR_AOT_REFINEMENT_SCHEMA_VERSION,
        .baseline = builder->baseline,
        .initial_state = builder->initial_state,
        .records = plan->records,
        .record_count = builder->record_count,
        .frozen = true,
    };
    refinement_plan_fingerprint(&plan->view);
    if (!verify_view(&plan->view, &current, target_plan,
                     &builder->target_index, false, diag)) {
        xr_aot_refinement_plan_free(plan);
        return false;
    }
    plan->view.verified = true;
    builder->frozen = true;
    *out_plan = plan;
    return true;
}

void xr_aot_refinement_plan_free(XrAotRefinementPlan *plan) {
    if (!plan)
        return;
    xr_free(plan->records);
    xr_free(plan);
}

XrAotRefinementPlanView xr_aot_refinement_plan_view(
    const XrAotRefinementPlan *plan) {
    XrAotRefinementPlanView empty = {0};
    return plan ? plan->view : empty;
}

bool xr_aot_refinement_direct_call_authority_build(
    const XrTargetPlan *target_plan, uint32_t pass_id,
    XrAotRefinementPlan **out_plan, XrAotRefinementDiagnostic *diag) {
    clear_diag(diag);
    if (!target_plan || pass_id == 0 || !out_plan)
        return fail_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT, 0, pass_id, 0);
    *out_plan = NULL;
    uint32_t call_count = 0;
    if (!xr_target_plan_calls(target_plan, &call_count) && call_count != 0)
        return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, 0, pass_id, 0);
    if (call_count > XR_AOT_REFINEMENT_MAX_RECORDS)
        return fail_diag(diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, 0, pass_id, 0);
    XrAotRefinementBuilder *builder =
        xr_aot_refinement_builder_create(target_plan, diag);
    if (!builder)
        return false;
    XrAotPassProtocol protocol = xr_aot_refinement_direct_call_protocol(pass_id);
    /* Every call row is visited, so coverage is total: a row that cannot be
     * proved becomes an explicit refusal rather than an absent record. */
    for (uint32_t i = 0; i < call_count; i++) {
        XrAotDirectCallRequest request = {.target_call_index = i};
        uint32_t decision = 0;
        if (!xr_aot_refinement_try_direct_call(builder, &protocol, target_plan,
                                               &request, &decision, diag)) {
            xr_aot_refinement_builder_free(builder);
            return false;
        }
    }
    XrAotRefinementPlan *plan = NULL;
    if (!xr_aot_refinement_builder_freeze(builder, target_plan, &plan, diag)) {
        xr_aot_refinement_builder_free(builder);
        return false;
    }
    xr_aot_refinement_builder_free(builder);
    *out_plan = plan;
    return true;
}

bool xr_aot_refinement_verify(const XrAotRefinementPlanView *view,
                              const XrTargetPlan *target_plan,
                              XrAotRefinementDiagnostic *diag) {
    clear_diag(diag);
    XrAotBaselineRef current;
    if (!xr_aot_refinement_baseline_from_target_plan(target_plan, &current,
                                                      diag))
        return false;
    XrAotTargetIndex target_index = {0};
    if (!target_index_init(target_plan, &target_index))
        return fail_diag(diag, XR_AOT_REFINEMENT_RESOURCE_BUDGET, 0, 0, 0);
    bool valid = verify_view(view, &current, target_plan, &target_index, true,
                             diag);
    target_index_dispose(&target_index);
    return valid;
}
