/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_verify.c - Independent structural TargetPlan verifier
 *
 * KEY CONCEPT:
 *   Verification consumes only the frozen semantic and target schemas. It
 *   does not call an AOT planner, C emitter, or VM dispatcher, so malformed
 *   plans fail before any backend can reinterpret missing facts.
 */

#include "xr_target_verify.h"
#include "xr_target_plan_internal.h"
#include "xr_target_profile_internal.h"
#include "../semantic/xr_semantic_verify.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>

static bool report(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
    return false;
}

static bool is_power_of_two(uint32_t value) {
    return value && (value & (value - 1u)) == 0;
}

static bool range_valid(uint32_t begin, uint32_t count, uint32_t limit) {
    return begin <= limit && count <= limit - begin;
}

static bool checked_u32_add(uint32_t left, uint32_t right, uint32_t *out) {
    if (left > UINT32_MAX - right)
        return false;
    *out = left + right;
    return true;
}

static bool fingerprint_is_zero(XrFingerprint fingerprint) {
    uint8_t combined = 0;
    for (uint32_t i = 0; i < sizeof(fingerprint.bytes); i++)
        combined |= fingerprint.bytes[i];
    return combined == 0;
}

static bool stable_id_is_zero(XrStableId id) {
    uint8_t combined = 0;
    for (uint32_t i = 0; i < sizeof(id.bytes); i++)
        combined |= id.bytes[i];
    return combined == 0;
}

static bool profile_identity_is_consistent(const XrTargetProfileDraft *facts) {
    switch (facts->operating_system) {
        case XR_TARGET_OS_WINDOWS:
            return facts->environment == XR_TARGET_ENV_MSVC &&
                   ((facts->architecture == XR_TARGET_ARCH_X86_64 &&
                     facts->native_abi == XR_TARGET_ABI_WIN64_X86_64) ||
                    (facts->architecture == XR_TARGET_ARCH_AARCH64 &&
                     facts->native_abi == XR_TARGET_ABI_WIN64_AARCH64));
        case XR_TARGET_OS_LINUX:
            if (facts->environment != XR_TARGET_ENV_GNU &&
                facts->environment != XR_TARGET_ENV_MUSL)
                return false;
            if (facts->architecture == XR_TARGET_ARCH_X86_64)
                return facts->native_abi == XR_TARGET_ABI_SYSV_X86_64;
            if (facts->architecture == XR_TARGET_ARCH_AARCH64)
                return facts->native_abi == XR_TARGET_ABI_AAPCS64;
            if (facts->architecture == XR_TARGET_ARCH_POWERPC64)
                return facts->native_abi == XR_TARGET_ABI_PPC64_ELFV2;
            if (facts->architecture == XR_TARGET_ARCH_LOONGARCH64)
                return facts->native_abi == XR_TARGET_ABI_LOONGARCH_LP64D;
            return false;
        case XR_TARGET_OS_MACOS:
            return facts->environment == XR_TARGET_ENV_DARWIN &&
                   ((facts->architecture == XR_TARGET_ARCH_X86_64 &&
                     facts->native_abi == XR_TARGET_ABI_DARWIN_X86_64) ||
                    (facts->architecture == XR_TARGET_ARCH_AARCH64 &&
                     facts->native_abi == XR_TARGET_ABI_DARWIN_AARCH64));
        case XR_TARGET_OS_WASI:
            return facts->architecture == XR_TARGET_ARCH_WASM32 &&
                   facts->environment == XR_TARGET_ENV_WASI &&
                   facts->native_abi == XR_TARGET_ABI_WASM;
        case XR_TARGET_OS_FREESTANDING:
            if (facts->environment != XR_TARGET_ENV_FREESTANDING)
                return false;
            switch (facts->architecture) {
                case XR_TARGET_ARCH_X86_64:
                    return facts->native_abi == XR_TARGET_ABI_SYSV_X86_64;
                case XR_TARGET_ARCH_AARCH64:
                    return facts->native_abi == XR_TARGET_ABI_AAPCS64;
                case XR_TARGET_ARCH_POWERPC64:
                    return facts->native_abi == XR_TARGET_ABI_PPC64_ELFV2;
                case XR_TARGET_ARCH_LOONGARCH64:
                    return facts->native_abi == XR_TARGET_ABI_LOONGARCH_LP64D;
                case XR_TARGET_ARCH_WASM32:
                    return facts->native_abi == XR_TARGET_ABI_WASM;
                default:
                    return false;
            }
        default:
            return false;
    }
}

static bool profile_machine_features_are_consistent(const XrTargetProfileDraft *facts) {
    uint64_t vectors = facts->vector_feature_mask;
    uint64_t allowed = 0;
    switch (facts->architecture) {
        case XR_TARGET_ARCH_X86_64:
            allowed = XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2 | XR_TARGET_VECTOR_AVX512;
            break;
        case XR_TARGET_ARCH_AARCH64:
            allowed = XR_TARGET_VECTOR_NEON | XR_TARGET_VECTOR_SVE;
            break;
        case XR_TARGET_ARCH_POWERPC64:
            allowed = XR_TARGET_VECTOR_VSX;
            break;
        case XR_TARGET_ARCH_LOONGARCH64:
            allowed = XR_TARGET_VECTOR_LSX;
            break;
        case XR_TARGET_ARCH_WASM32:
            allowed = XR_TARGET_VECTOR_WASM128;
            break;
        default:
            return false;
    }
    uint32_t expected_pointer_size =
        facts->architecture == XR_TARGET_ARCH_WASM32 ? 4u : 8u;
    bool requires_little_endian = facts->architecture == XR_TARGET_ARCH_X86_64 ||
                                  facts->architecture == XR_TARGET_ARCH_LOONGARCH64 ||
                                  facts->architecture == XR_TARGET_ARCH_WASM32 ||
                                  facts->operating_system == XR_TARGET_OS_WINDOWS ||
                                  facts->operating_system == XR_TARGET_OS_MACOS;
    uint16_t exact_vector_bits = 0;
    switch (facts->architecture) {
        case XR_TARGET_ARCH_X86_64:
            if (vectors == XR_TARGET_VECTOR_SSE2)
                exact_vector_bits = 128;
            else if (vectors == (XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2))
                exact_vector_bits = 256;
            else if (vectors == (XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2 |
                                 XR_TARGET_VECTOR_AVX512))
                exact_vector_bits = 512;
            else if (vectors != 0)
                return false;
            break;
        case XR_TARGET_ARCH_AARCH64:
            if (vectors == XR_TARGET_VECTOR_NEON)
                exact_vector_bits = 128;
            else if (vectors == (XR_TARGET_VECTOR_NEON | XR_TARGET_VECTOR_SVE)) {
                if (facts->maximum_vector_bits < 128 || facts->maximum_vector_bits > 2048 ||
                    !is_power_of_two(facts->maximum_vector_bits))
                    return false;
                exact_vector_bits = facts->maximum_vector_bits;
            } else if (vectors != 0) {
                return false;
            }
            break;
        case XR_TARGET_ARCH_POWERPC64:
            if (vectors == XR_TARGET_VECTOR_VSX)
                exact_vector_bits = 128;
            else if (vectors != 0)
                return false;
            break;
        case XR_TARGET_ARCH_LOONGARCH64:
            if (vectors == XR_TARGET_VECTOR_LSX)
                exact_vector_bits = 128;
            else if (vectors != 0)
                return false;
            break;
        case XR_TARGET_ARCH_WASM32:
            if (vectors == XR_TARGET_VECTOR_WASM128)
                exact_vector_bits = 128;
            else if (vectors != 0)
                return false;
            break;
        default:
            return false;
    }
    return (vectors & ~allowed) == 0 && facts->maximum_vector_bits == exact_vector_bits &&
           facts->data_layout.pointer.size == expected_pointer_size &&
           (!requires_little_endian ||
            facts->data_layout.endian == XR_TARGET_ENDIAN_LITTLE);
}

bool xr_target_profile_verify(const XrTargetProfile *profile, char *error, size_t error_size) {
    if (!profile || !profile->frozen)
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile is not frozen");
    const XrTargetProfileDraft *facts = &profile->facts;
    if (facts->schema_version != XR_TARGET_PLAN_SCHEMA_VERSION ||
        facts->architecture <= XR_TARGET_ARCH_NONE || facts->architecture >= XR_TARGET_ARCH_COUNT ||
        facts->operating_system <= XR_TARGET_OS_NONE ||
        facts->operating_system >= XR_TARGET_OS_COUNT ||
        facts->environment <= XR_TARGET_ENV_NONE || facts->environment >= XR_TARGET_ENV_COUNT ||
        facts->native_abi <= XR_TARGET_ABI_NONE || facts->native_abi >= XR_TARGET_ABI_COUNT ||
        facts->runtime_profile < XR_TARGET_RUNTIME_HOSTED ||
        facts->runtime_profile > XR_TARGET_RUNTIME_FREESTANDING || facts->reserved8[0] != 0 ||
        facts->reserved8[1] != 0 || facts->reserved8[2] != 0 || facts->reserved16 != 0)
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile contains an unsupported exact identity");
    if (!xr_target_data_layout_validate(&facts->data_layout))
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile data layout is invalid");
    const uint64_t atomic_width_mask = XR_TARGET_ATOMIC_WIDTH_8 | XR_TARGET_ATOMIC_WIDTH_16 |
                                       XR_TARGET_ATOMIC_WIDTH_32 | XR_TARGET_ATOMIC_WIDTH_64 |
                                       XR_TARGET_ATOMIC_WIDTH_128;
    const uint64_t atomic_order_mask = XR_TARGET_ATOMIC_RELAXED | XR_TARGET_ATOMIC_ACQUIRE |
                                       XR_TARGET_ATOMIC_RELEASE | XR_TARGET_ATOMIC_ACQ_REL |
                                       XR_TARGET_ATOMIC_SEQ_CST;
    const uint64_t float_mask = XR_TARGET_FLOAT_IEEE754 | XR_TARGET_FLOAT_STRICT |
                                XR_TARGET_FLOAT_FAST | XR_TARGET_FLOAT_FMA;
    const uint64_t vector_mask = XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2 |
                                 XR_TARGET_VECTOR_AVX512 | XR_TARGET_VECTOR_NEON |
                                 XR_TARGET_VECTOR_SVE | XR_TARGET_VECTOR_VSX |
                                 XR_TARGET_VECTOR_LSX | XR_TARGET_VECTOR_WASM128;
    const uint64_t provider_mask =
        ((UINT64_C(1) << XR_TARGET_PROVIDER_COUNT) - 1u) & ~UINT64_C(1);
    if ((facts->atomic_width_mask & ~atomic_width_mask) != 0 ||
        (facts->atomic_order_mask & ~atomic_order_mask) != 0 ||
        (facts->float_feature_mask & ~float_mask) != 0 ||
        (facts->vector_feature_mask & ~vector_mask) != 0 ||
        (facts->provider_mask & ~provider_mask) != 0 ||
        (facts->provider_mask & (UINT64_C(1) << XR_TARGET_PROVIDER_ALLOCATOR)) == 0 ||
        (facts->provider_mask & (UINT64_C(1) << XR_TARGET_PROVIDER_PANIC)) == 0 ||
        (facts->float_feature_mask & XR_TARGET_FLOAT_IEEE754) == 0 ||
        ((facts->float_feature_mask & XR_TARGET_FLOAT_STRICT) != 0 &&
         (facts->float_feature_mask & XR_TARGET_FLOAT_FAST) != 0) ||
        (facts->vector_feature_mask == 0 && facts->maximum_vector_bits != 0) ||
        (facts->vector_feature_mask != 0 &&
         (!is_power_of_two(facts->maximum_vector_bits) || facts->maximum_vector_bits < 128u ||
          facts->maximum_vector_bits > 2048u)) ||
        fingerprint_is_zero(facts->provider_set_fingerprint) ||
        fingerprint_is_zero(facts->object_header_fingerprint) ||
        fingerprint_is_zero(facts->runtime_abi_fingerprint))
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile runtime facts are incomplete");
    bool freestanding = facts->runtime_profile == XR_TARGET_RUNTIME_FREESTANDING;
    if (freestanding != (facts->operating_system == XR_TARGET_OS_FREESTANDING) ||
        freestanding != (facts->environment == XR_TARGET_ENV_FREESTANDING) ||
        !profile_identity_is_consistent(facts) ||
        !profile_machine_features_are_consistent(facts))
        return report(error, error_size, "XR_TARGET_1000",
                      "runtime profile disagrees with the target environment");
    XrFingerprint actual;
    xr_target_profile_compute_fingerprint(facts, &actual);
    if (!xr_fingerprint_equal(actual, profile->fingerprint))
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile fingerprint changed after freeze");
    return true;
}

static bool verify_resource_budgets(const XrTargetPlan *plan, char *error, size_t error_size) {
    if (plan->machine_reps_count > 256u || plan->extents_count > 1000000u ||
        plan->value_reps_count > 40000000u ||
        plan->layouts_count > 1000000u || plan->fields_count > 16000000u ||
        plan->storage_count > 4000000u || plan->allocations_count > 10000000u ||
        plan->extent_operands_count > 40000000u || plan->functions_count > 100000u ||
        plan->slots_count > 16000000u || plan->calls_count > 10000000u ||
        plan->call_arguments_count > 40000000u || plan->root_maps_count > 10000000u ||
        plan->root_slots_count > 40000000u || plan->cleanups_count > 40000000u ||
        plan->adapters_count > 1000000u || plan->capabilities_count > 65536u ||
        plan->coroutines_count > 10000000u)
        return report(error, error_size, "XR_EXEC_5003", "TargetPlan exceeds hard budgets");
    size_t total = sizeof(*plan);
#define XR_ADD_TARGET_BYTES(name)                                                                 \
    do {                                                                                          \
        if (plan->name##_count > (SIZE_MAX - total) / sizeof(*plan->name))                        \
            return report(error, error_size, "XR_EXEC_5003", "TargetPlan byte budget overflow"); \
        total += (size_t) plan->name##_count * sizeof(*plan->name);                               \
    } while (0)
    XR_ADD_TARGET_BYTES(machine_reps);
    XR_ADD_TARGET_BYTES(value_reps);
    XR_ADD_TARGET_BYTES(extents);
    XR_ADD_TARGET_BYTES(layouts);
    XR_ADD_TARGET_BYTES(fields);
    XR_ADD_TARGET_BYTES(storage);
    XR_ADD_TARGET_BYTES(allocations);
    XR_ADD_TARGET_BYTES(extent_operands);
    XR_ADD_TARGET_BYTES(functions);
    XR_ADD_TARGET_BYTES(slots);
    XR_ADD_TARGET_BYTES(calls);
    XR_ADD_TARGET_BYTES(call_arguments);
    XR_ADD_TARGET_BYTES(root_maps);
    XR_ADD_TARGET_BYTES(root_slots);
    XR_ADD_TARGET_BYTES(cleanups);
    XR_ADD_TARGET_BYTES(adapters);
    XR_ADD_TARGET_BYTES(capabilities);
    XR_ADD_TARGET_BYTES(coroutines);
#undef XR_ADD_TARGET_BYTES
    if (total > (size_t) UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003", "TargetPlan exceeds total byte budget");
#define XR_REQUIRE_TARGET_TABLE(name)                                                              \
    if (plan->name##_count && !plan->name)                                                         \
        return report(error, error_size, "XR_EXEC_5003", "TargetPlan table storage is missing")
    XR_REQUIRE_TARGET_TABLE(machine_reps);
    XR_REQUIRE_TARGET_TABLE(value_reps);
    XR_REQUIRE_TARGET_TABLE(extents);
    XR_REQUIRE_TARGET_TABLE(layouts);
    XR_REQUIRE_TARGET_TABLE(fields);
    XR_REQUIRE_TARGET_TABLE(storage);
    XR_REQUIRE_TARGET_TABLE(allocations);
    XR_REQUIRE_TARGET_TABLE(extent_operands);
    XR_REQUIRE_TARGET_TABLE(functions);
    XR_REQUIRE_TARGET_TABLE(slots);
    XR_REQUIRE_TARGET_TABLE(calls);
    XR_REQUIRE_TARGET_TABLE(call_arguments);
    XR_REQUIRE_TARGET_TABLE(root_maps);
    XR_REQUIRE_TARGET_TABLE(root_slots);
    XR_REQUIRE_TARGET_TABLE(cleanups);
    XR_REQUIRE_TARGET_TABLE(adapters);
    XR_REQUIRE_TARGET_TABLE(capabilities);
    XR_REQUIRE_TARGET_TABLE(coroutines);
#undef XR_REQUIRE_TARGET_TABLE
    return true;
}

static bool conversion_mask_in_range(const XrTargetMachineRepRecord *rep, uint32_t count) {
    for (uint32_t word = 0; word < 4; word++) {
        uint32_t begin = word * 64u;
        uint64_t allowed = UINT64_MAX;
        if (begin >= count)
            allowed = 0;
        else if (count - begin < 64u)
            allowed = (UINT64_C(1) << (count - begin)) - 1u;
        if ((rep->legal_conversion_mask[word] & ~allowed) != 0)
            return false;
    }
    return true;
}

static bool machine_reps_are_storage_compatible(const XrTargetMachineRepRecord *from,
                                                 const XrTargetMachineRepRecord *to) {
    return from->kind == to->kind && from->signedness == to->signedness &&
           from->root_kind == to->root_kind && from->ownership == to->ownership &&
           from->null_encoding == to->null_encoding && from->register_bits == to->register_bits &&
           from->memory_size == to->memory_size && from->memory_align == to->memory_align &&
           from->detail == to->detail && from->lane_count == to->lane_count;
}

static bool conversion_mask_is_independently_derived(const XrTargetPlan *plan, uint32_t index) {
    const XrTargetMachineRepRecord *from = &plan->machine_reps[index];
    for (uint32_t to = 0; to < plan->machine_reps_count; to++) {
        bool encoded =
            (from->legal_conversion_mask[to / 64u] & (UINT64_C(1) << (to % 64u))) != 0;
        bool expected = to != index &&
                        machine_reps_are_storage_compatible(from, &plan->machine_reps[to]);
        if (encoded != expected)
            return false;
    }
    return true;
}

static bool rep_matches_layout(const XrTargetMachineRepRecord *rep,
                               const XrTargetTypeLayout *layout, uint8_t signedness) {
    return rep->register_bits == layout->size * 8u && rep->memory_size == layout->size &&
           rep->memory_align == layout->align && rep->signedness == signedness;
}

static bool scalar_rep_matches_profile(const XrTargetMachineRepRecord *rep,
                                       const XrTargetProfileDraft *profile) {
    switch (rep->kind) {
        case XR_MACHINE_REP_I1:
            return rep->register_bits == 1 &&
                   rep->memory_size == profile->data_layout.boolean.size &&
                   rep->memory_align == profile->data_layout.boolean.align &&
                   rep->signedness == XR_TARGET_SIGN_NONE;
        case XR_MACHINE_REP_I8:
            return rep_matches_layout(rep, &profile->data_layout.i8, XR_TARGET_SIGN_SIGNED);
        case XR_MACHINE_REP_U8:
            return rep_matches_layout(rep, &profile->data_layout.u8, XR_TARGET_SIGN_UNSIGNED);
        case XR_MACHINE_REP_I16:
            return rep_matches_layout(rep, &profile->data_layout.i16, XR_TARGET_SIGN_SIGNED);
        case XR_MACHINE_REP_U16:
            return rep_matches_layout(rep, &profile->data_layout.u16, XR_TARGET_SIGN_UNSIGNED);
        case XR_MACHINE_REP_I32:
            return rep_matches_layout(rep, &profile->data_layout.i32, XR_TARGET_SIGN_SIGNED);
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_RUNE:
            return rep_matches_layout(rep, &profile->data_layout.u32, XR_TARGET_SIGN_UNSIGNED);
        case XR_MACHINE_REP_I64:
            return rep_matches_layout(rep, &profile->data_layout.i64, XR_TARGET_SIGN_SIGNED);
        case XR_MACHINE_REP_U64:
            return rep_matches_layout(rep, &profile->data_layout.u64, XR_TARGET_SIGN_UNSIGNED);
        case XR_MACHINE_REP_ISIZE:
            return rep_matches_layout(rep, &profile->data_layout.isize, XR_TARGET_SIGN_SIGNED);
        case XR_MACHINE_REP_USIZE:
            return rep_matches_layout(rep, &profile->data_layout.usize, XR_TARGET_SIGN_UNSIGNED);
        case XR_MACHINE_REP_F32:
            return rep_matches_layout(rep, &profile->data_layout.f32, XR_TARGET_SIGN_NONE);
        case XR_MACHINE_REP_F64:
            return rep_matches_layout(rep, &profile->data_layout.f64, XR_TARGET_SIGN_NONE);
        default:
            return true;
    }
}

static bool rep_kind_contract_is_exact(const XrTargetPlan *plan,
                                       const XrTargetMachineRepRecord *rep) {
    const XrTargetProfileDraft *profile = xr_target_profile_facts(plan->profile);
    bool scalar = rep->kind >= XR_MACHINE_REP_I1 && rep->kind <= XR_MACHINE_REP_RUNE;
    if (scalar)
        return rep->detail == 0 && rep->lane_count == 0 && rep->root_kind == XR_TARGET_ROOT_NONE &&
               rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
               rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE;
    switch (rep->kind) {
        case XR_MACHINE_REP_OBJECT_REF:
            return rep->root_kind == XR_TARGET_ROOT_OBJECT && rep->lane_count == 0 &&
                   rep->signedness == XR_TARGET_SIGN_NONE;
        case XR_MACHINE_REP_RAW_PTR:
        case XR_MACHINE_REP_CODE_REF:
            return rep->detail == 0 && rep->lane_count == 0 &&
                   rep->root_kind == XR_TARGET_ROOT_NONE &&
                   rep->signedness == XR_TARGET_SIGN_NONE;
        case XR_MACHINE_REP_DYN_VALUE:
            return rep->detail == 0 && rep->lane_count == 0 &&
                   rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
                   rep->signedness == XR_TARGET_SIGN_NONE;
        case XR_MACHINE_REP_AGGREGATE:
        case XR_MACHINE_REP_VIEW: {
            if (rep->detail >= plan->layouts_count || rep->lane_count != 0 ||
                rep->signedness != XR_TARGET_SIGN_NONE)
                return false;
            const XrTargetLayoutRecord *layout = &plan->layouts[rep->detail];
            uint8_t expected_kind = rep->kind == XR_MACHINE_REP_VIEW ? XR_TARGET_LAYOUT_VIEW
                                                                     : XR_TARGET_LAYOUT_AGGREGATE;
            return layout->kind == expected_kind && rep->memory_size == layout->fixed_prefix_size &&
                   rep->memory_align == layout->align &&
                   rep->register_bits == layout->fixed_prefix_size * 8u;
        }
        case XR_MACHINE_REP_VECTOR: {
            if (rep->detail >= plan->machine_reps_count || rep->detail == rep->id ||
                rep->lane_count < 2)
                return false;
            const XrTargetMachineRepRecord *lane = &plan->machine_reps[rep->detail];
            uint64_t size = (uint64_t) lane->memory_size * rep->lane_count;
            bool scalar_lane = lane->kind >= XR_MACHINE_REP_I8 &&
                               lane->kind <= XR_MACHINE_REP_F64;
            return scalar_lane && lane->root_kind == XR_TARGET_ROOT_NONE && size <= UINT32_MAX &&
                   size <= UINT32_MAX / 8u && rep->memory_size == size &&
                   rep->register_bits == size * 8u &&
                   rep->register_bits <= profile->maximum_vector_bits &&
                   rep->memory_align == size && rep->memory_align >= lane->memory_align;
        }
        default:
            return false;
    }
}

static bool verify_machine_reps(const XrTargetPlan *plan, char *error, size_t error_size) {
    const XrTargetProfileDraft *profile = xr_target_profile_facts(plan->profile);
    if (!plan->machine_reps_count)
        return report(error, error_size, "XR_TARGET_1001", "machine representation table is empty");
    for (uint32_t i = 0; i < plan->machine_reps_count; i++) {
        const XrTargetMachineRepRecord *rep = &plan->machine_reps[i];
        if (rep->id != i || rep->kind >= XR_MACHINE_REP_COUNT ||
            rep->signedness > XR_TARGET_SIGN_UNSIGNED || rep->root_kind > XR_TARGET_ROOT_VIEW_OWNER ||
            rep->ownership > XR_TARGET_OWNERSHIP_SHARED ||
            rep->null_encoding > XR_TARGET_NULL_TAGGED || rep->reserved != 0 ||
            !conversion_mask_in_range(rep, plan->machine_reps_count))
            return report(error, error_size, "XR_TARGET_1001",
                          "machine representation identity or enum is invalid");
        if (rep->kind == XR_MACHINE_REP_VOID) {
            if (rep->register_bits || rep->memory_size || rep->memory_align || rep->signedness ||
                rep->root_kind || rep->ownership || rep->null_encoding || rep->detail ||
                rep->lane_count || rep->legal_conversion_mask[0] ||
                rep->legal_conversion_mask[1] || rep->legal_conversion_mask[2] ||
                rep->legal_conversion_mask[3])
                return report(error, error_size, "XR_TARGET_1001",
                              "void representation carries storage facts");
            continue;
        }
        if (!rep->register_bits || !rep->memory_size || !is_power_of_two(rep->memory_align) ||
            rep->memory_align > rep->memory_size)
            return report(error, error_size, "XR_TARGET_1001",
                          "machine representation width or alignment is invalid");
        if (!scalar_rep_matches_profile(rep, profile))
            return report(error, error_size, "XR_TARGET_1001",
                          "scalar representation disagrees with the target profile");
        if (!rep_kind_contract_is_exact(plan, rep))
            return report(error, error_size, "XR_TARGET_1001",
                          "representation kind carries an invalid detail or lifecycle contract");
        if (rep->kind == XR_MACHINE_REP_OBJECT_REF || rep->kind == XR_MACHINE_REP_RAW_PTR ||
            rep->kind == XR_MACHINE_REP_CODE_REF) {
            if (!rep_matches_layout(rep, &profile->data_layout.pointer, XR_TARGET_SIGN_NONE))
                return report(error, error_size, "XR_TARGET_1001",
                              "pointer representation disagrees with the target profile");
        } else if (rep->kind == XR_MACHINE_REP_DYN_VALUE &&
                   !rep_matches_layout(rep, &profile->data_layout.xr_value,
                                       XR_TARGET_SIGN_NONE)) {
            return report(error, error_size, "XR_TARGET_1001",
                          "dynamic representation disagrees with the target profile");
        }
        if (rep->kind == XR_MACHINE_REP_VECTOR) {
            if (!rep->lane_count || rep->detail >= plan->machine_reps_count || rep->detail == i)
                return report(error, error_size, "XR_TARGET_1001",
                              "vector representation has no valid lane representation");
        } else if (rep->lane_count) {
            return report(error, error_size, "XR_TARGET_1001",
                          "non-vector representation carries a lane count");
        }
        if (rep->kind == XR_MACHINE_REP_OBJECT_REF &&
            (rep->detail >= plan->layouts_count ||
             plan->layouts[rep->detail].kind != XR_TARGET_LAYOUT_OBJECT))
            return report(error, error_size, "XR_TARGET_1001",
                          "representation references an invalid layout");
    }
    for (uint32_t i = 0; i < plan->machine_reps_count; i++)
        if (!conversion_mask_is_independently_derived(plan, i))
            return report(error, error_size, "XR_TARGET_1001",
                          "conversion mask disagrees with the independently derived legal domain");
    return true;
}

static bool machine_rep_allows_conversion(const XrTargetPlan *plan, uint16_t from, uint16_t to) {
    if (from >= plan->machine_reps_count || to >= plan->machine_reps_count)
        return false;
    return machine_reps_are_storage_compatible(&plan->machine_reps[from],
                                               &plan->machine_reps[to]);
}

static int semantic_type_expected_rep(const XrSemanticTypeRecord *type, uint16_t *out_kind) {
    if ((type->flags & XR_SEM_TYPE_NULLABLE) != 0)
        return 0;
    switch (type->kind) {
        case XR_KIND_INT:
            switch (type->scalar_rep) {
                case XR_NATIVE_I8: *out_kind = XR_MACHINE_REP_I8; return 1;
                case XR_NATIVE_I16: *out_kind = XR_MACHINE_REP_I16; return 1;
                case XR_NATIVE_I32: *out_kind = XR_MACHINE_REP_I32; return 1;
                case XR_NATIVE_I64: *out_kind = XR_MACHINE_REP_I64; return 1;
                case XR_NATIVE_U8: *out_kind = XR_MACHINE_REP_U8; return 1;
                case XR_NATIVE_U16: *out_kind = XR_MACHINE_REP_U16; return 1;
                case XR_NATIVE_U32: *out_kind = XR_MACHINE_REP_U32; return 1;
                case XR_NATIVE_U64: *out_kind = XR_MACHINE_REP_U64; return 1;
                case XR_NATIVE_ISIZE: *out_kind = XR_MACHINE_REP_ISIZE; return 1;
                case XR_NATIVE_USIZE: *out_kind = XR_MACHINE_REP_USIZE; return 1;
                default: return -1;
            }
        case XR_KIND_FLOAT:
            if (type->scalar_rep == XR_NATIVE_F32) {
                *out_kind = XR_MACHINE_REP_F32;
                return 1;
            }
            if (type->scalar_rep == XR_NATIVE_F64) {
                *out_kind = XR_MACHINE_REP_F64;
                return 1;
            }
            return -1;
        case XR_KIND_BOOL:
            /* Bool is canonical in the semantic schema and carries no native scalar spelling. */
            if (type->scalar_rep != 0)
                return -1;
            *out_kind = XR_MACHINE_REP_I1;
            return 1;
        case XR_KIND_RUNE:
            if (type->scalar_rep != 0)
                return -1;
            *out_kind = XR_MACHINE_REP_RUNE;
            return 1;
        case XR_KIND_UNIT:
        case XR_KIND_NEVER:
            if (type->scalar_rep != 0)
                return -1;
            *out_kind = XR_MACHINE_REP_VOID;
            return 1;
        default:
            return 0;
    }
}

static bool target_plan_has_layout_for_type(const XrTargetPlan *plan, uint32_t semantic_type) {
    uint32_t low = 0;
    uint32_t high = plan->layouts_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        uint32_t candidate = plan->layouts[middle].semantic_type;
        if (candidate < semantic_type)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < plan->layouts_count && plan->layouts[low].semantic_type == semantic_type;
}

static bool verify_value_binding(const XrTargetPlan *plan, uint32_t semantic_value,
                                 uint32_t semantic_type, uint32_t semantic_function,
                                 uint8_t *bound_slots) {
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(plan->semantic_plan, semantic_type);
    uint16_t expected_kind = XR_MACHINE_REP_COUNT;
    int eligibility = type ? semantic_type_expected_rep(type, &expected_kind) : -1;
    const XrTargetValueRepRecord *record = xr_target_plan_value_rep(plan, semantic_value);
    if (eligibility < 0)
        return false;
    if (eligibility == 0)
        return record == NULL;
    if (!record || plan->machine_reps[record->register_rep].kind != expected_kind ||
        plan->machine_reps[record->memory_rep].kind != expected_kind)
        return false;
    if (expected_kind == XR_MACHINE_REP_VOID)
        return record->slot == XR_SEMANTIC_INDEX_NONE;
    if (!target_plan_has_layout_for_type(plan, semantic_type))
        return false;
    if (semantic_function >= plan->functions_count || record->slot >= plan->slots_count)
        return false;
    const XrTargetFunctionRecord *target_function = &plan->functions[semantic_function];
    const XrTargetSlotRecord *slot = &plan->slots[record->slot];
    const XrTargetMachineRepRecord *memory = &plan->machine_reps[record->memory_rep];
    if (target_function->id != semantic_function ||
        target_function->semantic_function != semantic_function ||
        !range_valid(target_function->slot_begin, target_function->slot_count,
                     plan->slots_count) ||
        record->slot < target_function->slot_begin ||
        record->slot >= target_function->slot_begin + target_function->slot_count ||
        bound_slots[record->slot] || slot->function != semantic_function ||
        slot->register_rep != record->register_rep || slot->memory_rep != record->memory_rep ||
        slot->size != memory->memory_size || slot->align != memory->memory_align ||
        slot->root_kind != XR_TARGET_ROOT_NONE ||
        slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;
    bound_slots[record->slot] = 1;
    return true;
}

static bool verify_value_reps(const XrTargetPlan *plan, char *error, size_t error_size) {
    uint32_t expected_values = 0;
    size_t semantic_functions = xr_semantic_plan_function_count(plan->semantic_plan);
    if (semantic_functions > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003", "semantic function index budget overflow");
    if (plan->functions_count != semantic_functions)
        return report(error, error_size, "XR_TARGET_1001",
                      "target functions do not cover semantic value ownership");
    for (uint32_t function_index = 0; function_index < (uint32_t) semantic_functions;
         function_index++) {
        const XrSemanticFunctionRecord *function =
            xr_semantic_plan_function(plan->semantic_plan, function_index);
        if (!function || function->value_begin != expected_values ||
            function->value_count > UINT32_MAX - expected_values)
            return report(error, error_size, "XR_TARGET_1001",
                          "semantic value ranges are not globally dense");
        expected_values += function->value_count;
    }
    if (expected_values > SIZE_MAX / sizeof(uint32_t))
        return report(error, error_size, "XR_EXEC_5003", "value representation budget overflow");
    uint32_t previous_value = 0;
    for (uint32_t index = 0; index < plan->value_reps_count; index++) {
        const XrTargetValueRepRecord *record = &plan->value_reps[index];
        if (record->semantic_value >= expected_values ||
            (index && record->semantic_value <= previous_value) ||
            record->register_rep >= plan->machine_reps_count ||
            record->memory_rep >= plan->machine_reps_count ||
            !machine_rep_allows_conversion(plan, record->register_rep, record->memory_rep))
            return report(error, error_size, "XR_TARGET_1001",
                          "value representation binding is unordered, invalid, or not convertible");
        previous_value = record->semantic_value;
    }
    uint8_t *defined = NULL;
    uint8_t *bound_slots = NULL;
    if (expected_values) {
        defined = (uint8_t *) xr_calloc(expected_values, sizeof(*defined));
    }
    if (plan->slots_count)
        bound_slots = (uint8_t *) xr_calloc(plan->slots_count, sizeof(*bound_slots));
    if ((expected_values && !defined) ||
        (plan->slots_count && !bound_slots)) {
        xr_free(defined);
        xr_free(bound_slots);
        return report(error, error_size, "XR_EXEC_5003", "value representation verifier allocation failed");
    }
    bool valid = true;
    uint32_t parameters = (uint32_t) xr_semantic_plan_parameter_count(plan->semantic_plan);
    for (uint32_t index = 0; valid && index < parameters; index++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan->semantic_plan, index);
        if (!parameter || parameter->value >= expected_values)
            valid = false;
        else if (!defined[parameter->value]) {
            defined[parameter->value] = 1;
            valid = verify_value_binding(plan, parameter->value, parameter->type,
                                         parameter->function, bound_slots);
        }
    }
    uint32_t operations = (uint32_t) xr_semantic_plan_operation_count(plan->semantic_plan);
    for (uint32_t index = 0; valid && index < operations; index++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, index);
        if (!operation)
            valid = false;
        else if (operation->result_value != XR_SEMANTIC_INDEX_NONE) {
            if (operation->result_value >= expected_values)
                valid = false;
            else if (!defined[operation->result_value]) {
                defined[operation->result_value] = 1;
                valid = verify_value_binding(plan, operation->result_value, operation->result_type,
                                             operation->function, bound_slots);
            }
        }
    }
    for (uint32_t value = 0; value < plan->value_reps_count; value++) {
        const XrTargetValueRepRecord *record = &plan->value_reps[value];
        if (!defined[record->semantic_value])
            valid = false;
    }
    for (uint32_t slot = 0; valid && slot < plan->slots_count; slot++)
        if (!bound_slots[slot])
            valid = false;
    xr_free(defined);
    xr_free(bound_slots);
    if (!valid)
        return report(error, error_size, "XR_TARGET_1001",
                      "value representation binding is incomplete, incompatible, or unlocated");
    return true;
}

static bool verify_extents(const XrTargetPlan *plan, char *error, size_t error_size) {
    for (uint32_t i = 0; i < plan->extents_count; i++) {
        const XrTargetExtentRecord *extent = &plan->extents[i];
        if (extent->id != i || extent->kind > XR_TARGET_EXTENT_PROVIDER_DEFINED ||
            (extent->alignment && !is_power_of_two(extent->alignment)) ||
            (extent->flags & ~(XR_TARGET_EXTENT_ZERO | XR_TARGET_EXTENT_ACCOUNT |
                               XR_TARGET_EXTENT_CLONE | XR_TARGET_EXTENT_TEARDOWN |
                               XR_TARGET_EXTENT_SIZED_DEALLOC)) != 0)
            return report(error, error_size, "XR_TARGET_1002", "extent record is invalid");
        if (extent->kind == XR_TARGET_EXTENT_FIXED) {
            if (extent->operand_count || extent->alignment || extent->stride || extent->provider ||
                extent->element_layout != XR_SEMANTIC_INDEX_NONE)
                return report(error, error_size, "XR_TARGET_1002",
                              "fixed extent carries variable-size facts");
            continue;
        }
        return report(error, error_size, "XR_TARGET_1002",
                      "variable extent lacks independently frozen semantic shape facts");
    }
    return true;
}

static bool verify_layouts(const XrTargetPlan *plan, char *error, size_t error_size) {
    size_t semantic_types = xr_semantic_plan_type_count(plan->semantic_plan);
    uint32_t previous_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t next_field = 0;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        const XrTargetLayoutRecord *layout = &plan->layouts[i];
        if (layout->id != i || layout->semantic_type >= semantic_types ||
            (previous_type != XR_SEMANTIC_INDEX_NONE && layout->semantic_type <= previous_type) ||
            layout->kind != XR_TARGET_LAYOUT_SCALAR || layout->reserved != 0 ||
            !is_power_of_two(layout->align) || layout->fixed_prefix_size % layout->align != 0 ||
            layout->extent >= plan->extents_count ||
            layout->field_begin != next_field ||
            !range_valid(layout->field_begin, layout->field_count, plan->fields_count))
            return report(error, error_size, "XR_TARGET_1002", "layout record is invalid");
        const XrSemanticTypeRecord *semantic_type =
            xr_semantic_plan_type(plan->semantic_plan, layout->semantic_type);
        uint16_t expected_rep = XR_MACHINE_REP_COUNT;
        if (!semantic_type || semantic_type_expected_rep(semantic_type, &expected_rep) != 1 ||
            expected_rep == XR_MACHINE_REP_VOID || layout->field_count != 0 ||
            layout->root_field_count != 0 || !stable_id_is_zero(layout->destructor) ||
            !stable_id_is_zero(layout->clone) || !stable_id_is_zero(layout->equality_hash) ||
            plan->extents[layout->extent].kind != XR_TARGET_EXTENT_FIXED)
            return report(error, error_size, "XR_TARGET_1002",
                          "layout lacks an independently provable scalar semantic contract");
        bool physical_match = false;
        for (uint32_t r = 0; r < plan->machine_reps_count; r++)
            if (plan->machine_reps[r].kind == expected_rep &&
                plan->machine_reps[r].memory_size == layout->fixed_prefix_size &&
                plan->machine_reps[r].memory_align == layout->align)
                physical_match = true;
        if (!physical_match)
            return report(error, error_size, "XR_TARGET_1002",
                          "scalar layout disagrees with its canonical machine representation");
        previous_type = layout->semantic_type;
        uint32_t previous_end = 0;
        uint32_t roots = 0;
        for (uint32_t f = 0; f < layout->field_count; f++) {
            const XrTargetFieldRecord *field = &plan->fields[layout->field_begin + f];
            if (field->layout != i || !field->size || !is_power_of_two(field->align) ||
                field->offset % field->align != 0 || field->offset < previous_end ||
                field->offset > layout->fixed_prefix_size ||
                field->size > layout->fixed_prefix_size - field->offset ||
                field->memory_rep >= plan->machine_reps_count ||
                field->root_kind > XR_TARGET_ROOT_VIEW_OWNER || field->flags != 0 ||
                field->reserved != 0)
                return report(error, error_size, "XR_TARGET_1002",
                              "field layout is misaligned, overlapping, or out of range");
            const XrTargetMachineRepRecord *rep = &plan->machine_reps[field->memory_rep];
            if (field->size != rep->memory_size || field->align != rep->memory_align ||
                field->root_kind != rep->root_kind)
                return report(error, error_size, "XR_TARGET_1002",
                              "field representation disagrees with its layout");
            if (!checked_u32_add(field->offset, field->size, &previous_end))
                return report(error, error_size, "XR_TARGET_1002",
                              "field layout offset overflows");
            roots += field->root_kind != XR_TARGET_ROOT_NONE;
        }
        if (roots != layout->root_field_count)
            return report(error, error_size, "XR_TARGET_1002",
                          "layout root bitmap cardinality is inconsistent");
        XrFingerprint actual;
        xr_target_layout_compute_fingerprint(plan, i, &actual);
        if (!xr_fingerprint_equal(actual, layout->fingerprint))
            return report(error, error_size, "XR_TARGET_1002",
                          "layout fingerprint changed after freeze");
        next_field += layout->field_count;
    }
    if (next_field != plan->fields_count)
        return report(error, error_size, "XR_TARGET_1002",
                      "layout field ranges do not exactly partition the field table");
    return true;
}

static bool verify_storage_and_allocations(const XrTargetPlan *plan, char *error,
                                           size_t error_size) {
    uint32_t next_operand = 0;
    for (uint32_t i = 0; i < plan->allocations_count; i++) {
        const XrTargetAllocationRecord *allocation = &plan->allocations[i];
        if (allocation->operand_begin != next_operand ||
            !range_valid(allocation->operand_begin, allocation->operand_count,
                         plan->extent_operands_count) ||
            !checked_u32_add(next_operand, allocation->operand_count, &next_operand))
            return report(error, error_size, "XR_TARGET_1002",
                          "allocation operand ranges do not exactly partition their table");
    }
    if (next_operand != plan->extent_operands_count || plan->storage_count ||
        plan->allocations_count || plan->extent_operands_count)
        return report(error, error_size, "XR_TARGET_1002",
                      "allocation tables require semantic allocation shape facts");
    return true;
}

static bool verify_functions_and_slots(const XrTargetPlan *plan, char *error,
                                       size_t error_size) {
    size_t semantic_functions = xr_semantic_plan_function_count(plan->semantic_plan);
    if (plan->functions_count != semantic_functions)
        return report(error, error_size, "XR_TARGET_1002",
                      "target function table does not cover the semantic plan");
    uint32_t next_slot = 0;
    uint32_t next_root = 0;
    uint32_t next_cleanup = 0;
    for (uint32_t i = 0; i < plan->functions_count; i++) {
        const XrTargetFunctionRecord *function = &plan->functions[i];
        if (function->id != i || function->semantic_function != i ||
            function->slot_begin != next_slot || function->root_begin != next_root ||
            function->cleanup_begin != next_cleanup ||
            !range_valid(function->slot_begin, function->slot_count, plan->slots_count) ||
            !range_valid(function->root_begin, function->root_count, plan->root_maps_count) ||
            !range_valid(function->cleanup_begin, function->cleanup_count, plan->cleanups_count))
            return report(error, error_size, "XR_TARGET_1002",
                          "target function table range is invalid");
        uint32_t previous_end = 0;
        for (uint32_t s = 0; s < function->slot_count; s++) {
            uint32_t slot_index = function->slot_begin + s;
            const XrTargetSlotRecord *slot = &plan->slots[slot_index];
            uint32_t slot_end = 0;
            if (slot->id != slot_index || slot->function != i || !slot->size ||
                !is_power_of_two(slot->align) || slot->offset % slot->align != 0 ||
                slot->offset < previous_end || !checked_u32_add(slot->offset, slot->size, &slot_end) ||
                slot->register_rep >= plan->machine_reps_count ||
                slot->memory_rep >= plan->machine_reps_count ||
                slot->root_kind > XR_TARGET_ROOT_VIEW_OWNER ||
                slot->ownership > XR_TARGET_OWNERSHIP_SHARED ||
                slot->debug_variable != XR_SEMANTIC_INDEX_NONE)
                return report(error, error_size, "XR_TARGET_1002",
                              "slot or its bounded debug reference is invalid");
            const XrTargetMachineRepRecord *memory = &plan->machine_reps[slot->memory_rep];
            if (slot->size != memory->memory_size || slot->align != memory->memory_align ||
                !machine_rep_allows_conversion(plan, slot->register_rep, slot->memory_rep) ||
                slot->root_kind != memory->root_kind || slot->ownership != memory->ownership)
                return report(error, error_size, "XR_TARGET_1002",
                              "slot disagrees with its memory representation");
            previous_end = slot_end;
        }
        next_slot += function->slot_count;
        next_root += function->root_count;
        next_cleanup += function->cleanup_count;
    }
    if (next_slot != plan->slots_count || next_root != plan->root_maps_count ||
        next_cleanup != plan->cleanups_count)
        return report(error, error_size, "XR_TARGET_1002",
                      "target function ranges do not partition their tables");
    return true;
}

static bool verify_calls(const XrTargetPlan *plan, char *error, size_t error_size) {
    uint32_t next_argument = 0;
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        const XrTargetCallRecord *call = &plan->calls[i];
        if (call->result_register_rep >= plan->machine_reps_count ||
            call->result_memory_rep >= plan->machine_reps_count ||
            !machine_rep_allows_conversion(plan, call->result_register_rep,
                                           call->result_memory_rep) ||
            call->argument_begin != next_argument ||
            !range_valid(call->argument_begin, call->argument_count,
                         plan->call_arguments_count) ||
            !checked_u32_add(next_argument, call->argument_count, &next_argument))
            return report(error, error_size, "XR_TARGET_1003",
                          "call representation or argument partition is invalid");
        for (uint32_t a = 0; a < call->argument_count; a++) {
            const XrTargetCallArgumentRecord *argument =
                &plan->call_arguments[call->argument_begin + a];
            if (argument->register_rep >= plan->machine_reps_count ||
                argument->memory_rep >= plan->machine_reps_count ||
                !machine_rep_allows_conversion(plan, argument->register_rep,
                                               argument->memory_rep))
                return report(error, error_size, "XR_TARGET_1003",
                              "call argument representation is not independently convertible");
        }
    }
    if (next_argument != plan->call_arguments_count || plan->calls_count ||
        plan->call_arguments_count)
        return report(error, error_size, "XR_TARGET_1003",
                      "call tables require semantic callee and argument contract facts");
    return true;
}

static bool verify_roots_and_cleanups(const XrTargetPlan *plan, char *error, size_t error_size) {
    uint32_t next_root_slot = 0;
    for (uint32_t i = 0; i < plan->root_maps_count; i++) {
        const XrTargetRootMapRecord *root = &plan->root_maps[i];
        if (root->slot_begin != next_root_slot ||
            !range_valid(root->slot_begin, root->slot_count, plan->root_slots_count) ||
            !checked_u32_add(next_root_slot, root->slot_count, &next_root_slot))
            return report(error, error_size, "XR_TARGET_1002",
                          "root slot ranges do not exactly partition their table");
    }
    if (next_root_slot != plan->root_slots_count || plan->root_maps_count ||
        plan->root_slots_count || plan->cleanups_count)
        return report(error, error_size, "XR_TARGET_1002",
                      "root and cleanup tables require semantic liveness facts");
    return true;
}

static bool verify_adapters_and_capabilities(const XrTargetPlan *plan, char *error,
                                             size_t error_size) {
    if (plan->adapters_count) {
        for (uint32_t i = 0; i < plan->adapters_count; i++) {
            const XrTargetAdapterRecord *adapter = &plan->adapters[i];
            if (adapter->input_rep >= plan->machine_reps_count ||
                adapter->output_rep >= plan->machine_reps_count ||
                !machine_rep_allows_conversion(plan, adapter->input_rep, adapter->output_rep))
                return report(error, error_size, "XR_EXEC_5002",
                              "adapter representation is not independently convertible");
        }
    }
    if (plan->adapters_count)
        return report(error, error_size, "XR_EXEC_5002",
                      "adapter tables require semantic boundary facts");
    if (plan->capabilities_count)
        return report(error, error_size, "XR_TARGET_1004",
                      "capability tables require semantic provider requirements");
    return true;
}

static bool verify_coroutines(const XrTargetPlan *plan, char *error, size_t error_size) {
    if (plan->coroutines_count)
        return report(error, error_size, "XR_CORO_4000",
                      "coroutine tables require semantic suspension-state facts");
    return true;
}

bool xr_target_plan_verify(const XrTargetPlan *plan, char *error, size_t error_size) {
    if (!plan || !plan->frozen || !plan->semantic_plan || !plan->profile)
        return report(error, error_size, "XR_EXEC_5000",
                      "verifier requires a frozen TargetPlan");
    if (plan->schema_version != XR_TARGET_PLAN_SCHEMA_VERSION)
        return report(error, error_size, "XR_ARTIFACT_2000",
                      "TargetPlan schema version is not exactly supported");
    char nested_error[512] = {0};
    if (!xr_semantic_plan_verify(plan->semantic_plan, nested_error, sizeof(nested_error)) ||
        !xr_fingerprint_equal(plan->semantic_fingerprint,
                              xr_semantic_plan_fingerprint(plan->semantic_plan)))
        return report(error, error_size, "XR_TARGET_1000",
                      "TargetPlan semantic fingerprint does not match its exact input");
    if (!xr_target_profile_verify(plan->profile, error, error_size) ||
         !verify_resource_budgets(plan, error, error_size) ||
         !verify_machine_reps(plan, error, error_size) ||
         !verify_value_reps(plan, error, error_size) ||
        !verify_extents(plan, error, error_size) ||
        !verify_layouts(plan, error, error_size) ||
        !verify_storage_and_allocations(plan, error, error_size) ||
        !verify_functions_and_slots(plan, error, error_size) ||
        !verify_calls(plan, error, error_size) ||
        !verify_roots_and_cleanups(plan, error, error_size) ||
        !verify_adapters_and_capabilities(plan, error, error_size) ||
        !verify_coroutines(plan, error, error_size))
        return false;
    XrFingerprint actual;
    xr_target_plan_compute_fingerprint(plan, &actual);
    if (!xr_fingerprint_equal(actual, plan->fingerprint))
        return report(error, error_size, "XR_TARGET_1000",
                      "TargetPlan fingerprint changed after freeze");
    return true;
}
