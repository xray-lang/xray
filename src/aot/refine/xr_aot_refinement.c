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
#include "../../plan/target/xr_target_verify.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct XrAotRefinementBuilder {
    XrAotBaselineRef baseline;
    XrAotInvariantState initial_state;
    XrAotInvariantState current_state;
    XrAotTransformationRecord *records;
    uint32_t record_count;
    uint32_t record_capacity;
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

static void hash_u32(XrSHA256Context *ctx, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < 4; i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
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
    hash_u32(&ctx, record->use_operation);
    hash_u32(&ctx, record->use_operand);
    hash_u32(&ctx, record->adapter_kind);
    hash_u32(&ctx, record->input_rep_kind);
    hash_u32(&ctx, record->output_rep_kind);
    hash_u32(&ctx, record->layout);
    xr_sha256_update(&ctx, record->source_operation_id.bytes,
                     sizeof(record->source_operation_id.bytes));
    xr_sha256_update(&ctx, record->source_type_id.bytes,
                     sizeof(record->source_type_id.bytes));
    xr_sha256_update(&ctx, record->use_operation_id.bytes,
                     sizeof(record->use_operation_id.bytes));
    xr_sha256_update(&ctx, record->layout_fingerprint.bytes,
                     sizeof(record->layout_fingerprint.bytes));
    xr_sha256_final(&ctx, out->bytes);
}

static uint32_t derive_representation_record(
    const XrAotBaselineRef *baseline, const XrTargetPlan *target_plan,
    const XrAotRepresentationAdapterRequest *request,
    XrAotRepresentationAdapterRecord *out) {
    if (!baseline || !target_plan || !request || !out ||
        request->adapter_kind < XR_AOT_REP_ADAPTER_BOX ||
        request->adapter_kind >= XR_AOT_REP_ADAPTER_COUNT ||
        request->input_rep_kind >= XR_MACHINE_REP_COUNT ||
        request->output_rep_kind >= XR_MACHINE_REP_COUNT)
        return XR_AOT_REFINEMENT_REPRESENTATION;
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrTargetValueRepRecord *value_rep =
        xr_target_plan_value_rep(target_plan, request->source_value);
    if (!semantic || !value_rep)
        return XR_AOT_REFINEMENT_SOURCE_IDENTITY;
    const XrTargetMachineRepRecord *machine =
        xr_target_plan_machine_rep(target_plan, value_rep->register_rep);
    if (!machine)
        return XR_AOT_REFINEMENT_REPRESENTATION;

    uint32_t source_operation = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *source = NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate =
            xr_semantic_plan_operation(semantic, i);
        if (candidate && candidate->result_value == request->source_value) {
            if (source)
                return XR_AOT_REFINEMENT_SOURCE_IDENTITY;
            source = candidate;
            source_operation = i;
        }
    }
    if (!source)
        return XR_AOT_REFINEMENT_SOURCE_IDENTITY;
    const XrSemanticTypeRecord *source_type =
        xr_semantic_plan_type(semantic, source->result_type);
    if (!source_type)
        return XR_AOT_REFINEMENT_SOURCE_TYPE;
    bool enum_descriptor =
        request->adapter_kind == XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_BOX ||
        request->adapter_kind == XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_UNBOX;
    /* The scalar-only TargetPlan has no enum descriptor layout family yet. */
    if (enum_descriptor)
        return XR_AOT_REFINEMENT_SOURCE_TYPE;
    const XrSemanticOperationRecord *use =
        xr_semantic_plan_operation(semantic, request->use_operation);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    if (!use || request->use_operand >= use->operand_count ||
        use->operand_begin > operand_count ||
        use->operand_count > operand_count - use->operand_begin ||
        operands[use->operand_begin + request->use_operand].value !=
            request->source_value)
        return XR_AOT_REFINEMENT_USE_SITE;

    uint16_t expected_input = XR_MACHINE_REP_DYN_VALUE;
    uint16_t expected_output = XR_MACHINE_REP_DYN_VALUE;
    bool boxes = request->adapter_kind == XR_AOT_REP_ADAPTER_BOX ||
                 request->adapter_kind == XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_BOX;
    if (boxes)
        expected_input = machine->kind;
    else
        expected_output = machine->kind;
    if (request->input_rep_kind != expected_input ||
        request->output_rep_kind != expected_output ||
        machine->kind == XR_MACHINE_REP_VOID ||
        machine->kind == XR_MACHINE_REP_DYN_VALUE)
        return XR_AOT_REFINEMENT_REPRESENTATION;

    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts =
        xr_target_plan_layouts(target_plan, &layout_count);
    if (!layouts || request->layout >= layout_count ||
        layouts[request->layout].semantic_type != source->result_type ||
        fingerprint_is_zero(layouts[request->layout].fingerprint))
        return XR_AOT_REFINEMENT_LAYOUT;

    *out = (XrAotRepresentationAdapterRecord) {
        .source_function = source->function,
        .source_value = request->source_value,
        .source_operation = source_operation,
        .source_type = source->result_type,
        .use_operation = request->use_operation,
        .use_operand = request->use_operand,
        .adapter_kind = request->adapter_kind,
        .input_rep_kind = request->input_rep_kind,
        .output_rep_kind = request->output_rep_kind,
        .layout = request->layout,
        .source_operation_id = source->id,
        .source_type_id = source_type->id,
        .use_operation_id = use->id,
        .layout_fingerprint = layouts[request->layout].fingerprint,
    };
    representation_record_fingerprint(baseline, out, &out->fingerprint);
    return XR_AOT_REFINEMENT_OK;
}

static bool append_record(XrAotRefinementBuilder *builder,
                          const XrAotTransformationRecord *record,
                          XrAotRefinementDiagnostic *diag) {
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
        case XR_AOT_REFINEMENT_DIRECT_CALL_SCHEMA_UNAVAILABLE:
            return "XR_AOT_REFINEMENT_DIRECT_CALL_SCHEMA_UNAVAILABLE";
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
        case XR_AOT_REFINEMENT_PLAN_STATE:
            return "XR_AOT_REFINEMENT_PLAN_STATE";
        case XR_AOT_REFINEMENT_BACKEND_ABI:
            return "XR_AOT_REFINEMENT_BACKEND_ABI";
        case XR_AOT_REFINEMENT_BACKEND_INCOMPLETE_COVERAGE:
            return "XR_AOT_REFINEMENT_BACKEND_INCOMPLETE_COVERAGE";
        case XR_AOT_REFINEMENT_BACKEND_FAILURE:
            return "XR_AOT_REFINEMENT_BACKEND_FAILURE";
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
    if (baseline_valid(baseline) &&
        (baseline->completed_family_mask & XR_TARGET_FAMILY_SCALAR) != 0) {
        state.available = XR_AOT_INV_BIT(XR_AOT_INV_CFG) |
                          XR_AOT_INV_BIT(XR_AOT_INV_VALUES) |
                          XR_AOT_INV_BIT(XR_AOT_INV_TYPES);
    }
    return state;
}

bool xr_aot_refinement_state_after_invalidation(
    const XrAotInvariantState *input, XrAotInvariantMask invalidates,
    XrAotInvariantState *output, XrAotRefinementDiagnostic *diag) {
    clear_diag(diag);
    if (!state_valid(input) || !output || input == output || invalidates == 0 ||
        (invalidates & ~XR_AOT_INV_ALL) != 0)
        return fail_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT, 0, 0, 0);
    *output = *input;
    for (uint32_t i = 0; i < XR_AOT_INV_COUNT; i++) {
        if ((invalidates & XR_AOT_INV_BIT(i)) == 0)
            continue;
        if (output->generation[i] == UINT64_MAX)
            return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, 0, 0, 0);
        output->generation[i]++;
    }
    output->available &= ~invalidates;
    return true;
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
    return builder;
}

void xr_aot_refinement_builder_free(XrAotRefinementBuilder *builder) {
    if (!builder)
        return;
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
    /* Schema v1 deliberately does not snapshot an XrTargetCallRecord.  The
     * call family must first define its closed-target and mapping contract;
     * until then this typed row request remains a refusal-only audit record. */
    uint32_t issue = XR_AOT_REFINEMENT_DIRECT_CALL_SCHEMA_UNAVAILABLE;
    if ((builder->current_state.available & protocol->requires) !=
        protocol->requires)
        issue = XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE;
    XrAotTransformationRecord record = {
        .protocol = *protocol,
        .input_state = builder->current_state,
        .output_state = builder->current_state,
        .direct_call = *request,
        .decision = XR_AOT_REFINEMENT_REFUSED,
        .transform_kind = XR_AOT_TRANSFORM_DIRECT_CALL,
        .diagnostic_issue = issue,
    };
    if (!append_record(builder, &record, diag))
        return false;
    *out_decision = XR_AOT_REFINEMENT_REFUSED;
    write_diag(diag, issue, builder->record_count - 1u, protocol->pass_id,
               request->target_call_index);
    return true;
}

bool xr_aot_refinement_add_representation_adapter(
    XrAotRefinementBuilder *builder, const XrAotPassProtocol *protocol,
    const XrTargetPlan *target_plan,
    const XrAotRepresentationAdapterRequest *request,
    XrAotRefinementDiagnostic *diag) {
    clear_diag(diag);
    if (!builder || builder->frozen || !protocol_valid(protocol) ||
        protocol->transform_kind != XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER ||
        !target_plan || !request)
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
    uint32_t issue = derive_representation_record(&builder->baseline, target_plan,
                                                   request, &adapter);
    if (issue != XR_AOT_REFINEMENT_OK) {
        fail_diag(diag, issue, builder->record_count, protocol->pass_id, 0);
        if (diag) {
            diag->semantic_value = request->source_value;
            diag->semantic_operation = request->use_operation;
        }
        return false;
    }
    XrAotTransformationRecord record = {
        .protocol = *protocol,
        .input_state = builder->current_state,
        .output_state = builder->current_state,
        .representation_adapter = adapter,
        .decision = XR_AOT_REFINEMENT_APPLIED,
        .transform_kind = XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER,
        .diagnostic_issue = XR_AOT_REFINEMENT_OK,
    };
    if (!append_record(builder, &record, diag))
        return false;
    return true;
}

static bool verify_view(const XrAotRefinementPlanView *view,
                        const XrAotBaselineRef *current,
                        const XrTargetPlan *target_plan,
                        bool require_verified,
                        XrAotRefinementDiagnostic *diag) {
    if (!view)
        return fail_diag(diag, XR_AOT_REFINEMENT_INVALID_ARGUMENT, 0, 0, 0);
    if (view->schema_version != XR_AOT_REFINEMENT_SCHEMA_VERSION ||
        !view->frozen || (require_verified && !view->verified) ||
        !state_valid(&view->initial_state) ||
        (view->record_count != 0 && !view->records))
        return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, 0, 0, 0);
    if (!baseline_equal(&view->baseline, current))
        return fail_diag(diag, XR_AOT_REFINEMENT_BASELINE_FINGERPRINT,
                         0, 0, 0);
    XrAotInvariantState expected =
        xr_aot_refinement_initial_state(&view->baseline);
    if (!state_equal(&expected, &view->initial_state))
        return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, 0, 0, 0);
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
            uint32_t issue = XR_AOT_REFINEMENT_DIRECT_CALL_SCHEMA_UNAVAILABLE;
            if ((expected.available & record->protocol.requires) !=
                record->protocol.requires)
                issue = XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE;
            if (record->decision != XR_AOT_REFINEMENT_REFUSED ||
                record->diagnostic_issue != issue ||
                !state_equal(&record->output_state, &record->input_state))
                return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, i,
                                 record->protocol.pass_id,
                                 record->direct_call.target_call_index);
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
            .use_operand = actual->use_operand,
            .adapter_kind = actual->adapter_kind,
            .input_rep_kind = actual->input_rep_kind,
            .output_rep_kind = actual->output_rep_kind,
            .layout = actual->layout,
        };
        XrAotRepresentationAdapterRecord derived = {0};
        uint32_t issue = derive_representation_record(
            &view->baseline, target_plan, &request, &derived);
        if (issue != XR_AOT_REFINEMENT_OK) {
            fail_diag(diag, issue, i, record->protocol.pass_id, 0);
            if (diag) {
                diag->semantic_value = actual->source_value;
                diag->semantic_operation = actual->use_operation;
            }
            return false;
        }
        if (record->decision != XR_AOT_REFINEMENT_APPLIED ||
            record->diagnostic_issue != XR_AOT_REFINEMENT_OK ||
            !state_equal(&record->output_state, &record->input_state))
            return fail_diag(diag, XR_AOT_REFINEMENT_PLAN_STATE, i,
                             record->protocol.pass_id, 0);
        if (actual->source_function != derived.source_function ||
            actual->source_value != derived.source_value ||
            actual->source_operation != derived.source_operation ||
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
            actual->use_operand != derived.use_operand ||
            !xr_stable_id_equal(actual->use_operation_id,
                                derived.use_operation_id))
            return fail_diag(diag, XR_AOT_REFINEMENT_USE_SITE, i,
                             record->protocol.pass_id, 0);
        if (actual->adapter_kind != derived.adapter_kind ||
            actual->input_rep_kind != derived.input_rep_kind ||
            actual->output_rep_kind != derived.output_rep_kind)
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
    }
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
    }
    plan->view = (XrAotRefinementPlanView) {
        .schema_version = XR_AOT_REFINEMENT_SCHEMA_VERSION,
        .baseline = builder->baseline,
        .initial_state = builder->initial_state,
        .records = plan->records,
        .record_count = builder->record_count,
        .frozen = true,
    };
    if (!verify_view(&plan->view, &current, target_plan, false, diag)) {
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

bool xr_aot_refinement_verify(const XrAotRefinementPlanView *view,
                              const XrTargetPlan *target_plan,
                              XrAotRefinementDiagnostic *diag) {
    clear_diag(diag);
    XrAotBaselineRef current;
    if (!xr_aot_refinement_baseline_from_target_plan(target_plan, &current,
                                                      diag))
        return false;
    return verify_view(view, &current, target_plan, true, diag);
}

bool xr_aot_backend_run(const XrAotRefinementPlanView *view,
                        const XrTargetPlan *target_plan,
                        const XrAotBackendInterface *backend, void *context,
                        XrAotBackendStats *out_stats,
                        XrAotRefinementDiagnostic *diag) {
    clear_diag(diag);
    if (out_stats)
        memset(out_stats, 0, sizeof(*out_stats));
    if (!backend || !context || !out_stats ||
        backend->abi_version != XR_AOT_REFINEMENT_BACKEND_ABI_VERSION ||
        !backend->begin || !backend->visit || !backend->finish)
        return fail_diag(diag, XR_AOT_REFINEMENT_BACKEND_ABI, 0, 0, 0);
    if (!xr_aot_refinement_verify(view, target_plan, diag))
        return false;
    for (uint32_t i = 0; i < view->record_count; i++) {
        if ((backend->supported_transforms &
             XR_AOT_TRANSFORM_BIT(view->records[i].transform_kind)) == 0)
            return fail_diag(diag,
                             XR_AOT_REFINEMENT_BACKEND_INCOMPLETE_COVERAGE,
                             i, view->records[i].protocol.pass_id,
                             view->records[i].direct_call.target_call_index);
    }
    if (!backend->begin(context, &view->baseline, view->record_count))
        return fail_diag(diag, XR_AOT_REFINEMENT_BACKEND_FAILURE, 0, 0, 0);
    for (uint32_t i = 0; i < view->record_count; i++) {
        const XrAotTransformationRecord *record = &view->records[i];
        if (!backend->visit(context, i, record))
            return fail_diag(diag, XR_AOT_REFINEMENT_BACKEND_FAILURE, i,
                             record->protocol.pass_id,
                             record->direct_call.target_call_index);
        out_stats->visited++;
        if (record->decision == XR_AOT_REFINEMENT_APPLIED)
            out_stats->applied++;
        else
            out_stats->refused++;
    }
    if (!backend->finish(context))
        return fail_diag(diag, XR_AOT_REFINEMENT_BACKEND_FAILURE,
                         view->record_count, 0, 0);
    return true;
}

static bool null_begin(void *context, const XrAotBaselineRef *baseline,
                       uint32_t record_count) {
    XrAotNullBackend *backend = (XrAotNullBackend *) context;
    if (!backend || !baseline_valid(baseline))
        return false;
    memset(&backend->stats, 0, sizeof(backend->stats));
    backend->expected_records = record_count;
    return true;
}

static bool null_visit(void *context, uint32_t index,
                       const XrAotTransformationRecord *record) {
    XrAotNullBackend *backend = (XrAotNullBackend *) context;
    if (!backend || !record || index != backend->stats.visited)
        return false;
    backend->stats.visited++;
    if (record->decision == XR_AOT_REFINEMENT_APPLIED)
        backend->stats.applied++;
    else
        backend->stats.refused++;
    return true;
}

static bool null_finish(void *context) {
    const XrAotNullBackend *backend = (const XrAotNullBackend *) context;
    return backend && backend->stats.visited == backend->expected_records;
}

static bool test_append(XrAotTestBackend *backend, const char *format, ...) {
    if (!backend || !backend->buffer || backend->length >= backend->capacity)
        return false;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(backend->buffer + backend->length,
                            backend->capacity - backend->length, format, args);
    va_end(args);
    if (written < 0 || (size_t) written >= backend->capacity - backend->length)
        return false;
    backend->length += (size_t) written;
    return true;
}

static bool test_begin(void *context, const XrAotBaselineRef *baseline,
                       uint32_t record_count) {
    XrAotTestBackend *backend = (XrAotTestBackend *) context;
    char semantic[XR_FINGERPRINT_BYTES * 2 + 1];
    char target[XR_FINGERPRINT_BYTES * 2 + 1];
    char profile[XR_FINGERPRINT_BYTES * 2 + 1];
    if (!backend || !backend->buffer || backend->capacity == 0 ||
        !baseline_valid(baseline))
        return false;
    backend->length = 0;
    backend->visited = 0;
    backend->expected_records = record_count;
    backend->buffer[0] = '\0';
    xr_fingerprint_hex(baseline->semantic_fingerprint, semantic);
    xr_fingerprint_hex(baseline->target_plan_fingerprint, target);
    xr_fingerprint_hex(baseline->target_profile_fingerprint, profile);
    return test_append(backend,
                       "baseline semantic=%s target=%s profile=%s families=%016llx records=%u\n",
                       semantic, target, profile,
                       (unsigned long long) baseline->completed_family_mask,
                       (unsigned) record_count);
}

static bool test_visit(void *context, uint32_t index,
                        const XrAotTransformationRecord *record) {
    XrAotTestBackend *backend = (XrAotTestBackend *) context;
    if (!backend || !record || index != backend->visited)
        return false;
    backend->visited++;
    if (record->transform_kind == XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER) {
        const XrAotRepresentationAdapterRecord *adapter =
            &record->representation_adapter;
        return test_append(
            backend,
            "record=%u pass=%u transform=representation-adapter decision=applied "
            "kind=%u source=%u use=%u:%u rep=%u->%u layout=%u\n",
            (unsigned) index, (unsigned) record->protocol.pass_id,
            (unsigned) adapter->adapter_kind,
            (unsigned) adapter->source_value,
            (unsigned) adapter->use_operation,
            (unsigned) adapter->use_operand,
            (unsigned) adapter->input_rep_kind,
            (unsigned) adapter->output_rep_kind,
            (unsigned) adapter->layout);
    }
    return test_append(
        backend,
        "record=%u pass=%u transform=direct-call decision=refused target-call=%u issue=%s\n",
        (unsigned) index, (unsigned) record->protocol.pass_id,
        (unsigned) record->direct_call.target_call_index,
        xr_aot_refinement_issue_name(record->diagnostic_issue));
}

static bool test_finish(void *context) {
    XrAotTestBackend *backend = (XrAotTestBackend *) context;
    return backend && backend->visited == backend->expected_records &&
           test_append(backend, "end records=%u\n",
                       (unsigned) backend->visited);
}

const XrAotBackendInterface *xr_aot_null_backend_interface(void) {
    static const XrAotBackendInterface interface = {
        .abi_version = XR_AOT_REFINEMENT_BACKEND_ABI_VERSION,
        .supported_transforms =
            XR_AOT_TRANSFORM_BIT(XR_AOT_TRANSFORM_DIRECT_CALL) |
            XR_AOT_TRANSFORM_BIT(XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER),
        .begin = null_begin,
        .visit = null_visit,
        .finish = null_finish,
    };
    return &interface;
}

const XrAotBackendInterface *xr_aot_test_backend_interface(void) {
    static const XrAotBackendInterface interface = {
        .abi_version = XR_AOT_REFINEMENT_BACKEND_ABI_VERSION,
        .supported_transforms =
            XR_AOT_TRANSFORM_BIT(XR_AOT_TRANSFORM_DIRECT_CALL) |
            XR_AOT_TRANSFORM_BIT(XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER),
        .begin = test_begin,
        .visit = test_visit,
        .finish = test_finish,
    };
    return &interface;
}
