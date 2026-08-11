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
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

#define XR_C_EMISSION_PLAN_SCHEMA_VERSION UINT32_C(1)

struct XrCEmissionPlan {
    XrCScalarEmissionView *scalars;
    uint32_t scalar_count;
    uint32_t schema_version;
    XrFingerprint target_fingerprint;
    XrFingerprint fingerprint;
    bool verified;
};

static bool emission_error(char *error, size_t error_size, const char *code,
                           const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", code, detail);
    return false;
}

static bool machine_kind_to_c_rep(uint16_t kind, XrCScalarRep *out,
                                  const char **c_type) {
    if (!out || !c_type)
        return false;
    switch (kind) {
        case XR_MACHINE_REP_VOID:
            *out = XR_C_SCALAR_REP_VOID;
            *c_type = "void";
            return true;
        case XR_MACHINE_REP_I1:
            *out = XR_C_SCALAR_REP_BOOL;
            *c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I8:
            *out = XR_C_SCALAR_REP_I8;
            *c_type = "int8_t";
            return true;
        case XR_MACHINE_REP_U8:
            *out = XR_C_SCALAR_REP_U8;
            *c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I16:
            *out = XR_C_SCALAR_REP_I16;
            *c_type = "int16_t";
            return true;
        case XR_MACHINE_REP_U16:
            *out = XR_C_SCALAR_REP_U16;
            *c_type = "uint16_t";
            return true;
        case XR_MACHINE_REP_I32:
            *out = XR_C_SCALAR_REP_I32;
            *c_type = "int32_t";
            return true;
        case XR_MACHINE_REP_U32:
            *out = XR_C_SCALAR_REP_U32;
            *c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_I64:
            *out = XR_C_SCALAR_REP_I64;
            *c_type = "int64_t";
            return true;
        case XR_MACHINE_REP_U64:
            *out = XR_C_SCALAR_REP_U64;
            *c_type = "uint64_t";
            return true;
        case XR_MACHINE_REP_ISIZE:
            *out = XR_C_SCALAR_REP_ISIZE;
            *c_type = "ptrdiff_t";
            return true;
        case XR_MACHINE_REP_USIZE:
            *out = XR_C_SCALAR_REP_USIZE;
            *c_type = "size_t";
            return true;
        case XR_MACHINE_REP_F32:
            *out = XR_C_SCALAR_REP_F32;
            *c_type = "float";
            return true;
        case XR_MACHINE_REP_F64:
            *out = XR_C_SCALAR_REP_F64;
            *c_type = "double";
            return true;
        case XR_MACHINE_REP_RUNE:
            *out = XR_C_SCALAR_REP_RUNE;
            *c_type = "uint32_t";
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
    static const uint8_t domain[] = "xray-c-emission-plan-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    hash_u64(&ctx, plan->schema_version);
    xr_sha256_update(&ctx, plan->target_fingerprint.bytes,
                     sizeof(plan->target_fingerprint.bytes));
    hash_u64(&ctx, plan->scalar_count);
    for (uint32_t i = 0; i < plan->scalar_count; i++) {
        const XrCScalarEmissionView *scalar = &plan->scalars[i];
        hash_u64(&ctx, scalar->semantic_value);
        hash_u64(&ctx, scalar->target_register_rep);
        hash_u64(&ctx, scalar->target_memory_rep);
        hash_u64(&ctx, scalar->target_register_kind);
        hash_u64(&ctx, scalar->target_memory_kind);
        hash_u64(&ctx, scalar->register_bits);
        hash_u64(&ctx, scalar->memory_align);
        hash_u64(&ctx, scalar->memory_size);
        hash_u64(&ctx, scalar->rep);
        size_t c_type_length = strlen(scalar->c_type);
        hash_u64(&ctx, c_type_length);
        xr_sha256_update(&ctx, (const uint8_t *) scalar->c_type, c_type_length);
    }
    xr_sha256_final(&ctx, out->bytes);
}

static bool verify_scalar(const XrCScalarEmissionView *scalar) {
    XrCScalarRep expected_rep = XR_C_SCALAR_REP_COUNT;
    const char *expected_c_type = NULL;
    if (scalar->target_register_kind != scalar->target_memory_kind)
        return false;
    switch (scalar->target_register_kind) {
        case XR_MACHINE_REP_VOID:
            expected_rep = XR_C_SCALAR_REP_VOID;
            expected_c_type = "void";
            break;
        case XR_MACHINE_REP_I1:
            expected_rep = XR_C_SCALAR_REP_BOOL;
            expected_c_type = "uint8_t";
            break;
        case XR_MACHINE_REP_I8:
            expected_rep = XR_C_SCALAR_REP_I8;
            expected_c_type = "int8_t";
            break;
        case XR_MACHINE_REP_U8:
            expected_rep = XR_C_SCALAR_REP_U8;
            expected_c_type = "uint8_t";
            break;
        case XR_MACHINE_REP_I16:
            expected_rep = XR_C_SCALAR_REP_I16;
            expected_c_type = "int16_t";
            break;
        case XR_MACHINE_REP_U16:
            expected_rep = XR_C_SCALAR_REP_U16;
            expected_c_type = "uint16_t";
            break;
        case XR_MACHINE_REP_I32:
            expected_rep = XR_C_SCALAR_REP_I32;
            expected_c_type = "int32_t";
            break;
        case XR_MACHINE_REP_U32:
            expected_rep = XR_C_SCALAR_REP_U32;
            expected_c_type = "uint32_t";
            break;
        case XR_MACHINE_REP_I64:
            expected_rep = XR_C_SCALAR_REP_I64;
            expected_c_type = "int64_t";
            break;
        case XR_MACHINE_REP_U64:
            expected_rep = XR_C_SCALAR_REP_U64;
            expected_c_type = "uint64_t";
            break;
        case XR_MACHINE_REP_ISIZE:
            expected_rep = XR_C_SCALAR_REP_ISIZE;
            expected_c_type = "ptrdiff_t";
            break;
        case XR_MACHINE_REP_USIZE:
            expected_rep = XR_C_SCALAR_REP_USIZE;
            expected_c_type = "size_t";
            break;
        case XR_MACHINE_REP_F32:
            expected_rep = XR_C_SCALAR_REP_F32;
            expected_c_type = "float";
            break;
        case XR_MACHINE_REP_F64:
            expected_rep = XR_C_SCALAR_REP_F64;
            expected_c_type = "double";
            break;
        case XR_MACHINE_REP_RUNE:
            expected_rep = XR_C_SCALAR_REP_RUNE;
            expected_c_type = "uint32_t";
            break;
        default: return false;
    }
    return expected_rep == (XrCScalarRep) scalar->rep && scalar->c_type &&
           strcmp(scalar->c_type, expected_c_type) == 0 &&
           (scalar->rep == XR_C_SCALAR_REP_VOID
                ? scalar->register_bits == 0 && scalar->memory_size == 0 &&
                      scalar->memory_align == 0
                : scalar->register_bits != 0 && scalar->memory_size != 0 &&
                      scalar->memory_align != 0);
}

static bool verify_plan(const XrCEmissionPlan *plan) {
    if (!plan || plan->schema_version != XR_C_EMISSION_PLAN_SCHEMA_VERSION ||
        (plan->scalar_count && !plan->scalars))
        return false;
    for (uint32_t i = 0; i < plan->scalar_count; i++) {
        if (!verify_scalar(&plan->scalars[i]) ||
            (i && plan->scalars[i - 1u].semantic_value >= plan->scalars[i].semantic_value))
            return false;
    }
    XrFingerprint actual = {{0}};
    compute_fingerprint(plan, &actual);
    return xr_fingerprint_equal(actual, plan->fingerprint);
}

bool xr_c_emission_plan_build(const XrTargetPlan *target_plan, XrCEmissionPlan **out,
                              char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!target_plan || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan input is missing");
    if (!xr_target_plan_is_verified(target_plan))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan requires a verified TargetPlan");
    uint32_t value_count = 0;
    const XrTargetValueRepRecord *values =
        xr_target_plan_value_reps(target_plan, &value_count);
    if (value_count > SIZE_MAX / sizeof(XrCScalarEmissionView))
        return emission_error(error, error_size, "XR_EXEC_5003",
                              "C emission scalar record budget overflow");
    XrCEmissionPlan *plan = (XrCEmissionPlan *) xr_calloc(1, sizeof(*plan));
    if (!plan)
        return emission_error(error, error_size, "XR_EXEC_5003",
                              "C emission plan allocation failed");
    if (value_count) {
        plan->scalars =
            (XrCScalarEmissionView *) xr_calloc(value_count, sizeof(*plan->scalars));
        if (!plan->scalars) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C emission scalar record allocation failed");
        }
    }
    plan->scalar_count = value_count;
    plan->schema_version = XR_C_EMISSION_PLAN_SCHEMA_VERSION;
    plan->target_fingerprint = xr_target_plan_fingerprint(target_plan);
    for (uint32_t i = 0; i < value_count; i++) {
        const XrTargetValueRepRecord *binding = &values[i];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        XrCScalarRep c_rep = XR_C_SCALAR_REP_COUNT;
        const char *c_type = NULL;
        /* This scalar foundation freezes exact register/memory kind identity.
         * A future non-scalar or conversion family must partition its own
         * records instead of weakening this contract. */
        if (!register_rep || !memory_rep || register_rep->kind != memory_rep->kind ||
            !machine_kind_to_c_rep(register_rep->kind, &c_rep, &c_type)) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "TargetPlan value binding has no scalar C projection");
        }
        XrCScalarEmissionView *scalar = &plan->scalars[i];
        scalar->semantic_value = binding->semantic_value;
        scalar->target_register_rep = binding->register_rep;
        scalar->target_memory_rep = binding->memory_rep;
        scalar->target_register_kind = register_rep->kind;
        scalar->target_memory_kind = memory_rep->kind;
        scalar->register_bits = register_rep->register_bits;
        scalar->memory_align = memory_rep->memory_align;
        scalar->memory_size = memory_rep->memory_size;
        scalar->rep = (uint8_t) c_rep;
        scalar->c_type = c_type;
    }
    compute_fingerprint(plan, &plan->fingerprint);
    if (!verify_plan(plan)) {
        xr_c_emission_plan_free(plan);
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan verification failed");
    }
    plan->verified = true;
    *out = plan;
    return true;
}

void xr_c_emission_plan_free(XrCEmissionPlan *plan) {
    if (!plan)
        return;
    xr_free(plan->scalars);
    xr_free(plan);
}

bool xr_c_emission_plan_is_verified(const XrCEmissionPlan *plan) {
    return plan && plan->verified;
}

uint32_t xr_c_emission_plan_scalar_count(const XrCEmissionPlan *plan) {
    return plan ? plan->scalar_count : 0;
}

XrFingerprint xr_c_emission_plan_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->fingerprint : zero;
}

XrFingerprint xr_c_emission_plan_target_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->target_fingerprint : zero;
}

bool xr_c_emission_plan_scalar_view(const XrCEmissionPlan *plan, uint32_t semantic_value,
                                    XrCScalarEmissionView *out, char *error,
                                    size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!plan || !plan->verified || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "verified C emission plan input is missing");
    uint32_t begin = 0;
    uint32_t end = plan->scalar_count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2u;
        const XrCScalarEmissionView *candidate = &plan->scalars[middle];
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
                          "semantic scalar value has no immutable C emission binding");
}
