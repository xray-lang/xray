/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_profile.c - Immutable target profile storage and hashing
 */

#include "xr_target_profile_internal.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../core/xr_core_spec_gen.h"
#include <stdio.h>
#include <string.h>

static void set_error(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
}

static void set_runtime_error(char *error, size_t size, const char *detail,
                              XrRuntimeAbiStatus status) {
    if (error && size)
        snprintf(error, size, "XR_TARGET_1000: %s: %s", detail, xr_runtime_abi_status_name(status));
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void hash_fingerprint(XrSHA256Context *ctx, XrFingerprint fingerprint) {
    xr_sha256_update(ctx, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static void hash_type_layout(XrSHA256Context *ctx, XrTargetTypeLayout layout) {
    hash_u64(ctx, layout.size);
    hash_u64(ctx, layout.align);
}

static bool type_layout_equal(XrTargetTypeLayout left, XrTargetTypeLayout right) {
    return left.size == right.size && left.align == right.align;
}

static bool data_layout_equal(const XrTargetDataLayout *left, const XrTargetDataLayout *right) {
#define XR_LAYOUT_FIELD_EQUAL(name) type_layout_equal(left->name, right->name)
    return XR_LAYOUT_FIELD_EQUAL(i8) && XR_LAYOUT_FIELD_EQUAL(u8) && XR_LAYOUT_FIELD_EQUAL(i16) &&
           XR_LAYOUT_FIELD_EQUAL(u16) && XR_LAYOUT_FIELD_EQUAL(i32) && XR_LAYOUT_FIELD_EQUAL(u32) &&
           XR_LAYOUT_FIELD_EQUAL(i64) && XR_LAYOUT_FIELD_EQUAL(u64) && XR_LAYOUT_FIELD_EQUAL(f32) &&
           XR_LAYOUT_FIELD_EQUAL(f64) && XR_LAYOUT_FIELD_EQUAL(boolean) &&
           XR_LAYOUT_FIELD_EQUAL(pointer) && XR_LAYOUT_FIELD_EQUAL(isize) &&
           XR_LAYOUT_FIELD_EQUAL(usize) && XR_LAYOUT_FIELD_EQUAL(xr_value) &&
           left->endian == right->endian && left->abi_id == right->abi_id &&
           left->stable_hash == right->stable_hash;
#undef XR_LAYOUT_FIELD_EQUAL
}

static bool machine_facts_equal(const XrTargetMachineFacts *left,
                                const XrTargetMachineFacts *right) {
    return left->architecture == right->architecture &&
           left->operating_system == right->operating_system &&
           left->environment == right->environment && left->native_abi == right->native_abi &&
           left->runtime_profile == right->runtime_profile &&
           memcmp(left->reserved8, right->reserved8, sizeof(left->reserved8)) == 0 &&
           data_layout_equal(&left->data_layout, &right->data_layout) &&
           left->atomic_width_mask == right->atomic_width_mask &&
           left->atomic_order_mask == right->atomic_order_mask &&
           left->float_feature_mask == right->float_feature_mask &&
           left->vector_feature_mask == right->vector_feature_mask &&
           left->maximum_vector_bits == right->maximum_vector_bits &&
           left->reserved16 == right->reserved16;
}

static bool profile_facts_equal(const XrTargetProfileDraft *left,
                                const XrTargetProfileDraft *right) {
    return left->schema_version == right->schema_version &&
           machine_facts_equal(&left->machine, &right->machine) &&
           left->provider_mask == right->provider_mask &&
           xr_fingerprint_equal(left->provider_set_fingerprint, right->provider_set_fingerprint) &&
           xr_fingerprint_equal(left->object_header_fingerprint,
                                right->object_header_fingerprint) &&
           xr_fingerprint_equal(left->runtime_abi_fingerprint, right->runtime_abi_fingerprint) &&
           xr_fingerprint_equal(left->string_literal.fingerprint,
                                right->string_literal.fingerprint);
}

static void hash_machine_facts(XrSHA256Context *ctx, const XrTargetMachineFacts *machine) {
    hash_u64(ctx, machine->architecture);
    hash_u64(ctx, machine->operating_system);
    hash_u64(ctx, machine->environment);
    hash_u64(ctx, machine->native_abi);
    hash_u64(ctx, machine->data_layout.endian);
    hash_u64(ctx, machine->data_layout.abi_id);
    hash_u64(ctx, machine->data_layout.stable_hash);
    hash_type_layout(ctx, machine->data_layout.i8);
    hash_type_layout(ctx, machine->data_layout.u8);
    hash_type_layout(ctx, machine->data_layout.i16);
    hash_type_layout(ctx, machine->data_layout.u16);
    hash_type_layout(ctx, machine->data_layout.i32);
    hash_type_layout(ctx, machine->data_layout.u32);
    hash_type_layout(ctx, machine->data_layout.i64);
    hash_type_layout(ctx, machine->data_layout.u64);
    hash_type_layout(ctx, machine->data_layout.f32);
    hash_type_layout(ctx, machine->data_layout.f64);
    hash_type_layout(ctx, machine->data_layout.boolean);
    hash_type_layout(ctx, machine->data_layout.pointer);
    hash_type_layout(ctx, machine->data_layout.isize);
    hash_type_layout(ctx, machine->data_layout.usize);
    hash_type_layout(ctx, machine->data_layout.xr_value);
    hash_u64(ctx, machine->atomic_width_mask);
    hash_u64(ctx, machine->atomic_order_mask);
    hash_u64(ctx, machine->float_feature_mask);
    hash_u64(ctx, machine->vector_feature_mask);
    hash_u64(ctx, machine->maximum_vector_bits);
}

static void finish_domain_hash(const char *domain, const XrTargetMachineFacts *machine,
                               XrFingerprint *out) {
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, (const uint8_t *) domain, strlen(domain) + 1u);
    hash_machine_facts(&context, machine);
    xr_sha256_final(&context, out->bytes);
}

static XrBoundaryValueAbi boundary_value(uint16_t type_id, uint8_t representation,
                                         uint8_t ownership, XrTargetTypeLayout layout) {
    XrBoundaryValueAbi value = {
        .type_id = type_id,
        .representation = representation,
        .ownership = ownership,
        .size = (uint16_t) layout.size,
        .alignment = (uint16_t) layout.align,
    };
    return value;
}

static void compute_boundary_abi(const XrTargetProfileDraft *facts, XrBoundaryAbi *boundary) {
    static const uint8_t domain[] = "xray-boundary-abi-v1\0";
    const XrTargetDataLayout *layout = &facts->machine.data_layout;
    memset(boundary, 0, sizeof(*boundary));
    boundary->schema_version = XR_BOUNDARY_ABI_SCHEMA_VERSION;
    boundary->target_endian = (uint8_t) layout->endian;
    boundary->pointer_size = (uint8_t) layout->pointer.size;
    boundary->pointer_alignment = (uint8_t) layout->pointer.align;
    boundary->value_count = XR_BOUNDARY_ABI_VALUE_COUNT;
    boundary->call_convention = XR_BOUNDARY_CALL_CANONICAL;
    boundary->error_model = XR_BOUNDARY_ERROR_TYPED_CODE;
    boundary->values[0] = boundary_value(XR_CORE_TYPE_VOID, XR_BOUNDARY_VALUE_VOID,
                                         XR_BOUNDARY_OWNERSHIP_NONE, (XrTargetTypeLayout) {0});
    boundary->values[1] = boundary_value(XR_CORE_TYPE_BOOL, XR_BOUNDARY_VALUE_CANONICAL_BOOL,
                                         XR_BOUNDARY_OWNERSHIP_COPY, layout->boolean);
    boundary->values[2] =
        boundary_value(XR_CORE_TYPE_I64, XR_BOUNDARY_VALUE_TWOS_COMPLEMENT_INTEGER,
                       XR_BOUNDARY_OWNERSHIP_COPY, layout->i64);
    boundary->values[3] = boundary_value(XR_CORE_TYPE_U32, XR_BOUNDARY_VALUE_UNSIGNED_INTEGER,
                                         XR_BOUNDARY_OWNERSHIP_COPY, layout->u32);
    boundary->values[4] = boundary_value(XR_CORE_TYPE_ERROR, XR_BOUNDARY_VALUE_TYPED_ERROR_CODE,
                                         XR_BOUNDARY_OWNERSHIP_COPY, layout->u32);
    boundary->object_header_id = facts->object_header_fingerprint;
    boundary->string_object_id = facts->string_literal.fingerprint;

    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u64(&context, boundary->schema_version);
    hash_u64(&context, boundary->target_endian);
    hash_u64(&context, boundary->pointer_size);
    hash_u64(&context, boundary->pointer_alignment);
    hash_u64(&context, boundary->value_count);
    hash_u64(&context, boundary->call_convention);
    hash_u64(&context, boundary->error_model);
    hash_u64(&context, boundary->coroutine_model);
    for (uint32_t index = 0; index < boundary->value_count; ++index) {
        const XrBoundaryValueAbi *value = &boundary->values[index];
        hash_u64(&context, value->type_id);
        hash_u64(&context, value->representation);
        hash_u64(&context, value->ownership);
        hash_u64(&context, value->size);
        hash_u64(&context, value->alignment);
    }
    hash_fingerprint(&context, boundary->object_header_id);
    hash_fingerprint(&context, boundary->string_object_id);
    xr_sha256_final(&context, boundary->id.bytes);
}

static void compute_runtime_kernel(const XrTargetProfileDraft *facts,
                                   XrRuntimeKernelContract *kernel) {
    static const uint8_t domain[] = "xray-runtime-kernel-v1\0";
    memset(kernel, 0, sizeof(*kernel));
    kernel->schema_version = XR_RUNTIME_KERNEL_SCHEMA_VERSION;
    kernel->runtime_profile = facts->machine.runtime_profile;
    kernel->rc_policy = XR_RUNTIME_KERNEL_POLICY_CONTRACTUAL;
    kernel->weak_policy = XR_RUNTIME_KERNEL_POLICY_CONTRACTUAL;
    kernel->generation_protocol = 1u;
    kernel->panic_policy = XR_RUNTIME_KERNEL_POLICY_CONTRACTUAL;
    kernel->oom_policy = XR_RUNTIME_KERNEL_POLICY_CONTRACTUAL;
    kernel->runtime_abi_id = facts->runtime_abi_fingerprint;

    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u64(&context, kernel->schema_version);
    hash_u64(&context, kernel->runtime_profile);
    hash_u64(&context, kernel->rc_policy);
    hash_u64(&context, kernel->weak_policy);
    hash_u64(&context, kernel->generation_protocol);
    hash_u64(&context, kernel->panic_policy);
    hash_u64(&context, kernel->oom_policy);
    hash_u64(&context, kernel->scheduler_hook_mask);
    hash_fingerprint(&context, kernel->runtime_abi_id);
    xr_sha256_final(&context, kernel->id.bytes);
}

void xr_target_profile_compute_partitions(const XrTargetProfileDraft *facts,
                                          XrTargetSemanticsId *target_semantics_id,
                                          XrBoundaryAbi *boundary_abi,
                                          XrRuntimeKernelContract *runtime_kernel) {
    finish_domain_hash("xray-target-semantics-v1", &facts->machine, target_semantics_id);
    compute_boundary_abi(facts, boundary_abi);
    compute_runtime_kernel(facts, runtime_kernel);
}

void xr_target_profile_compute_fingerprint(const XrTargetProfileDraft *facts, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-target-profile-v4\0";
    XrTargetSemanticsId target_semantics_id;
    XrBoundaryAbi boundary;
    XrRuntimeKernelContract kernel;
    xr_target_profile_compute_partitions(facts, &target_semantics_id, &boundary, &kernel);

    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u64(&context, facts->schema_version);
    hash_fingerprint(&context, target_semantics_id);
    hash_fingerprint(&context, boundary.id);
    hash_fingerprint(&context, kernel.id);
    hash_u64(&context, facts->provider_mask);
    hash_fingerprint(&context, facts->provider_set_fingerprint);
    xr_sha256_final(&context, out->bytes);
}

bool xr_target_profile_build(const XrTargetProfileBuildInput *input, XrTargetProfile **out,
                             char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!input || !out || !input->runtime_abi || !input->object_header_materialization ||
        !input->string_contract || !input->providers) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "structured target profile input is missing");
        return false;
    }
    if (input->provider_count == 0 || input->provider_count > XR_RUNTIME_ABI_MAX_PROVIDERS) {
        set_error(error, error_size, "XR_TARGET_1000", "target provider contract count is invalid");
        return false;
    }
    for (size_t i = 0; i < input->provider_count; i++) {
        if (input->providers[i].runtime_profile != input->machine.runtime_profile) {
            set_error(error, error_size, "XR_TARGET_1000",
                      "provider runtime profile does not match machine facts");
            return false;
        }
    }
    if (input->runtime_abi->target_endian != input->machine.data_layout.endian ||
        input->runtime_abi->pointer_width != input->machine.data_layout.pointer.size) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "runtime ABI does not match exact machine layout facts");
        return false;
    }

    XrRuntimeObjectHeaderAbi materialized_header;
    XrRuntimeAbiStatus status = xr_runtime_object_header_abi_materialize(
        input->object_header_materialization, &materialized_header);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size, "object-header materialization facts are invalid",
                          status);
        return false;
    }
    XrFingerprint object_header_fingerprint;
    status =
        xr_runtime_object_header_abi_fingerprint(&materialized_header, &object_header_fingerprint);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size, "materialized object-header ABI is invalid", status);
        return false;
    }
    XrFingerprint contract_header_fingerprint;
    status = xr_runtime_object_header_abi_fingerprint(&input->runtime_abi->object_header,
                                                      &contract_header_fingerprint);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size, "runtime object-header ABI is invalid", status);
        return false;
    }
    if (!xr_fingerprint_equal(object_header_fingerprint, contract_header_fingerprint)) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "runtime and materialized object-header ABIs differ");
        return false;
    }

    XrFingerprint runtime_abi_fingerprint;
    status = xr_runtime_abi_contract_fingerprint(input->runtime_abi, &runtime_abi_fingerprint);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size, "runtime ABI contract is invalid", status);
        return false;
    }
    status = xr_runtime_string_object_contract_verify(input->string_contract);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size, "runtime String materialization contract is invalid",
                          status);
        return false;
    }
    uint64_t provider_mask = 0;
    XrFingerprint provider_set_fingerprint;
    status = xr_target_provider_set_fingerprint(input->providers, input->provider_count,
                                                &provider_mask, &provider_set_fingerprint);
    if (status != XR_RUNTIME_ABI_OK) {
        set_runtime_error(error, error_size, "target provider contracts are invalid", status);
        return false;
    }
    XrTargetProfileDraft draft = {
        .schema_version = XR_TARGET_PROFILE_SCHEMA_VERSION,
        .machine = input->machine,
        .provider_mask = provider_mask,
        .provider_set_fingerprint = provider_set_fingerprint,
        .object_header_fingerprint = object_header_fingerprint,
        .runtime_abi_fingerprint = runtime_abi_fingerprint,
        .string_literal = input->string_contract->literal_view,
    };
    XrTargetProfile *profile = NULL;
    if (!xr_target_profile_freeze(&draft, &profile, error, error_size))
        return false;
    profile->providers = xr_calloc(input->provider_count, sizeof(XrTargetProviderContract));
    if (!profile->providers) {
        xr_target_profile_free(profile);
        set_error(error, error_size, "XR_EXEC_5003", "provider contract allocation failed");
        return false;
    }
    memcpy(profile->providers, input->providers,
           input->provider_count * sizeof(XrTargetProviderContract));
    profile->provider_count = input->provider_count;
    profile->provider_contracts_materialized = true;
    if (!xr_target_profile_verify(profile, error, error_size)) {
        xr_target_profile_free(profile);
        return false;
    }
    *out = profile;
    return true;
}

bool xr_target_profile_freeze(const XrTargetProfileDraft *draft, XrTargetProfile **out, char *error,
                              size_t error_size) {
    if (out)
        *out = NULL;
    if (!draft || !out) {
        set_error(error, error_size, "XR_TARGET_1000", "target profile input is missing");
        return false;
    }
    XrTargetProfile *profile = (XrTargetProfile *) xr_calloc(1, sizeof(*profile));
    if (!profile) {
        set_error(error, error_size, "XR_EXEC_5003", "target profile allocation failed");
        return false;
    }
    atomic_init(&profile->references, 1);
    memcpy(&profile->facts, draft, sizeof(*draft));
    xr_target_profile_compute_partitions(&profile->facts, &profile->target_semantics_id,
                                         &profile->boundary_abi, &profile->runtime_kernel);
    xr_target_profile_compute_fingerprint(&profile->facts, &profile->fingerprint);
    profile->frozen = true;
    if (!xr_target_profile_verify(profile, error, error_size)) {
        xr_target_profile_free(profile);
        return false;
    }
    *out = profile;
    return true;
}

bool xr_target_profile_require_exact(const XrTargetProfile *expected, const XrTargetProfile *actual,
                                     char *error, size_t error_size) {
    if (!expected || !actual || !xr_target_profile_verify(expected, NULL, 0) ||
        !xr_target_profile_verify(actual, NULL, 0)) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "exact target profile comparison requires verified profiles");
        return false;
    }
    if (!profile_facts_equal(&expected->facts, &actual->facts) ||
        !xr_fingerprint_equal(expected->fingerprint, actual->fingerprint)) {
        set_error(error, error_size, "XR_TARGET_1000", "exact target profile fingerprint mismatch");
        return false;
    }
    return true;
}

XrTargetProfile *xr_target_profile_retain(const XrTargetProfile *profile) {
    XrTargetProfile *retained = (XrTargetProfile *) profile;
    if (retained)
        atomic_fetch_add_explicit(&retained->references, 1, memory_order_relaxed);
    return retained;
}

void xr_target_profile_free(XrTargetProfile *profile) {
    if (!profile)
        return;
    if (atomic_fetch_sub_explicit(&profile->references, 1, memory_order_acq_rel) != 1)
        return;
    xr_free(profile->providers);
    xr_free(profile);
}

bool xr_target_profile_is_frozen(const XrTargetProfile *profile) {
    return profile && profile->frozen;
}

XrFingerprint xr_target_profile_fingerprint(const XrTargetProfile *profile) {
    XrFingerprint zero = {{0}};
    return profile ? profile->fingerprint : zero;
}

const XrTargetProfileDraft *xr_target_profile_facts(const XrTargetProfile *profile) {
    return profile ? &profile->facts : NULL;
}

const XrTargetMachineFacts *xr_target_profile_machine_facts(const XrTargetProfile *profile) {
    return profile ? &profile->facts.machine : NULL;
}

XrTargetSemanticsId xr_target_profile_target_semantics_id(const XrTargetProfile *profile) {
    XrTargetSemanticsId zero = {{0}};
    return profile ? profile->target_semantics_id : zero;
}

const XrBoundaryAbi *xr_target_profile_boundary_abi(const XrTargetProfile *profile) {
    return profile ? &profile->boundary_abi : NULL;
}

const XrRuntimeKernelContract *xr_target_profile_runtime_kernel(const XrTargetProfile *profile) {
    return profile ? &profile->runtime_kernel : NULL;
}

XrProviderContractSetId xr_target_profile_provider_contract_set_id(const XrTargetProfile *profile) {
    XrProviderContractSetId zero = {{0}};
    return profile ? profile->facts.provider_set_fingerprint : zero;
}

size_t xr_target_profile_provider_count(const XrTargetProfile *profile) {
    return profile && profile->provider_contracts_materialized ? profile->provider_count : 0u;
}

const XrTargetProviderContract *xr_target_profile_provider(const XrTargetProfile *profile,
                                                           size_t index) {
    if (!profile || !profile->provider_contracts_materialized || index >= profile->provider_count)
        return NULL;
    return &profile->providers[index];
}
