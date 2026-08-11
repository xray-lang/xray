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
#include "xr_target_instruction_verify.h"
#include "xr_target_plan_internal.h"
#include "xr_target_profile_internal.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
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

static bool checked_align_u32(uint32_t value, uint32_t alignment, uint32_t *out) {
    if (!is_power_of_two(alignment) || value > UINT32_MAX - alignment + 1u)
        return false;
    *out = (value + alignment - 1u) & ~(alignment - 1u);
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

static bool profile_identity_is_consistent(const XrTargetMachineFacts *facts) {
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

static bool profile_machine_features_are_consistent(const XrTargetMachineFacts *facts) {
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
    const XrTargetMachineFacts *machine = &facts->machine;
    if (facts->schema_version != XR_TARGET_PROFILE_SCHEMA_VERSION ||
        machine->architecture <= XR_TARGET_ARCH_NONE ||
        machine->architecture >= XR_TARGET_ARCH_COUNT ||
        machine->operating_system <= XR_TARGET_OS_NONE ||
        machine->operating_system >= XR_TARGET_OS_COUNT ||
        machine->environment <= XR_TARGET_ENV_NONE ||
        machine->environment >= XR_TARGET_ENV_COUNT ||
        machine->native_abi <= XR_TARGET_ABI_NONE ||
        machine->native_abi >= XR_TARGET_ABI_COUNT ||
        machine->runtime_profile < XR_TARGET_RUNTIME_PROFILE_HOSTED ||
        machine->runtime_profile > XR_TARGET_RUNTIME_PROFILE_FREESTANDING ||
        machine->reserved8[0] != 0 || machine->reserved8[1] != 0 ||
        machine->reserved8[2] != 0 || machine->reserved16 != 0)
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile contains an unsupported exact identity");
    if (!xr_target_data_layout_validate(&machine->data_layout))
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
    const uint64_t provider_mask = XR_TARGET_PROVIDER_MASK_ALL;
    if ((machine->atomic_width_mask & ~atomic_width_mask) != 0 ||
        (machine->atomic_order_mask & ~atomic_order_mask) != 0 ||
        (machine->float_feature_mask & ~float_mask) != 0 ||
        (machine->vector_feature_mask & ~vector_mask) != 0 ||
        (facts->provider_mask & ~provider_mask) != 0 ||
        (facts->provider_mask & XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR)) == 0 ||
        (facts->provider_mask & XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC)) == 0 ||
        (machine->float_feature_mask & XR_TARGET_FLOAT_IEEE754) == 0 ||
        ((machine->float_feature_mask & XR_TARGET_FLOAT_STRICT) != 0 &&
         (machine->float_feature_mask & XR_TARGET_FLOAT_FAST) != 0) ||
        (machine->vector_feature_mask == 0 && machine->maximum_vector_bits != 0) ||
        (machine->vector_feature_mask != 0 &&
         (!is_power_of_two(machine->maximum_vector_bits) ||
          machine->maximum_vector_bits < 128u ||
          machine->maximum_vector_bits > 2048u)) ||
        fingerprint_is_zero(facts->provider_set_fingerprint) ||
        fingerprint_is_zero(facts->object_header_fingerprint) ||
        fingerprint_is_zero(facts->runtime_abi_fingerprint))
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile runtime facts are incomplete");
    if (!profile_identity_is_consistent(machine) ||
        !profile_machine_features_are_consistent(machine))
        return report(error, error_size, "XR_TARGET_1000",
                      "target machine identity or feature facts are inconsistent");
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
        plan->slots_count > 16000000u || plan->instructions_count > 40000000u ||
        plan->calls_count > 10000000u ||
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
    XR_ADD_TARGET_BYTES(instructions);
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
    XR_REQUIRE_TARGET_TABLE(instructions);
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
                                       const XrTargetMachineFacts *profile) {
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
    const XrTargetMachineFacts *profile =
        xr_target_profile_machine_facts(plan->profile);
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
                rep->signedness != XR_TARGET_SIGN_NONE ||
                (rep->kind == XR_MACHINE_REP_AGGREGATE &&
                 (rep->root_kind != XR_TARGET_ROOT_NONE ||
                  rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
                  rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE)))
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
                rep->lane_count < 2 || rep->signedness != XR_TARGET_SIGN_NONE ||
                rep->root_kind != XR_TARGET_ROOT_NONE ||
                rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
                rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE)
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
    const XrTargetMachineFacts *profile =
        xr_target_profile_machine_facts(plan->profile);
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
            if (type->scalar_rep != XR_SCALAR_REP_NONE)
                return -1;
            *out_kind = XR_MACHINE_REP_I1;
            return 1;
        case XR_KIND_RUNE:
            if (type->scalar_rep != XR_SCALAR_REP_NONE)
                return -1;
            *out_kind = XR_MACHINE_REP_RUNE;
            return 1;
        case XR_KIND_UNIT:
        case XR_KIND_NEVER:
            if (type->scalar_rep != XR_SCALAR_REP_NONE)
                return -1;
            *out_kind = XR_MACHINE_REP_VOID;
            return 1;
        default:
            return 0;
    }
}

static int semantic_aggregate_kind(const XrSemanticTypeRecord *type) {
    if (!type)
        return -1;
    if ((type->flags & XR_SEM_TYPE_NULLABLE) != 0)
        return 0;
    if (type->kind == XR_KIND_TUPLE || type->kind == XR_KIND_FIXED_ARRAY)
        return type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1;
    if (type->kind == XR_KIND_STRUCT_OBJECT)
        return (type->flags & XR_SEM_TYPE_VALUE) == 0
                   ? 0
                   : (type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1);
    if (type->kind == XR_KIND_INSTANCE)
        return (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) == 0
                   ? 0
                   : (type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1);
    return 0;
}

static int semantic_aggregate_eligibility(const XrSemanticPlan *plan,
                                          uint32_t semantic_type,
                                          uint32_t *stack, uint32_t depth) {
    if (semantic_type >= xr_semantic_plan_type_count(plan) || depth >= 64)
        return -1;
    for (uint32_t i = 0; i < depth; i++)
        if (stack[i] == semantic_type)
            return -1;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    uint16_t scalar_kind = XR_MACHINE_REP_COUNT;
    int scalar = type ? semantic_type_expected_rep(type, &scalar_kind) : -1;
    if (scalar < 0)
        return -1;
    if (scalar == 1)
        return scalar_kind == XR_MACHINE_REP_VOID ? 0 : 1;
    int aggregate = semantic_aggregate_kind(type);
    if (aggregate <= 0)
        return aggregate;
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (type->child_begin > child_count ||
        type->child_count > child_count - type->child_begin ||
        (type->kind == XR_KIND_FIXED_ARRAY
             ? (type->child_count != 1 || type->aggregate_extent == 0 ||
                type->aggregate_extent > UINT16_MAX)
             : type->aggregate_extent != type->child_count))
        return -1;
    stack[depth] = semantic_type;
    uint32_t dependencies =
        type->kind == XR_KIND_FIXED_ARRAY ? 1u : type->child_count;
    for (uint32_t i = 0; i < dependencies; i++) {
        int child = semantic_aggregate_eligibility(
            plan, children[type->child_begin + i], stack, depth + 1u);
        if (child <= 0)
            return child;
    }
    return 1;
}

static bool semantic_fixed_array_count(const XrSemanticPlan *plan, uint32_t semantic_type,
                                       uint32_t *out) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || type->child_count != 1 ||
        type->aggregate_extent == 0 || type->aggregate_extent > UINT16_MAX)
        return false;
    *out = type->aggregate_extent;
    return true;
}

static bool mark_coroutine_functions(const XrSemanticPlan *plan, uint8_t *deferred,
                                     uint32_t function_count) {
    size_t entity_count = xr_semantic_plan_entity_count(plan);
    size_t operation_count = xr_semantic_plan_operation_count(plan);
    for (size_t i = 0; i < entity_count; i++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(plan, i);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE ||
            entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION)
            continue;
        if (entity->subject >= operation_count)
            return false;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, entity->subject);
        if (!operation || operation->function >= function_count)
            return false;
        deferred[operation->function] = 1;
    }
    return true;
}

static const XrSemanticParameterRecord *semantic_parameter_for_value(
    const XrSemanticPlan *plan, uint32_t function, uint32_t value) {
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(plan);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, i);
        if (parameter && parameter->function == function && parameter->value == value)
            return parameter;
    }
    return NULL;
}

static bool reconstruct_value_slot_identity(const XrTargetPlan *plan,
                                             const XrTargetSlotRecord *slot,
                                             uint32_t semantic_value,
                                             uint32_t semantic_function,
                                             XrStableId *out) {
    if (!slot || !out || slot->semantic_value != semantic_value ||
        slot->function != semantic_function ||
        slot->logical_slot != XR_SEMANTIC_INDEX_NONE)
        return false;
    const XrSemanticParameterRecord *parameter =
        semantic_parameter_for_value(plan->semantic_plan, semantic_function, semantic_value);
    XrStableId source;
    uint8_t expected_role;
    if (parameter) {
        if (slot->semantic_operation != XR_SEMANTIC_INDEX_NONE)
            return false;
        expected_role = XR_TARGET_SLOT_PARAMETER;
        source = parameter->id;
    } else {
        if (slot->semantic_operation >=
            xr_semantic_plan_operation_count(plan->semantic_plan))
            return false;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan,
                                       slot->semantic_operation);
        if (!operation || operation->function != semantic_function ||
            operation->result_value != semantic_value ||
            operation->opcode == XI_PARAM)
            return false;
        expected_role = operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI
                                                    : XR_TARGET_SLOT_TEMPORARY;
        source = operation->id;
    }
    if (slot->role != expected_role)
        return false;
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(plan->semantic_plan, semantic_function);
    if (!function)
        return false;
    char function_id[XR_STABLE_ID_BYTES * 2 + 1];
    char source_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    xr_stable_id_hex(function->id, function_id);
    xr_stable_id_hex(source, source_id);
    int written = snprintf(key, sizeof(key),
                           "xray-target-slot-v2:function=%s:role=%u:source=%s:logical=%u",
                           function_id, (unsigned) expected_role, source_id,
                           XR_SEMANTIC_INDEX_NONE);
    XrFingerprint digest;
    return written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static int target_plan_layout_for_type(const XrTargetPlan *plan, uint32_t semantic_type) {
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
    return low < plan->layouts_count && plan->layouts[low].semantic_type == semantic_type
               ? (int) low
               : -1;
}

static bool verify_value_binding(const XrTargetPlan *plan, uint32_t semantic_value,
                                 uint32_t semantic_type, uint32_t semantic_function,
                                 const XrSemanticOperationRecord *operation,
                                 uint8_t *bound_slots,
                                 const uint8_t *deferred_functions,
                                 uint32_t *failure_reason) {
#define XR_VALUE_BINDING_FAIL(reason)                                                            \
    do {                                                                                          \
        if (failure_reason)                                                                       \
            *failure_reason = (reason);                                                           \
        return false;                                                                             \
    } while (0)
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(plan->semantic_plan, semantic_type);
    uint16_t expected_kind = XR_MACHINE_REP_COUNT;
    bool generated_result_void =
        operation && operation->opcode < XI_OP_COUNT &&
        xi_generated_op_result_kind(operation->opcode) == XI_GEN_RESULT_VOID;
    bool operation_result_void =
        generated_result_void && operation->function == semantic_function &&
        operation->result_value == semantic_value &&
        operation->result_type == semantic_type &&
        operation->effects == xi_generated_op_effects(operation->opcode) &&
        operation->result_ownership ==
            xi_generated_op_result_ownership(operation->opcode);
    if (generated_result_void && !operation_result_void)
        XR_VALUE_BINDING_FAIL(1);
    int eligibility = operation_result_void
                          ? 1
                          : (type ? semantic_type_expected_rep(type,
                                                               &expected_kind)
                                  : -1);
    /* Aggregate bindings in these domains belong to later exact families. */
    bool deferred_operation =
        deferred_functions[semantic_function] ||
        (operation && operation->opcode < XI_OP_COUNT &&
         (xi_generated_op_class(operation->opcode) == XI_GEN_CLASS_CALL ||
          xi_generated_op_class(operation->opcode) == XI_GEN_CLASS_COROUTINE));
    uint32_t aggregate_stack[64] = {0};
    int aggregate = eligibility == 0 && !deferred_operation
                        ? semantic_aggregate_eligibility(plan->semantic_plan,
                                                         semantic_type,
                                                         aggregate_stack, 0)
                        : 0;
    int expected_layout = -1;
    if (eligibility == 0 && deferred_operation) {
        eligibility = 0;
    } else if (aggregate == 1) {
        expected_kind = XR_MACHINE_REP_AGGREGATE;
        expected_layout = target_plan_layout_for_type(plan, semantic_type);
        eligibility = expected_layout >= 0 ? 1 : -1;
    } else if (aggregate < 0) {
        eligibility = -1;
    } else if (aggregate == 0 && eligibility == 0) {
        eligibility = 0;
    }
    if (operation_result_void)
        expected_kind = XR_MACHINE_REP_VOID;
    const XrTargetValueRepRecord *record = xr_target_plan_value_rep(plan, semantic_value);
    if (eligibility < 0)
        XR_VALUE_BINDING_FAIL(2);
    if (eligibility == 0) {
        if (record != NULL)
            XR_VALUE_BINDING_FAIL(7);
        return true;
    }
    if (!record || plan->machine_reps[record->register_rep].kind != expected_kind ||
        plan->machine_reps[record->memory_rep].kind != expected_kind ||
        (expected_kind == XR_MACHINE_REP_AGGREGATE &&
         (plan->machine_reps[record->register_rep].detail != (uint32_t) expected_layout ||
          plan->machine_reps[record->memory_rep].detail != (uint32_t) expected_layout)))
        XR_VALUE_BINDING_FAIL(3);
    if (expected_kind == XR_MACHINE_REP_VOID) {
        if (record->slot != XR_SEMANTIC_INDEX_NONE)
            XR_VALUE_BINDING_FAIL(8);
        return true;
    }
    if (target_plan_layout_for_type(plan, semantic_type) < 0)
        XR_VALUE_BINDING_FAIL(4);
    if (semantic_function >= plan->functions_count || record->slot >= plan->slots_count)
        XR_VALUE_BINDING_FAIL(5);
    const XrTargetFunctionRecord *target_function = &plan->functions[semantic_function];
    const XrTargetSlotRecord *slot = &plan->slots[record->slot];
    const XrTargetMachineRepRecord *memory = &plan->machine_reps[record->memory_rep];
    XrStableId expected_slot_identity;
    if (target_function->id != semantic_function ||
        target_function->semantic_function != semantic_function ||
        !range_valid(target_function->slot_begin, target_function->slot_count,
                     plan->slots_count) ||
        record->slot < target_function->slot_begin ||
        record->slot >= target_function->slot_begin + target_function->slot_count ||
        bound_slots[record->slot] || slot->function != semantic_function ||
        !reconstruct_value_slot_identity(plan, slot, semantic_value, semantic_function,
                                         &expected_slot_identity) ||
        !xr_stable_id_equal(slot->identity, expected_slot_identity) ||
        slot->register_rep != record->register_rep || slot->memory_rep != record->memory_rep ||
        slot->size != memory->memory_size || slot->align != memory->memory_align ||
        slot->root_kind != memory->root_kind ||
        slot->ownership != memory->ownership)
        XR_VALUE_BINDING_FAIL(6);
    bound_slots[record->slot] = 1;
#undef XR_VALUE_BINDING_FAIL
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
    uint8_t *deferred_functions = NULL;
    if (expected_values) {
        defined = (uint8_t *) xr_calloc(expected_values, sizeof(*defined));
    }
    if (plan->slots_count)
        bound_slots = (uint8_t *) xr_calloc(plan->slots_count, sizeof(*bound_slots));
    if (semantic_functions)
        deferred_functions =
            (uint8_t *) xr_calloc(semantic_functions, sizeof(*deferred_functions));
    if ((expected_values && !defined) ||
        (plan->slots_count && !bound_slots) ||
        (semantic_functions && !deferred_functions) ||
        !mark_coroutine_functions(plan->semantic_plan, deferred_functions,
                                  (uint32_t) semantic_functions)) {
        xr_free(defined);
        xr_free(bound_slots);
        xr_free(deferred_functions);
        return report(error, error_size, "XR_EXEC_5003", "value representation verifier allocation failed");
    }
    bool valid = true;
    uint32_t failed_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t failed_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t failed_opcode = XI_OP_COUNT;
    uint32_t failure_reason = 0;
    uint32_t parameters = (uint32_t) xr_semantic_plan_parameter_count(plan->semantic_plan);
    for (uint32_t index = 0; valid && index < parameters; index++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan->semantic_plan, index);
        if (!parameter || parameter->value >= expected_values)
            valid = false;
        else if (!defined[parameter->value]) {
            defined[parameter->value] = 1;
            valid = verify_value_binding(plan, parameter->value, parameter->type,
                                         parameter->function, NULL, bound_slots,
                                         deferred_functions,
                                         &failure_reason);
            if (!valid) {
                failed_value = parameter->value;
                failed_type = parameter->type;
            }
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
                                             operation->function, operation,
                                             bound_slots, deferred_functions,
                                             &failure_reason);
                if (!valid) {
                    failed_value = operation->result_value;
                    failed_type = operation->result_type;
                    failed_opcode = operation->opcode;
                }
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
    xr_free(deferred_functions);
    if (!valid) {
        const XrSemanticTypeRecord *failed_semantic_type =
            failed_type == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_type(plan->semantic_plan, failed_type);
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_TARGET_1001: value representation binding is incomplete, "
                     "incompatible, or unlocated "
                     "(value=%u type=%u:%s opcode=%u:%s reason=%u)",
                     failed_value, failed_type,
                     failed_semantic_type && failed_semantic_type->canonical_key
                         ? failed_semantic_type->canonical_key
                         : "<unknown>",
                     failed_opcode,
                     failed_opcode < XI_OP_COUNT
                         ? xi_generated_op_name((XiOp) failed_opcode)
                         : "<parameter>", failure_reason);
        return false;
    }
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
                extent->element_layout != XR_SEMANTIC_INDEX_NONE || extent->flags)
                return report(error, error_size, "XR_TARGET_1002",
                              "fixed extent carries variable-size facts");
            continue;
        }
        return report(error, error_size, "XR_TARGET_1002",
                      "variable extent lacks independently frozen semantic shape facts");
    }
    return true;
}

static bool verify_extent_references(const XrTargetPlan *plan, char *error,
                                     size_t error_size) {
    uint8_t *referenced = NULL;
    if (plan->extents_count) {
        referenced = (uint8_t *) xr_calloc(plan->extents_count, sizeof(*referenced));
        if (!referenced)
            return report(error, error_size, "XR_EXEC_5003",
                          "extent reference verifier allocation failed");
    }
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        referenced[plan->layouts[i].extent] = 1;
    for (uint32_t i = 0; i < plan->extents_count; i++) {
        if (!referenced[i]) {
            xr_free(referenced);
            return report(error, error_size, "XR_TARGET_1002",
                          "extent table contains a row outside the layout reference domain");
        }
    }
    xr_free(referenced);
    return true;
}

static bool verify_aggregate_layout_acyclic(const XrTargetPlan *plan, uint32_t layout,
                                            uint8_t *states, uint32_t depth) {
    if (depth > 64 || states[layout] == 1)
        return false;
    if (states[layout] == 2)
        return true;
    states[layout] = 1;
    const XrTargetLayoutRecord *record = &plan->layouts[layout];
    for (uint32_t i = 0; i < record->field_count; i++) {
        const XrTargetFieldRecord *field = &plan->fields[record->field_begin + i];
        if (field->memory_rep >= plan->machine_reps_count)
            return false;
        const XrTargetMachineRepRecord *rep = &plan->machine_reps[field->memory_rep];
        if (rep->kind == XR_MACHINE_REP_AGGREGATE &&
            (rep->detail >= plan->layouts_count ||
             !verify_aggregate_layout_acyclic(plan, rep->detail, states, depth + 1u)))
            return false;
    }
    states[layout] = 2;
    return true;
}

static bool verify_layouts(const XrTargetPlan *plan, char *error, size_t error_size) {
    size_t semantic_types = xr_semantic_plan_type_count(plan->semantic_plan);
    uint32_t child_table_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(plan->semantic_plan, &child_table_count);
    uint32_t previous_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t next_field = 0;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        const XrTargetLayoutRecord *layout = &plan->layouts[i];
        if (layout->id != i || layout->semantic_type >= semantic_types ||
            (previous_type != XR_SEMANTIC_INDEX_NONE && layout->semantic_type <= previous_type) ||
            (layout->kind != XR_TARGET_LAYOUT_SCALAR &&
             layout->kind != XR_TARGET_LAYOUT_AGGREGATE) ||
            layout->reserved != 0 ||
            !is_power_of_two(layout->align) || layout->fixed_prefix_size % layout->align != 0 ||
            layout->extent >= plan->extents_count ||
            layout->field_begin != next_field ||
            !range_valid(layout->field_begin, layout->field_count, plan->fields_count))
            return report(error, error_size, "XR_TARGET_1002", "layout record is invalid");
        const XrSemanticTypeRecord *semantic_type =
            xr_semantic_plan_type(plan->semantic_plan, layout->semantic_type);
        uint16_t expected_rep = XR_MACHINE_REP_COUNT;
        int scalar = semantic_type ? semantic_type_expected_rep(semantic_type, &expected_rep) : -1;
        if (!semantic_type || !stable_id_is_zero(layout->destructor) ||
            !stable_id_is_zero(layout->clone) || !stable_id_is_zero(layout->equality_hash) ||
            plan->extents[layout->extent].kind != XR_TARGET_EXTENT_FIXED)
            return report(error, error_size, "XR_TARGET_1002",
                          "layout lacks an independently provable semantic contract");
        if (layout->kind == XR_TARGET_LAYOUT_SCALAR) {
            if (scalar != 1 || expected_rep == XR_MACHINE_REP_VOID || layout->field_count != 0 ||
                layout->root_field_count != 0)
                return report(error, error_size, "XR_TARGET_1002",
                              "scalar layout semantic contract is incomplete");
            bool physical_match = false;
            for (uint32_t r = 0; r < plan->machine_reps_count; r++)
                if (plan->machine_reps[r].kind == expected_rep &&
                    plan->machine_reps[r].memory_size == layout->fixed_prefix_size &&
                    plan->machine_reps[r].memory_align == layout->align)
                    physical_match = true;
            if (!physical_match)
                return report(error, error_size, "XR_TARGET_1002",
                              "scalar layout disagrees with its canonical machine representation");
        } else {
            uint32_t expected_fields = semantic_type->aggregate_extent;
            if (scalar != 0 || semantic_aggregate_kind(semantic_type) != 1 ||
                semantic_type->child_begin > child_table_count ||
                semantic_type->child_count > child_table_count - semantic_type->child_begin ||
                semantic_type->aggregate_align > UINT16_MAX ||
                (semantic_type->kind == XR_KIND_FIXED_ARRAY &&
                 (semantic_type->child_count != 1 ||
                  !semantic_fixed_array_count(plan->semantic_plan, layout->semantic_type,
                                              &expected_fields))) ||
                (semantic_type->kind != XR_KIND_FIXED_ARRAY &&
                 semantic_type->aggregate_extent != semantic_type->child_count) ||
                layout->field_count != expected_fields)
                return report(error, error_size, "XR_TARGET_1002",
                              "aggregate layout semantic field facts are incomplete");
            uint32_t representation_count = 0;
            for (uint32_t r = 0; r < plan->machine_reps_count; r++)
                representation_count += plan->machine_reps[r].kind == XR_MACHINE_REP_AGGREGATE &&
                                        plan->machine_reps[r].detail == i;
            if (representation_count != 1)
                return report(error, error_size, "XR_TARGET_1002",
                              "aggregate layout has no unique machine representation");
        }
        previous_type = layout->semantic_type;
        uint32_t previous_end = 0;
        uint32_t expected_align = 1;
        uint32_t roots = 0;
        for (uint32_t f = 0; f < layout->field_count; f++) {
            const XrTargetFieldRecord *field = &plan->fields[layout->field_begin + f];
            uint32_t child_ordinal = semantic_type->kind == XR_KIND_FIXED_ARRAY ? 0u : f;
            uint32_t child_type = children[semantic_type->child_begin + child_ordinal];
            int child_layout_index = target_plan_layout_for_type(plan, child_type);
            uint32_t expected_offset = 0;
            if (field->layout != i || !field->size || !is_power_of_two(field->align) ||
                field->semantic_field != f || child_layout_index < 0 ||
                !checked_align_u32(previous_end, field->align, &expected_offset) ||
                field->offset != expected_offset ||
                field->offset > layout->fixed_prefix_size ||
                field->size > layout->fixed_prefix_size - field->offset ||
                field->memory_rep >= plan->machine_reps_count ||
                field->root_kind > XR_TARGET_ROOT_VIEW_OWNER || field->flags != 0 ||
                field->reserved != 0)
                return report(error, error_size, "XR_TARGET_1002",
                              "field layout is misaligned, overlapping, or out of range");
            const XrTargetMachineRepRecord *rep = &plan->machine_reps[field->memory_rep];
            const XrTargetLayoutRecord *child_layout = &plan->layouts[child_layout_index];
            if (field->size != rep->memory_size || field->align != rep->memory_align ||
                field->root_kind != rep->root_kind ||
                field->size != child_layout->fixed_prefix_size ||
                field->align != child_layout->align ||
                (child_layout->kind == XR_TARGET_LAYOUT_AGGREGATE &&
                 (rep->kind != XR_MACHINE_REP_AGGREGATE ||
                  rep->detail != (uint32_t) child_layout_index)))
                return report(error, error_size, "XR_TARGET_1002",
                              "field representation disagrees with its layout");
            if (child_layout->kind == XR_TARGET_LAYOUT_SCALAR) {
                const XrSemanticTypeRecord *child_semantic =
                    xr_semantic_plan_type(plan->semantic_plan, child_type);
                uint16_t child_rep = XR_MACHINE_REP_COUNT;
                if (!child_semantic || semantic_type_expected_rep(child_semantic, &child_rep) != 1 ||
                    rep->kind != child_rep)
                    return report(error, error_size, "XR_TARGET_1002",
                                  "field scalar representation disagrees with SemanticPlan");
            }
            if (!checked_u32_add(field->offset, field->size, &previous_end))
                return report(error, error_size, "XR_TARGET_1002",
                              "field layout offset overflows");
            roots += field->root_kind != XR_TARGET_ROOT_NONE;
            if (field->align > expected_align)
                expected_align = field->align;
        }
        uint32_t expected_size = previous_end ? previous_end : 1u;
        if (semantic_type->aggregate_align > expected_align)
            expected_align = semantic_type->aggregate_align;
        if (!checked_align_u32(expected_size, expected_align, &expected_size) ||
            roots != layout->root_field_count ||
            (layout->kind == XR_TARGET_LAYOUT_AGGREGATE &&
             (layout->align != expected_align || layout->fixed_prefix_size != expected_size)))
            return report(error, error_size, "XR_TARGET_1002",
                          "layout padding, alignment, size, or root cardinality is inconsistent");
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
    uint8_t *states = plan->layouts_count
                          ? (uint8_t *) xr_calloc(plan->layouts_count, sizeof(*states))
                          : NULL;
    if (plan->layouts_count && !states)
        return report(error, error_size, "XR_EXEC_5003",
                      "aggregate recursion verifier allocation failed");
    bool acyclic = true;
    for (uint32_t i = 0; acyclic && i < plan->layouts_count; i++)
        if (plan->layouts[i].kind == XR_TARGET_LAYOUT_AGGREGATE)
            acyclic = verify_aggregate_layout_acyclic(plan, i, states, 0);
    xr_free(states);
    if (!acyclic)
        return report(error, error_size, "XR_TARGET_1002",
                      "aggregate layout contains a recursive inline cycle");
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
            function->cleanup_begin != next_cleanup || function->reserved != 0 ||
            !range_valid(function->slot_begin, function->slot_count, plan->slots_count) ||
            !range_valid(function->root_begin, function->root_count, plan->root_maps_count) ||
            !range_valid(function->cleanup_begin, function->cleanup_count, plan->cleanups_count))
            return report(error, error_size, "XR_TARGET_1002",
                          "target function table range is invalid");
        uint32_t previous_end = 0;
        uint32_t expected_frame_align = 1;
        for (uint32_t s = 0; s < function->slot_count; s++) {
            uint32_t slot_index = function->slot_begin + s;
            const XrTargetSlotRecord *slot = &plan->slots[slot_index];
            uint32_t slot_end = 0;
            uint32_t expected_offset = 0;
            if (slot->id != slot_index || slot->function != i ||
                stable_id_is_zero(slot->identity) || !slot->size ||
                !is_power_of_two(slot->align) || slot->offset % slot->align != 0 ||
                !checked_align_u32(previous_end, slot->align, &expected_offset) ||
                slot->offset != expected_offset ||
                !checked_u32_add(slot->offset, slot->size, &slot_end) ||
                slot->register_rep >= plan->machine_reps_count ||
                slot->memory_rep >= plan->machine_reps_count ||
                slot->role <= XR_TARGET_SLOT_ROLE_INVALID ||
                slot->role >= XR_TARGET_SLOT_ROLE_COUNT ||
                slot->root_kind > XR_TARGET_ROOT_VIEW_OWNER ||
                slot->ownership > XR_TARGET_OWNERSHIP_SHARED ||
                slot->reserved != 0 || slot->debug_variable != XR_SEMANTIC_INDEX_NONE ||
                (s && xr_stable_id_compare(plan->slots[slot_index - 1u].identity,
                                           slot->identity) >= 0))
                return report(error, error_size, "XR_TARGET_1002",
                              "slot or its bounded debug reference is invalid");
            const XrTargetMachineRepRecord *memory = &plan->machine_reps[slot->memory_rep];
            if (slot->size != memory->memory_size || slot->align != memory->memory_align ||
                !machine_rep_allows_conversion(plan, slot->register_rep, slot->memory_rep) ||
                slot->root_kind != memory->root_kind || slot->ownership != memory->ownership)
                return report(error, error_size, "XR_TARGET_1002",
                              "slot disagrees with its memory representation");
            if (slot->align > expected_frame_align)
                expected_frame_align = slot->align;
            previous_end = slot_end;
        }
        uint32_t expected_frame_size = 0;
        if (!checked_align_u32(previous_end, expected_frame_align, &expected_frame_size) ||
            function->frame_align != expected_frame_align ||
            function->frame_size != expected_frame_size)
            return report(error, error_size, "XR_TARGET_1002",
                          "function frame does not exactly pack its slot range");
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

static bool reconstruct_call_identity(const char *domain, XrStableId first,
                                      XrStableId second, uint32_t ordinal,
                                      XrStableId *out) {
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrFingerprint digest;
    xr_stable_id_hex(first, first_hex);
    xr_stable_id_hex(second, second_hex);
    int written = snprintf(key, sizeof(key), "%s:first=%s:second=%s:ordinal=%u",
                           domain, first_hex, second_hex, ordinal);
    return out && written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static bool operation_is_call_shaped(const XrSemanticPlan *semantic,
                                     const XrSemanticOperationRecord *operation) {
    if (operation) {
        /* Keep the independent boundary on exact semantic op identities. */
        switch (operation->opcode) {
            case XI_CALL:
            case XI_CALL_METHOD:
            case XI_CALL_METHOD_DIRECT:
            case XI_TAIL_CALL:
            case XI_CALL_BUILTIN:
            case XI_ATOMIC_TO_STRING:
            case XI_EXTRACT:
            case XI_GEN_CALL:
            case XI_MULTI_RET: return true;
            default: break;
        }
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    if (!operation || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    for (uint32_t i = 0; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *operand = &operands[operation->operand_begin + i];
        if (operand->role == XR_SEM_OPERAND_CALLEE ||
            operand->role == XR_SEM_OPERAND_RECEIVER ||
            (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0)
            return true;
    }
    return false;
}

static bool slot_binds_value_in_function(const XrTargetPlan *plan,
                                         const XrTargetValueRepRecord *value,
                                         uint32_t semantic_function) {
    if (!value)
        return false;
    if (plan->machine_reps[value->memory_rep].kind == XR_MACHINE_REP_VOID)
        return value->slot == XR_SEMANTIC_INDEX_NONE;
    if (value->slot >= plan->slots_count || semantic_function >= plan->functions_count)
        return false;
    const XrTargetSlotRecord *slot = &plan->slots[value->slot];
    const XrTargetFunctionRecord *function = &plan->functions[semantic_function];
    return slot->semantic_value == value->semantic_value &&
           slot->function == semantic_function &&
           value->slot >= function->slot_begin &&
           value->slot < function->slot_begin + function->slot_count &&
           slot->register_rep == value->register_rep &&
           slot->memory_rep == value->memory_rep;
}

static bool verify_calls(const XrTargetPlan *plan, char *error, size_t error_size) {
    const XrSemanticPlan *semantic = plan->semantic_plan;
    uint32_t semantic_operations = (uint32_t) xr_semantic_plan_operation_count(semantic);
    uint32_t semantic_targets = (uint32_t) xr_semantic_plan_call_target_count(semantic);
    uint32_t semantic_functions = (uint32_t) xr_semantic_plan_function_count(semantic);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    uint8_t *covered = (uint8_t *) xr_calloc(semantic_operations, sizeof(*covered));
    uint8_t *deferred = (uint8_t *) xr_calloc(semantic_functions, sizeof(*deferred));
    if ((semantic_operations && !covered) || (semantic_functions && !deferred) ||
        !mark_coroutine_functions(semantic, deferred, semantic_functions)) {
        xr_free(covered);
        xr_free(deferred);
        return report(error, error_size, "XR_EXEC_5003", "call verifier allocation failed");
    }
    bool valid = plan->calls_count == semantic_targets;
    uint32_t next_argument = 0;
    uint32_t next_adapter = 0;
    for (uint32_t i = 0; valid && i < plan->calls_count; i++) {
        const XrTargetCallRecord *call = &plan->calls[i];
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(semantic, i);
        const XrSemanticOperationRecord *operation = target
                                                         ? xr_semantic_plan_operation(
                                                               semantic, target->operation)
                                                         : NULL;
        const XrSemanticFunctionRecord *callee = target
                                                      ? xr_semantic_plan_function(
                                                            semantic, target->function)
                                                      : NULL;
        XrStableId expected_identity;
        uint16_t result_kind = XR_MACHINE_REP_COUNT;
        const XrSemanticTypeRecord *result_type = operation
                                                       ? xr_semantic_plan_type(
                                                             semantic, operation->result_type)
                                                       : NULL;
        const XrTargetValueRepRecord *result = operation
                                                    ? xr_target_plan_value_rep(
                                                          plan, operation->result_value)
                                                    : NULL;
        int result_scalar = result_type
                                ? semantic_type_expected_rep(result_type, &result_kind)
                                : -1;
        const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(plan->profile);
        valid = target && operation && callee && machine &&
                target->operation < semantic_operations && !covered[target->operation] &&
                target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
                operation->opcode == XI_CALL && operation->function < semantic_functions &&
                !deferred[operation->function] && !deferred[target->function] &&
                operation->result_type == callee->return_type && result_scalar == 1 &&
                result && result->register_rep < plan->machine_reps_count &&
                result->memory_rep < plan->machine_reps_count &&
                plan->machine_reps[result->register_rep].kind == result_kind &&
                plan->machine_reps[result->memory_rep].kind == result_kind &&
                slot_binds_value_in_function(plan, result, operation->function) &&
                operation->operand_count == (uint32_t) callee->parameter_count + 1u &&
                operation->operand_begin <= operand_count &&
                operation->operand_count <= operand_count - operation->operand_begin &&
                callee->parameter_begin <= xr_semantic_plan_parameter_count(semantic) &&
                callee->parameter_count <= xr_semantic_plan_parameter_count(semantic) -
                                                   callee->parameter_begin &&
                reconstruct_call_identity("xray-target-call-v2", target->id,
                                          operation->id, 0, &expected_identity) &&
                xr_stable_id_equal(call->identity, expected_identity) && call->id == i &&
                call->semantic_call_target == i &&
                call->semantic_operation == target->operation &&
                call->caller_function == operation->function &&
                call->callee_function == target->function &&
                call->result_value == operation->result_value &&
                call->result_slot == result->slot &&
                call->caller_storage_slot == XR_SEMANTIC_INDEX_NONE &&
                call->error_slot == XR_SEMANTIC_INDEX_NONE &&
                call->argument_begin == next_argument &&
                range_valid(call->argument_begin, call->argument_count,
                            plan->call_arguments_count) &&
                call->argument_count == callee->parameter_count &&
                call->adapter_begin == next_adapter && call->adapter_count == 0 &&
                call->result_register_rep == result->register_rep &&
                call->result_memory_rep == result->memory_rep &&
                call->error_register_rep < plan->machine_reps_count &&
                call->error_memory_rep < plan->machine_reps_count &&
                plan->machine_reps[call->error_register_rep].kind == XR_MACHINE_REP_VOID &&
                plan->machine_reps[call->error_memory_rep].kind == XR_MACHINE_REP_VOID &&
                call->native_abi == machine->native_abi && call->flags == 0 &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
                call->target_kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
                call->result_mode == XR_TARGET_CALL_VALUE &&
                call->result_ownership == XR_TARGET_CALL_NONE &&
                call->error_mode == XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL &&
                call->reserved8 == 0;
        if (!valid)
            break;
        covered[target->operation] = 1;
        const XrSemanticOperandRecord *callee_operand =
            &operands[operation->operand_begin];
        valid = callee_operand->role == XR_SEM_OPERAND_CALLEE &&
                callee_operand->parameter == -1 &&
                (callee_operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0;
        for (uint32_t ordinal = 0; valid && ordinal < call->argument_count; ordinal++) {
            const XrTargetCallArgumentRecord *argument =
                &plan->call_arguments[next_argument];
            uint32_t parameter_index = callee->parameter_begin + ordinal;
            uint32_t semantic_operand = operation->operand_begin + ordinal + 1u;
            const XrSemanticParameterRecord *parameter =
                xr_semantic_plan_parameter(semantic, parameter_index);
            const XrSemanticOperandRecord *operand = &operands[semantic_operand];
            const XrTargetValueRepRecord *caller_value =
                xr_target_plan_value_rep(plan, operand->value);
            const XrTargetValueRepRecord *callee_value = parameter
                                                             ? xr_target_plan_value_rep(
                                                                   plan, parameter->value)
                                                             : NULL;
            XrStableId argument_identity;
            uint16_t argument_kind = XR_MACHINE_REP_COUNT;
            int argument_scalar = operand->type < xr_semantic_plan_type_count(semantic)
                                      ? semantic_type_expected_rep(
                                            xr_semantic_plan_type(semantic, operand->type),
                                            &argument_kind)
                                      : -1;
            uint8_t ownership = operand->ownership_action == XR_SEM_OPERAND_CONSUME
                                    ? XR_TARGET_CALL_CONSUME
                                    : XR_TARGET_CALL_READ;
            valid = parameter && operand->role == XR_SEM_OPERAND_ARGUMENT &&
                    operand->parameter == (int16_t) ordinal &&
                    operand->type == parameter->type &&
                    operand->parameter_mode == parameter->mode &&
                    operand->transfer_mode == parameter->transfer_mode &&
                    (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0 &&
                    argument_scalar == 1 && parameter->mode == XR_PARAM_READ &&
                    operand->access == XR_CALL_ARG_PLAIN &&
                    (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) == 0 &&
                    parameter->ownership == XI_OWN_NONE && caller_value && callee_value &&
                    slot_binds_value_in_function(plan, caller_value, operation->function) &&
                    slot_binds_value_in_function(plan, callee_value, target->function) &&
                    caller_value->register_rep == callee_value->register_rep &&
                    caller_value->memory_rep == callee_value->memory_rep &&
                    reconstruct_call_identity("xray-target-call-argument-v1", target->id,
                                              parameter->id, ordinal,
                                              &argument_identity) &&
                    xr_stable_id_equal(argument->identity, argument_identity) &&
                    argument->call == i && argument->semantic_operand == semantic_operand &&
                    argument->semantic_value == operand->value &&
                    argument->callee_parameter == parameter_index &&
                    argument->caller_slot == caller_value->slot &&
                    argument->callee_slot == callee_value->slot &&
                    argument->register_rep == caller_value->register_rep &&
                    argument->memory_rep == caller_value->memory_rep &&
                    plan->machine_reps[argument->register_rep].kind == argument_kind &&
                    argument->ordinal == ordinal && argument->mode == XR_TARGET_CALL_VALUE &&
                    argument->ownership == ownership &&
                    argument->transfer_mode == operand->transfer_mode && argument->flags == 0;
            next_argument++;
        }
        XrFingerprint fingerprint;
        xr_target_call_compute_fingerprint(plan, i, &fingerprint);
        valid = valid && xr_fingerprint_equal(fingerprint, call->fingerprint);
    }
    for (uint32_t i = 0; valid && i < semantic_operations; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (operation_is_call_shaped(semantic, operation) && !covered[i])
            valid = false;
    }
    valid = valid && next_argument == plan->call_arguments_count &&
            next_adapter == plan->adapters_count;
    xr_free(covered);
    xr_free(deferred);
    return valid || report(error, error_size, "XR_TARGET_1003",
                           "call/adapter tables do not exactly cover DIRECT_LOCAL authority");
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
    if (plan->adapters_count)
        return report(error, error_size, "XR_TARGET_1003",
                      "DIRECT_LOCAL scalar calls require an exact empty adapter partition");
    const XrTargetProfileDraft *facts = xr_target_profile_facts(plan->profile);
    uint64_t capability_mask = 0;
    for (uint32_t i = 0; i < plan->capabilities_count; i++) {
        const XrTargetCapabilityRecord *record = &plan->capabilities[i];
        if (record->id != i ||
            record->capability <= XR_TARGET_PROVIDER_INVALID ||
            record->capability >= XR_TARGET_PROVIDER_KIND_COUNT ||
            record->provider != record->capability ||
            record->flags != XR_TARGET_CAPABILITY_REQUIRED)
            return report(error, error_size, "XR_TARGET_1004",
                          "capability record is not canonically provider-bound");
        uint64_t bit = XR_TARGET_PROVIDER_MASK(record->provider);
        if ((capability_mask & bit) != 0 || !facts ||
            (facts->provider_mask & bit) == 0)
            return report(error, error_size, "XR_TARGET_1004",
                          "capability provider is absent or duplicated");
        capability_mask |= bit;
    }
    if (capability_mask != XR_TARGET_FOUNDATION_CAPABILITY_MASK)
        return report(error, error_size, "XR_TARGET_1004",
                      "TargetPlan foundation capability closure is incomplete");
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
    if (plan->completed_family_mask != XR_TARGET_REQUIRED_FAMILIES)
        return report(error, error_size, "XR_TARGET_1001",
                      "TargetPlan family coverage is incomplete or unsupported");
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
        !verify_extent_references(plan, error, error_size) ||
        !verify_storage_and_allocations(plan, error, error_size) ||
        !verify_functions_and_slots(plan, error, error_size) ||
        !xr_target_instruction_program_verify(plan, error, error_size) ||
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
