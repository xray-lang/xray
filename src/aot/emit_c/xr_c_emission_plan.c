/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_emission_plan.c - Immutable TargetPlan-backed C emission plan
 */

#include "xr_c_emission_plan.h"
#include "../../plan/target/xr_target_plan.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

#define XR_C_EMISSION_PLAN_SCHEMA_VERSION UINT32_C(4)

struct XrCEmissionPlan {
    XrCValueEmissionView *values;
    uint32_t value_count;
    uint32_t schema_version;
    XrFingerprint target_fingerprint;
    XrFingerprint profile_fingerprint;
    XrFingerprint fingerprint;
    bool verified;
};

static bool emission_error(char *error, size_t error_size, const char *code,
                           const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", code, detail);
    return false;
}

static bool machine_kind_to_c_rep(uint16_t kind, XrCValueRep *out, const char **c_type) {
    if (!out || !c_type)
        return false;
    switch (kind) {
        case XR_MACHINE_REP_VOID:
            *out = XR_C_VALUE_REP_VOID;
            *c_type = "void";
            return true;
        case XR_MACHINE_REP_I1:
            *out = XR_C_VALUE_REP_BOOL;
            *c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I8:
            *out = XR_C_VALUE_REP_I8;
            *c_type = "int8_t";
            return true;
        case XR_MACHINE_REP_U8:
            *out = XR_C_VALUE_REP_U8;
            *c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I16:
            *out = XR_C_VALUE_REP_I16;
            *c_type = "int16_t";
            return true;
        case XR_MACHINE_REP_U16:
            *out = XR_C_VALUE_REP_U16;
            *c_type = "uint16_t";
            return true;
        case XR_MACHINE_REP_I32:
            *out = XR_C_VALUE_REP_I32;
            *c_type = "int32_t";
            return true;
        case XR_MACHINE_REP_U32:
            *out = XR_C_VALUE_REP_U32;
            *c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_I64:
            *out = XR_C_VALUE_REP_I64;
            *c_type = "int64_t";
            return true;
        case XR_MACHINE_REP_U64:
            *out = XR_C_VALUE_REP_U64;
            *c_type = "uint64_t";
            return true;
        case XR_MACHINE_REP_ISIZE:
            *out = XR_C_VALUE_REP_ISIZE;
            *c_type = "ptrdiff_t";
            return true;
        case XR_MACHINE_REP_USIZE:
            *out = XR_C_VALUE_REP_USIZE;
            *c_type = "size_t";
            return true;
        case XR_MACHINE_REP_F32:
            *out = XR_C_VALUE_REP_F32;
            *c_type = "float";
            return true;
        case XR_MACHINE_REP_F64:
            *out = XR_C_VALUE_REP_F64;
            *c_type = "double";
            return true;
        case XR_MACHINE_REP_RUNE:
            *out = XR_C_VALUE_REP_RUNE;
            *c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_DYN_VALUE:
            *out = XR_C_VALUE_REP_TAGGED;
            *c_type = "XrValue";
            return true;
        default: return false;
    }
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t encoded[8];
    for (uint32_t i = 0; i < sizeof(encoded); i++)
        encoded[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(ctx, encoded, sizeof(encoded));
}

static void compute_fingerprint(const XrCEmissionPlan *plan, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-c-emission-plan-v4\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    hash_u64(&ctx, plan->schema_version);
    xr_sha256_update(&ctx, plan->target_fingerprint.bytes,
                     sizeof(plan->target_fingerprint.bytes));
    xr_sha256_update(&ctx, plan->profile_fingerprint.bytes,
                     sizeof(plan->profile_fingerprint.bytes));
    hash_u64(&ctx, plan->value_count);
    for (uint32_t i = 0; i < plan->value_count; i++) {
        const XrCValueEmissionView *value = &plan->values[i];
        hash_u64(&ctx, value->semantic_value);
        hash_u64(&ctx, value->target_register_rep);
        hash_u64(&ctx, value->target_memory_rep);
        hash_u64(&ctx, value->target_register_kind);
        hash_u64(&ctx, value->target_memory_kind);
        hash_u64(&ctx, value->register_bits);
        hash_u64(&ctx, value->memory_align);
        hash_u64(&ctx, value->memory_size);
        hash_u64(&ctx, value->rep);
        size_t c_type_length = strlen(value->c_type);
        hash_u64(&ctx, c_type_length);
        xr_sha256_update(&ctx, (const uint8_t *) value->c_type, c_type_length);
    }
    xr_sha256_final(&ctx, out->bytes);
}

static bool verify_value(const XrCValueEmissionView *value) {
    XrCValueRep expected_rep = XR_C_VALUE_REP_COUNT;
    const char *expected_c_type = NULL;
    if (value->target_register_kind != value->target_memory_kind)
        return false;
    switch (value->target_register_kind) {
        case XR_MACHINE_REP_VOID:
            expected_rep = XR_C_VALUE_REP_VOID;
            expected_c_type = "void";
            break;
        case XR_MACHINE_REP_I1:
            expected_rep = XR_C_VALUE_REP_BOOL;
            expected_c_type = "uint8_t";
            break;
        case XR_MACHINE_REP_I8:
            expected_rep = XR_C_VALUE_REP_I8;
            expected_c_type = "int8_t";
            break;
        case XR_MACHINE_REP_U8:
            expected_rep = XR_C_VALUE_REP_U8;
            expected_c_type = "uint8_t";
            break;
        case XR_MACHINE_REP_I16:
            expected_rep = XR_C_VALUE_REP_I16;
            expected_c_type = "int16_t";
            break;
        case XR_MACHINE_REP_U16:
            expected_rep = XR_C_VALUE_REP_U16;
            expected_c_type = "uint16_t";
            break;
        case XR_MACHINE_REP_I32:
            expected_rep = XR_C_VALUE_REP_I32;
            expected_c_type = "int32_t";
            break;
        case XR_MACHINE_REP_U32:
            expected_rep = XR_C_VALUE_REP_U32;
            expected_c_type = "uint32_t";
            break;
        case XR_MACHINE_REP_I64:
            expected_rep = XR_C_VALUE_REP_I64;
            expected_c_type = "int64_t";
            break;
        case XR_MACHINE_REP_U64:
            expected_rep = XR_C_VALUE_REP_U64;
            expected_c_type = "uint64_t";
            break;
        case XR_MACHINE_REP_ISIZE:
            expected_rep = XR_C_VALUE_REP_ISIZE;
            expected_c_type = "ptrdiff_t";
            break;
        case XR_MACHINE_REP_USIZE:
            expected_rep = XR_C_VALUE_REP_USIZE;
            expected_c_type = "size_t";
            break;
        case XR_MACHINE_REP_F32:
            expected_rep = XR_C_VALUE_REP_F32;
            expected_c_type = "float";
            break;
        case XR_MACHINE_REP_F64:
            expected_rep = XR_C_VALUE_REP_F64;
            expected_c_type = "double";
            break;
        case XR_MACHINE_REP_RUNE:
            expected_rep = XR_C_VALUE_REP_RUNE;
            expected_c_type = "uint32_t";
            break;
        case XR_MACHINE_REP_DYN_VALUE:
            expected_rep = XR_C_VALUE_REP_TAGGED;
            expected_c_type = "XrValue";
            break;
        default: return false;
    }
    return expected_rep == (XrCValueRep) value->rep && value->c_type &&
           strcmp(value->c_type, expected_c_type) == 0 &&
           (value->rep == XR_C_VALUE_REP_VOID
                ? value->register_bits == 0 && value->memory_size == 0 &&
                      value->memory_align == 0
                : value->register_bits != 0 && value->memory_size != 0 &&
                      value->memory_align != 0);
}

static bool verify_plan(const XrCEmissionPlan *plan) {
    if (!plan || plan->schema_version != XR_C_EMISSION_PLAN_SCHEMA_VERSION ||
        (plan->value_count && !plan->values))
        return false;
    for (uint32_t i = 0; i < plan->value_count; i++) {
        if (!verify_value(&plan->values[i]) ||
            (i && plan->values[i - 1u].semantic_value >= plan->values[i].semantic_value))
            return false;
    }
    XrFingerprint actual = {{0}};
    compute_fingerprint(plan, &actual);
    return xr_fingerprint_equal(actual, plan->fingerprint);
}

static bool verify_target_kind_projection(uint16_t kind,
                                          XrCValueRep *out_rep,
                                          const char **out_c_type) {
    if (!out_rep || !out_c_type)
        return false;
    switch ((XrMachineRepKind) kind) {
        case XR_MACHINE_REP_VOID:
            *out_rep = XR_C_VALUE_REP_VOID;
            *out_c_type = "void";
            return true;
        case XR_MACHINE_REP_I1:
            *out_rep = XR_C_VALUE_REP_BOOL;
            *out_c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I8:
            *out_rep = XR_C_VALUE_REP_I8;
            *out_c_type = "int8_t";
            return true;
        case XR_MACHINE_REP_U8:
            *out_rep = XR_C_VALUE_REP_U8;
            *out_c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I16:
            *out_rep = XR_C_VALUE_REP_I16;
            *out_c_type = "int16_t";
            return true;
        case XR_MACHINE_REP_U16:
            *out_rep = XR_C_VALUE_REP_U16;
            *out_c_type = "uint16_t";
            return true;
        case XR_MACHINE_REP_I32:
            *out_rep = XR_C_VALUE_REP_I32;
            *out_c_type = "int32_t";
            return true;
        case XR_MACHINE_REP_U32:
            *out_rep = XR_C_VALUE_REP_U32;
            *out_c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_I64:
            *out_rep = XR_C_VALUE_REP_I64;
            *out_c_type = "int64_t";
            return true;
        case XR_MACHINE_REP_U64:
            *out_rep = XR_C_VALUE_REP_U64;
            *out_c_type = "uint64_t";
            return true;
        case XR_MACHINE_REP_ISIZE:
            *out_rep = XR_C_VALUE_REP_ISIZE;
            *out_c_type = "ptrdiff_t";
            return true;
        case XR_MACHINE_REP_USIZE:
            *out_rep = XR_C_VALUE_REP_USIZE;
            *out_c_type = "size_t";
            return true;
        case XR_MACHINE_REP_F32:
            *out_rep = XR_C_VALUE_REP_F32;
            *out_c_type = "float";
            return true;
        case XR_MACHINE_REP_F64:
            *out_rep = XR_C_VALUE_REP_F64;
            *out_c_type = "double";
            return true;
        case XR_MACHINE_REP_RUNE:
            *out_rep = XR_C_VALUE_REP_RUNE;
            *out_c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_DYN_VALUE:
            *out_rep = XR_C_VALUE_REP_TAGGED;
            *out_c_type = "XrValue";
            return true;
        default: return false;
    }
}

bool xr_c_emission_plan_verify(
    const XrCEmissionPlan *plan, const XrTargetPlan *target_plan,
    XrFingerprint expected_profile_fingerprint, char *error,
    size_t error_size) {
    if (!plan || !target_plan || !xr_target_plan_is_verified(target_plan))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission verification authority is missing");
    XrFingerprint target_fingerprint = xr_target_plan_fingerprint(target_plan);
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    XrFingerprint profile_fingerprint =
        xr_target_profile_fingerprint(profile);
    if (!profile ||
        !xr_fingerprint_equal(profile_fingerprint,
                              expected_profile_fingerprint) ||
        !xr_fingerprint_equal(plan->profile_fingerprint,
                              expected_profile_fingerprint))
        return emission_error(error, error_size, "XR_TARGET_1000",
                              "C emission profile fingerprint is stale");
    if (!xr_fingerprint_equal(plan->target_fingerprint,
                              target_fingerprint))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission TargetPlan fingerprint is stale");
    if (plan->schema_version != XR_C_EMISSION_PLAN_SCHEMA_VERSION ||
        (plan->value_count && !plan->values))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission schema is invalid");

    uint32_t value_count = 0;
    const XrTargetValueRepRecord *values =
        xr_target_plan_value_reps(target_plan, &value_count);
    if (value_count && !values)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "TargetPlan value-representation table is missing");
    uint32_t projected = 0;
    for (uint32_t i = 0; i < value_count; i++) {
        const XrTargetValueRepRecord *binding = &values[i];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        XrCValueRep expected_register = XR_C_VALUE_REP_COUNT;
        XrCValueRep expected_memory = XR_C_VALUE_REP_COUNT;
        const char *register_c_type = NULL;
        const char *memory_c_type = NULL;
        bool register_supported = register_rep &&
            verify_target_kind_projection(register_rep->kind,
                                          &expected_register,
                                          &register_c_type);
        bool memory_supported = memory_rep &&
            verify_target_kind_projection(memory_rep->kind, &expected_memory,
                                          &memory_c_type);
        if (!register_supported && !memory_supported)
            continue;
        if (!register_supported || !memory_supported ||
            register_rep->kind != memory_rep->kind ||
            expected_register != expected_memory ||
            strcmp(register_c_type, memory_c_type) != 0)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "TargetPlan C projection is inconsistent");
        if (projected >= plan->value_count)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission projection is missing a TargetPlan row");
        const XrCValueEmissionView *row = &plan->values[projected++];
        if (row->semantic_value > binding->semantic_value)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission projection is missing a TargetPlan row");
        if (row->semantic_value < binding->semantic_value)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission projection has an extra row");
        if (row->semantic_value != binding->semantic_value ||
            row->target_register_rep != binding->register_rep ||
            row->target_memory_rep != binding->memory_rep ||
            row->target_register_kind != register_rep->kind ||
            row->target_memory_kind != memory_rep->kind ||
            row->register_bits != register_rep->register_bits ||
            row->memory_size != memory_rep->memory_size ||
            row->memory_align != memory_rep->memory_align ||
            row->rep != (uint8_t) expected_register || !row->c_type ||
            strcmp(row->c_type, register_c_type) != 0)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission row disagrees with TargetPlan authority");
    }
    if (projected != plan->value_count)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission projection has an extra row");
    if (!verify_plan(plan))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission fingerprint or canonical form is invalid");
    return true;
}

bool xr_c_emission_plan_build(const XrTargetPlan *target_plan,
                              XrFingerprint expected_profile_fingerprint,
                              XrCEmissionPlan **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!target_plan || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan input is missing");
    if (!xr_target_plan_is_verified(target_plan))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan requires a verified TargetPlan");
    const uint64_t required_value_families =
        XR_TARGET_FAMILY_SCALAR | XR_TARGET_FAMILY_CLOSURE_STORAGE;
    if ((xr_target_plan_completed_family_mask(target_plan) &
         required_value_families) != required_value_families)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan requires completed value-storage families");
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    XrFingerprint actual_profile_fingerprint = xr_target_profile_fingerprint(profile);
    if (!profile || !xr_fingerprint_equal(actual_profile_fingerprint,
                                           expected_profile_fingerprint))
        return emission_error(error, error_size, "XR_TARGET_1000",
                              "C emission target profile fingerprint does not match");
    uint32_t target_value_count = 0;
    const XrTargetValueRepRecord *values =
        xr_target_plan_value_reps(target_plan, &target_value_count);
    if (target_value_count && !values)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "TargetPlan value-representation table is missing");
    uint32_t emission_value_count = 0;
    for (uint32_t i = 0; i < target_value_count; i++) {
        const XrTargetValueRepRecord *binding = &values[i];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        XrCValueRep register_c_rep = XR_C_VALUE_REP_COUNT;
        XrCValueRep memory_c_rep = XR_C_VALUE_REP_COUNT;
        const char *register_c_type = NULL;
        const char *memory_c_type = NULL;
        bool register_is_value =
            register_rep && machine_kind_to_c_rep(register_rep->kind, &register_c_rep,
                                                   &register_c_type);
        bool memory_is_value =
            memory_rep && machine_kind_to_c_rep(memory_rep->kind, &memory_c_rep,
                                                 &memory_c_type);
        if (!register_is_value && !memory_is_value)
            continue;
        if (!register_is_value || !memory_is_value || register_rep->kind != memory_rep->kind ||
            register_c_rep != memory_c_rep || strcmp(register_c_type, memory_c_type) != 0) {
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "TargetPlan value binding has no exact C projection");
        }
        emission_value_count++;
    }
    if (emission_value_count > SIZE_MAX / sizeof(XrCValueEmissionView))
        return emission_error(error, error_size, "XR_EXEC_5003",
                              "C emission value record budget overflow");
    XrCEmissionPlan *plan = (XrCEmissionPlan *) xr_calloc(1, sizeof(*plan));
    if (!plan)
        return emission_error(error, error_size, "XR_EXEC_5003",
                              "C emission plan allocation failed");
    if (emission_value_count) {
        plan->values =
            (XrCValueEmissionView *) xr_calloc(emission_value_count,
                                               sizeof(*plan->values));
        if (!plan->values) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C emission value record allocation failed");
        }
    }
    plan->value_count = emission_value_count;
    plan->schema_version = XR_C_EMISSION_PLAN_SCHEMA_VERSION;
    plan->target_fingerprint = xr_target_plan_fingerprint(target_plan);
    plan->profile_fingerprint = actual_profile_fingerprint;
    uint32_t value_index = 0;
    for (uint32_t i = 0; i < target_value_count; i++) {
        const XrTargetValueRepRecord *binding = &values[i];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        XrCValueRep c_rep = XR_C_VALUE_REP_COUNT;
        const char *c_type = NULL;
        if (!register_rep || !memory_rep || register_rep->kind != memory_rep->kind ||
            !machine_kind_to_c_rep(register_rep->kind, &c_rep, &c_type))
            continue;
        XrCValueEmissionView *value = &plan->values[value_index++];
        value->semantic_value = binding->semantic_value;
        value->target_register_rep = binding->register_rep;
        value->target_memory_rep = binding->memory_rep;
        value->target_register_kind = register_rep->kind;
        value->target_memory_kind = memory_rep->kind;
        value->register_bits = register_rep->register_bits;
        value->memory_align = memory_rep->memory_align;
        value->memory_size = memory_rep->memory_size;
        value->rep = (uint8_t) c_rep;
        value->c_type = c_type;
    }
    if (value_index != emission_value_count) {
        xr_c_emission_plan_free(plan);
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission value partition is not exact");
    }
    compute_fingerprint(plan, &plan->fingerprint);
    if (!xr_c_emission_plan_verify(plan, target_plan,
                                   expected_profile_fingerprint, error,
                                   error_size)) {
        xr_c_emission_plan_free(plan);
        return false;
    }
    plan->verified = true;
    *out = plan;
    return true;
}

void xr_c_emission_plan_free(XrCEmissionPlan *plan) {
    if (!plan)
        return;
    xr_free(plan->values);
    xr_free(plan);
}

bool xr_c_emission_plan_is_verified(const XrCEmissionPlan *plan) {
    return plan && plan->verified;
}

uint32_t xr_c_emission_plan_value_count(const XrCEmissionPlan *plan) {
    return plan ? plan->value_count : 0;
}

XrFingerprint xr_c_emission_plan_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->fingerprint : zero;
}

XrFingerprint xr_c_emission_plan_target_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->target_fingerprint : zero;
}

XrFingerprint xr_c_emission_plan_profile_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->profile_fingerprint : zero;
}

bool xr_c_emission_plan_value_view(const XrCEmissionPlan *plan, uint32_t semantic_value,
                                    XrCValueEmissionView *out, char *error,
                                    size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!plan || !plan->verified || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "verified C emission plan input is missing");
    uint32_t begin = 0;
    uint32_t end = plan->value_count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2u;
        const XrCValueEmissionView *candidate = &plan->values[middle];
        if (candidate->semantic_value == semantic_value) {
            *out = *candidate;
            return true;
        }
        if (candidate->semantic_value < semantic_value)
            begin = middle + 1u;
        else
            end = middle;
    }
    return emission_error(error, error_size, "XR_TARGET_1001",
                          "semantic C value has no immutable C emission binding");
}
