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
#include "../../frontend/analyzer/xa_intrinsic_registry.h"
#include "../semantic/xr_semantic_verify.h"
#include "../semantic/xr_semantic_graph.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

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
        fingerprint_is_zero(facts->runtime_abi_fingerprint) ||
        xr_runtime_string_literal_materialization_contract_verify(
            &facts->string_literal) != XR_RUNTIME_ABI_OK)
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
        case XR_MACHINE_REP_ENUM_ORDINAL:
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
        case XR_MACHINE_REP_ENUM_ORDINAL: {
            const XrSemanticTypeRecord *type =
                xr_semantic_plan_type(plan->semantic_plan, rep->detail);
            return type && type->kind == XR_KIND_ENUM && type->source_enum_key &&
                   type->enum_layout_id != 0 && type->enum_member_count != 0 &&
                   type->enum_flags ==
                       (XR_SEM_ENUM_DECLARATION_EXACT | XR_SEM_ENUM_UNIT) &&
                   type->reserved_enum == 0 && type->builtin_type == XR_TID_NULL &&
                   type->child_count == 0 && type->aggregate_extent == 0 &&
                   type->aggregate_align == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
                   (type->flags & XR_SEM_TYPE_NULLABLE) == 0 && rep->lane_count == 0 &&
                   rep->root_kind == XR_TARGET_ROOT_NONE &&
                   rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
                   rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE;
        }
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
                   (rep->ownership == XR_TARGET_OWNERSHIP_OWNED ||
                    rep->ownership == XR_TARGET_OWNERSHIP_BORROWED) &&
                   rep->null_encoding == XR_TARGET_NULL_TAGGED &&
                   rep->signedness == XR_TARGET_SIGN_NONE;
        case XR_MACHINE_REP_AGGREGATE:
        case XR_MACHINE_REP_VIEW: {
            int layout_index = rep->kind == XR_MACHINE_REP_VIEW
                                   ? target_plan_layout_for_type(plan, rep->detail)
                                   : (rep->detail < plan->layouts_count ? (int) rep->detail : -1);
            if (layout_index < 0 || rep->lane_count != 0 ||
                rep->signedness != XR_TARGET_SIGN_NONE ||
                (rep->kind == XR_MACHINE_REP_AGGREGATE &&
                 (rep->root_kind != XR_TARGET_ROOT_NONE ||
                  rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
                  rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE)))
                return false;
            const XrTargetLayoutRecord *layout = &plan->layouts[layout_index];
            uint8_t expected_kind = rep->kind == XR_MACHINE_REP_VIEW ? XR_TARGET_LAYOUT_VIEW
                                                                     : XR_TARGET_LAYOUT_AGGREGATE;
            return layout->kind == expected_kind &&
                   (rep->kind != XR_MACHINE_REP_VIEW ||
                    (rep->detail == layout->semantic_type && rep->root_kind == XR_TARGET_ROOT_VIEW_OWNER &&
                     rep->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
                     rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE)) &&
                   rep->memory_size == layout->fixed_prefix_size &&
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

static bool semantic_unit_enum_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_ENUM && type->source_enum_key &&
           !xr_stable_id_equal(type->source_enum_identity, zero) &&
           type->enum_layout_id != 0 && type->enum_member_count != 0 &&
           type->enum_flags == (XR_SEM_ENUM_DECLARATION_EXACT | XR_SEM_ENUM_UNIT) &&
           type->reserved_enum == 0 && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           (type->flags & XR_SEM_TYPE_NULLABLE) == 0;
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

static bool semantic_heap_closure_is_exact(const XrSemanticPlan *semantic,
                                           const XrSemanticOperationRecord *operation) {
    static const char suffix[] = "/allocation";
    XrStableId expected_allocation;
    XrFingerprint allocation_digest;
    size_t canonical_length =
        operation && operation->canonical_key
            ? strlen(operation->canonical_key)
            : 0;
    size_t allocation_length =
        operation && operation->allocation_key
            ? strlen(operation->allocation_key)
            : 0;
    if (!semantic || !operation || operation->opcode != XI_CLOSURE_NEW ||
        operation->callable_function >= xr_semantic_plan_function_count(semantic) ||
        operation->operand_count != 0 || !operation->canonical_key ||
        !operation->allocation_key ||
        canonical_length > SIZE_MAX - sizeof(suffix) ||
        allocation_length != canonical_length + sizeof(suffix) - 1u ||
        memcmp(operation->allocation_key, operation->canonical_key,
               canonical_length) != 0 ||
        memcmp(operation->allocation_key + canonical_length, suffix,
               sizeof(suffix)) != 0 ||
        !xr_stable_id_from_key(operation->allocation_key,
                               &expected_allocation, &allocation_digest) ||
        !xr_stable_id_equal(expected_allocation, operation->allocation_id) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(semantic, operation->callable_function);
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, operation->result_type);
    uint32_t child_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_count);
    bool typed_function = type && type->kind == XR_KIND_FUNCTION;
    bool opaque_closure = type && type->kind == XR_KIND_UNKNOWN &&
                          type->child_count == 0;
    if (!callee || !type || callee->parent != operation->function ||
        callee->capture_count != 0 || (!typed_function && !opaque_closure) ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE |
                        XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        callee->parameter_count == UINT16_MAX ||
        (typed_function &&
         type->child_count != (uint32_t) callee->parameter_count + 1u) ||
        type->child_begin > child_count ||
        type->child_count > child_count - type->child_begin ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(semantic) ||
        callee->parameter_count > xr_semantic_plan_parameter_count(semantic) -
                                      callee->parameter_begin)
        return false;
    for (uint32_t i = 0; i < callee->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, callee->parameter_begin + i);
        if (!parameter || parameter->function != operation->callable_function ||
            parameter->ordinal != i ||
            (typed_function &&
             children[type->child_begin + i] != parameter->type))
            return false;
    }
    return opaque_closure ||
           children[type->child_begin + callee->parameter_count] ==
               callee->return_type;
}

static bool semantic_string_literal_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation) {
    if (!semantic || !operation || operation->opcode != XI_CONST ||
        operation->operand_count != 0 ||
        operation->constant >= xr_semantic_plan_constant_count(semantic) ||
        operation->allocation_key || !stable_id_is_zero(operation->allocation_id) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1)
        return false;
    const XrSemanticConstantRecord *constant =
        xr_semantic_plan_constant(semantic, operation->constant);
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, operation->result_type);
    if (!constant || constant->kind != XR_SEM_CONST_STRING ||
        !constant->string || constant->type != operation->result_type ||
        !type || type->kind != XR_KIND_STRING || type->child_count != 0 ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->aggregate_extent != 0 || type->aggregate_align != 0)
        return false;
    uint8_t forbidden = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT;
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                       XR_SEM_TYPE_OWNERSHIP_ROOT;
    return (type->flags & forbidden) == 0 &&
           (type->flags & required) == required;
}

static bool semantic_stringbuilder_type_is_exact(const XrSemanticTypeRecord *type) {
    char expected_type_key[160];
    int written = snprintf(
        expected_type_key, sizeof(expected_type_key),
        "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:13:StringBuilder[0]",
        (unsigned) XR_KIND_INSTANCE, (unsigned) XR_TID_STRINGBUILDER,
        (unsigned) XR_SCALAR_REP_NONE);
    return type && written > 0 && (size_t) written < sizeof(expected_type_key) &&
           type->kind == XR_KIND_INSTANCE && type->builtin_type == XR_TID_STRINGBUILDER &&
           type->child_count == 0 && type->aggregate_extent == 0 &&
           type->aggregate_align == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key && strcmp(type->canonical_key, expected_type_key) == 0;
}

static bool operation_is_exact_stringbuilder_append_rune(
    const XrSemanticPlan *semantic, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value, uint32_t *argument_value);

static bool semantic_stringbuilder_constructor_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation) {
    static const char suffix[] = "/allocation";
    uint32_t metadata_count = 0;
    const char *const *metadata =
        xr_semantic_plan_metadata(semantic, &metadata_count);
    size_t canonical_length = operation && operation->canonical_key
                                  ? strlen(operation->canonical_key)
                                  : 0;
    size_t allocation_length = operation && operation->allocation_key
                                   ? strlen(operation->allocation_key)
                                   : 0;
    XrStableId expected_allocation;
    XrFingerprint digest;
    if (!semantic || !operation || operation->opcode != XI_CALL_BUILTIN ||
        operation->operand_count != 0 || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count || !metadata ||
        strcmp(metadata[operation->metadata_begin], "StringBuilder") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->semantic_immediate != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_BUILTIN) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_BUILTIN) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_BUILTIN) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        !operation->canonical_key || !operation->allocation_key ||
        canonical_length > SIZE_MAX - sizeof(suffix) ||
        allocation_length != canonical_length + sizeof(suffix) - 1u ||
        memcmp(operation->allocation_key, operation->canonical_key,
               canonical_length) != 0 ||
        memcmp(operation->allocation_key + canonical_length, suffix,
               sizeof(suffix)) != 0 ||
        !xr_stable_id_from_key(operation->allocation_key,
                               &expected_allocation, &digest) ||
        !xr_stable_id_equal(expected_allocation, operation->allocation_id))
        return false;
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, operation->result_type);
    return semantic_stringbuilder_type_is_exact(type);
}

/* Independent reconstruction: consume only frozen SemanticPlan rows. */
static bool semantic_string_byte_slice_view_is_exact(
    const XrSemanticPlan *semantic, const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!semantic || !operation || operation->opcode != XI_CALL_BUILTIN ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        operation->evidence[1] != XA_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->view_source_operand != 0 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_RECEIVER || operation->view_capability != 1 ||
        operation->view_lifetime != 1 || operation->view_complete != 1 ||
        operation->view_element_type >= xr_semantic_plan_type_count(semantic))
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(semantic, source->type);
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(semantic, operation->result_type);
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(semantic, operation->view_element_type);
    return source->value == operation->view_source_value && source->parameter == 0 &&
           source->role == XR_SEM_OPERAND_ARGUMENT &&
           (source->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0 && source_type &&
           source_type->kind == XR_KIND_STRING && source_type->scalar_rep == XR_SCALAR_REP_NONE &&
           result_type && result_type->kind == XR_KIND_SLICE && result_type->child_count == 1 &&
           result_type->child_begin < child_count &&
           children[result_type->child_begin] == operation->view_element_type &&
           result_type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW) &&
           element && element->kind == XR_KIND_INT && element->scalar_rep == XR_NATIVE_U8;
}

static bool semantic_u8_slice_type_is_exact(const XrSemanticPlan *semantic,
                                            uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW;
    const uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!type || type->kind != XR_KIND_SLICE || type->builtin_type != XR_TID_NULL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 || type->child_count != 1 ||
        type->child_begin >= child_count || (type->flags & required) != required ||
        (type->flags & ~allowed) != 0)
        return false;
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(semantic, children[type->child_begin]);
    return element && element->kind == XR_KIND_INT &&
           element->builtin_type == XR_TID_NULL && element->scalar_rep == XR_NATIVE_U8 &&
           element->flags == 0 && element->child_count == 0 &&
           element->aggregate_extent == 0 && element->aggregate_align == 0;
}

static bool semantic_u8_slice_parameter_is_exact(
    const XrSemanticPlan *semantic, const XrSemanticParameterRecord *parameter) {
    return parameter && parameter->function < xr_semantic_plan_function_count(semantic) &&
           parameter->value != XR_SEMANTIC_INDEX_NONE && parameter->mode == XR_PARAM_READ &&
           parameter->ownership == XI_OWN_BORROWED &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 &&
           parameter->reserved == 0 && semantic_u8_slice_type_is_exact(semantic, parameter->type);
}

static bool verifier_channel_type_is_exact(const XrSemanticPlan *semantic,
                                           uint32_t type_index,
                                           uint32_t *element_type) {
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, type_index);
    uint32_t child_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_count);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                       XR_SEM_TYPE_OWNERSHIP_ROOT;
    uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!semantic || !type || type->kind != XR_KIND_CHANNEL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 1 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & required) != required ||
        (type->flags & ~allowed) != 0 || type->child_begin >= child_count ||
        children[type->child_begin] >= xr_semantic_plan_type_count(semantic))
        return false;
    if (element_type)
        *element_type = children[type->child_begin];
    return true;
}

static bool verifier_channel_capacity_type_is_exact(
    const XrSemanticPlan *semantic, uint32_t type_index) {
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, type_index);
    uint16_t kind = XR_MACHINE_REP_COUNT;
    return type && type->kind == XR_KIND_INT &&
           semantic_type_expected_rep(type, &kind) == 1 &&
           kind >= XR_MACHINE_REP_I8 && kind <= XR_MACHINE_REP_USIZE;
}

static bool verifier_channel_allocation_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation) {
    if (!semantic || !operation)
        return false;
    static const char suffix[] = "/allocation";
    size_t key_length = operation->canonical_key
                            ? strlen(operation->canonical_key)
                            : 0;
    size_t allocation_length = operation->allocation_key
                                   ? strlen(operation->allocation_key)
                                   : 0;
    XrStableId expected_allocation;
    XrFingerprint digest;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    if (operation->opcode != XI_CHAN_NEW ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        !operation->canonical_key || !operation->allocation_key ||
        key_length > SIZE_MAX - sizeof(suffix) ||
        allocation_length != key_length + sizeof(suffix) - 1u ||
        memcmp(operation->allocation_key, operation->canonical_key,
               key_length) != 0 ||
        memcmp(operation->allocation_key + key_length, suffix,
               sizeof(suffix)) != 0 ||
        !xr_stable_id_from_key(operation->allocation_key,
                               &expected_allocation, &digest) ||
        !xr_stable_id_equal(expected_allocation, operation->allocation_id) ||
        !verifier_channel_type_is_exact(semantic, operation->result_type,
                                        &element_type) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_NEW) ||
        operation->result_ownership !=
            xi_generated_op_result_ownership(XI_CHAN_NEW) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *capacity =
        &operands[operation->operand_begin];
    return capacity->value != XR_SEMANTIC_INDEX_NONE &&
           capacity->type < xr_semantic_plan_type_count(semantic) &&
           capacity->role == XR_SEM_OPERAND_VALUE && capacity->parameter == -1 &&
           capacity->flags == 0 &&
           verifier_channel_capacity_type_is_exact(semantic, capacity->type) &&
           element_type < xr_semantic_plan_type_count(semantic);
}

static bool verifier_channel_identity_copy_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation,
    const uint8_t *exact_channel_values, uint32_t value_count) {
    if (!semantic || !operation || !exact_channel_values)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    if (operation->opcode != XI_COPY || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY ||
        operation->allocation_key || !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *source =
        &operands[operation->operand_begin];
    uint32_t source_element = XR_SEMANTIC_INDEX_NONE;
    uint32_t result_element = XR_SEMANTIC_INDEX_NONE;
    return source->value < value_count && exact_channel_values[source->value] &&
           source->role == XR_SEM_OPERAND_VALUE && source->parameter == -1 &&
           source->flags == 0 &&
           verifier_channel_type_is_exact(semantic, source->type,
                                          &source_element) &&
           verifier_channel_type_is_exact(semantic, operation->result_type,
                                          &result_element) &&
           source_element == result_element;
}

static bool collect_exact_channel_values(const XrTargetPlan *plan,
                                         uint8_t **out, char *error,
                                         size_t error_size) {
    if (out)
        *out = NULL;
    size_t function_count =
        xr_semantic_plan_function_count(plan->semantic_plan);
    size_t operation_count =
        xr_semantic_plan_operation_count(plan->semantic_plan);
    if (!out || function_count > UINT32_MAX || operation_count > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003",
                      "channel outer-storage verifier budget is exhausted");
    uint32_t value_count = 0;
    if (function_count) {
        const XrSemanticFunctionRecord *last = xr_semantic_plan_function(
            plan->semantic_plan, (uint32_t) function_count - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return report(error, error_size, "XR_EXEC_5003",
                          "channel outer-storage value budget overflow");
        value_count = last->value_begin + last->value_count;
    }
    uint8_t *exact = value_count
                         ? (uint8_t *) xr_calloc(value_count, sizeof(*exact))
                         : NULL;
    if (value_count && !exact)
        return report(error, error_size, "XR_EXEC_5003",
                      "channel outer-storage verifier allocation failed");
    for (uint32_t i = 0; i < (uint32_t) operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!operation || operation->result_value >= value_count) {
            xr_free(exact);
            return report(error, error_size, "XR_TARGET_1001",
                          "channel outer-storage operation identity is invalid");
        }
        bool allocation = verifier_channel_allocation_is_exact(
            plan->semantic_plan, operation);
        bool alias = verifier_channel_identity_copy_is_exact(
            plan->semantic_plan, operation, exact, value_count);
        if (operation->opcode == XI_CHAN_NEW && !allocation) {
            xr_free(exact);
            return report(error, error_size, "XR_TARGET_1001",
                          "channel allocation authority is not exact");
        }
        if (allocation || alias)
            exact[operation->result_value] = 1;
    }
    *out = exact;
    return true;
}

/* Verifier-side reconstruction intentionally does not call the builder
 * predicate. It proves that every tagged receive boundary is a scalar
 * Channel<T> -> T projection rooted in the canonical channel family. */
static bool verifier_channel_receive_is_exact(
    const XrTargetPlan *plan, const XrSemanticOperationRecord *operation,
    const uint8_t *exact_channel_values, uint32_t value_count) {
    if (!plan || !operation || !exact_channel_values ||
        operation->opcode != XI_CHAN_TRY_RECV || operation->operand_count != 1 ||
        operation->result_value >= value_count || operation->allocation_key ||
        !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 || operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_TRY_RECV) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_TRY_RECV) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CHAN_TRY_RECV) ||
        operation->result_ownership !=
            xi_generated_op_result_ownership(XI_CHAN_TRY_RECV) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan->semantic_plan, &operand_count);
    if (operation->operand_begin >= operand_count)
        return false;
    const XrSemanticOperandRecord *receiver =
        &operands[operation->operand_begin];
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    uint16_t result_kind = XR_MACHINE_REP_COUNT;
    return receiver->value < value_count &&
           exact_channel_values[receiver->value] != 0 &&
           receiver->role == XR_SEM_OPERAND_VALUE && receiver->parameter == -1 &&
           receiver->transfer_mode == XR_TRANSFER_SHARE &&
           receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
           receiver->parameter_mode == XR_PARAM_READ &&
           receiver->access == XR_CALL_ARG_PLAIN &&
           receiver->origin == XI_PLACE_ORIGIN_NONE &&
           receiver->lifetime == XI_PLACE_LIFETIME_NONE &&
           receiver->escape == XI_PLACE_ESCAPE_NONE && receiver->flags == 0 &&
           verifier_channel_type_is_exact(plan->semantic_plan, receiver->type,
                                          &element_type) &&
           element_type == operation->result_type &&
           semantic_type_expected_rep(
               xr_semantic_plan_type(plan->semantic_plan,
                                     operation->result_type),
               &result_kind) == 1 &&
           result_kind != XR_MACHINE_REP_VOID;
}

static bool collect_exact_channel_receive_values(
    const XrTargetPlan *plan, const uint8_t *exact_channel_values,
    uint8_t **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    size_t function_count =
        xr_semantic_plan_function_count(plan->semantic_plan);
    size_t operation_count =
        xr_semantic_plan_operation_count(plan->semantic_plan);
    if (!out || !exact_channel_values || function_count > UINT32_MAX ||
        operation_count > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003",
                      "channel receive verifier budget is exhausted");
    uint32_t value_count = 0;
    if (function_count) {
        const XrSemanticFunctionRecord *last = xr_semantic_plan_function(
            plan->semantic_plan, (uint32_t) function_count - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return report(error, error_size, "XR_EXEC_5003",
                          "channel receive value budget overflow");
        value_count = last->value_begin + last->value_count;
    }
    uint8_t *exact = value_count
                         ? (uint8_t *) xr_calloc(value_count, sizeof(*exact))
                         : NULL;
    if (value_count && !exact)
        return report(error, error_size, "XR_EXEC_5003",
                      "channel receive verifier allocation failed");
    for (uint32_t i = 0; i < (uint32_t) operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!operation || operation->result_value >= value_count) {
            xr_free(exact);
            return report(error, error_size, "XR_TARGET_1001",
                          "channel receive operation identity is invalid");
        }
        if (operation->opcode != XI_CHAN_TRY_RECV)
            continue;
        uint16_t receive_kind = XR_MACHINE_REP_COUNT;
        if (semantic_type_expected_rep(
                xr_semantic_plan_type(plan->semantic_plan,
                                      operation->result_type),
                &receive_kind) != 1 ||
            receive_kind == XR_MACHINE_REP_VOID)
            continue;
        if (!verifier_channel_receive_is_exact(
                plan, operation, exact_channel_values, value_count)) {
            xr_free(exact);
            return report(error, error_size, "XR_TARGET_1001",
                          "channel receive storage authority is not exact");
        }
        exact[operation->result_value] = 1;
    }
    *out = exact;
    return true;
}

static bool verifier_direct_local_callee_type_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation,
    uint32_t target_function) {
    if (!semantic || !operation || operation->opcode != XI_GET_SHARED ||
        operation->semantic_immediate < 0 ||
        operation->semantic_immediate > UINT16_MAX ||
        operation->operand_count != 0 || operation->allocation_key ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_ownership !=
            xi_generated_op_result_ownership(XI_GET_SHARED) ||
        operation->effects != xi_generated_op_effects(XI_GET_SHARED) ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1 || operation->return_parameter != -1 ||
        target_function >= xr_semantic_plan_function_count(semantic))
        return false;
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (operation->allocation_id.bytes[i] != 0)
            return false;
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, operation->result_type);
    const XrSemanticFunctionRecord *target =
        xr_semantic_plan_function(semantic, target_function);
    uint32_t lexical_owner = target ? target->parent : XR_SEMANTIC_INDEX_NONE;
    uint32_t caller_ancestor = operation->function;
    for (uint32_t depth = 0;
         caller_ancestor != XR_SEMANTIC_INDEX_NONE &&
         caller_ancestor != lexical_owner &&
         depth < xr_semantic_plan_function_count(semantic);
         depth++) {
        const XrSemanticFunctionRecord *ancestor =
            xr_semantic_plan_function(semantic, caller_ancestor);
        caller_ancestor = ancestor ? ancestor->parent : XR_SEMANTIC_INDEX_NONE;
    }
    if (!type || !target || lexical_owner == XR_SEMANTIC_INDEX_NONE ||
        caller_ancestor != lexical_owner ||
        (type->kind != XR_KIND_FUNCTION &&
         type->kind != XR_KIND_UNKNOWN) ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->child_count != 0 ||
        target->parameter_begin > xr_semantic_plan_parameter_count(semantic) ||
        target->parameter_count > xr_semantic_plan_parameter_count(semantic) -
                                      target->parameter_begin ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE |
                        XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE |
             XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    return true;
}

static bool collect_exact_direct_local_callee_values(
    const XrTargetPlan *plan, uint8_t **out_exact, uint32_t **out_targets,
    char *error, size_t error_size) {
    if (out_exact)
        *out_exact = NULL;
    if (out_targets)
        *out_targets = NULL;
    size_t operation_size =
        xr_semantic_plan_operation_count(plan->semantic_plan);
    size_t target_size =
        xr_semantic_plan_call_target_count(plan->semantic_plan);
    size_t function_size =
        xr_semantic_plan_function_count(plan->semantic_plan);
    if (!out_exact || !out_targets || operation_size > UINT32_MAX ||
        target_size > UINT32_MAX || function_size > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003",
                      "direct-local callee verifier budget is exhausted");
    uint32_t value_count = 0;
    if (function_size) {
        const XrSemanticFunctionRecord *last = xr_semantic_plan_function(
            plan->semantic_plan, (uint32_t) function_size - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return report(error, error_size, "XR_EXEC_5003",
                          "direct-local callee value budget overflow");
        value_count = last->value_begin + last->value_count;
    }
    uint32_t operation_count = (uint32_t) operation_size;
    uint32_t *definition = operation_count || value_count
                               ? (uint32_t *) xr_calloc(
                                     value_count ? value_count : 1u,
                                     sizeof(*definition))
                               : NULL;
    uint32_t *target_by_operation = operation_count
                                        ? (uint32_t *) xr_calloc(
                                              operation_count,
                                              sizeof(*target_by_operation))
                                        : NULL;
    uint32_t *target_by_value = value_count
                                    ? (uint32_t *) xr_calloc(
                                          value_count,
                                          sizeof(*target_by_value))
                                    : NULL;
    uint32_t *use_count = value_count
                              ? (uint32_t *) xr_calloc(value_count,
                                                       sizeof(*use_count))
                              : NULL;
    uint8_t *invalid = value_count
                           ? (uint8_t *) xr_calloc(value_count,
                                                   sizeof(*invalid))
                           : NULL;
    uint8_t *exact = value_count
                         ? (uint8_t *) xr_calloc(value_count, sizeof(*exact))
                         : NULL;
    if ((value_count && (!definition || !target_by_value || !use_count ||
                         !invalid || !exact)) ||
        (operation_count && !target_by_operation)) {
        xr_free(definition);
        xr_free(target_by_operation);
        xr_free(target_by_value);
        xr_free(use_count);
        xr_free(invalid);
        xr_free(exact);
        return report(error, error_size, "XR_EXEC_5003",
                      "direct-local callee verifier allocation failed");
    }
    for (uint32_t i = 0; i < value_count; i++) {
        definition[i] = XR_SEMANTIC_INDEX_NONE;
        target_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!operation || operation->result_value >= value_count ||
            definition[operation->result_value] != XR_SEMANTIC_INDEX_NONE) {
            goto invalid_authority;
        }
        definition[operation->result_value] = i;
    }
    for (uint32_t i = 0; i < (uint32_t) target_size; i++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(plan->semantic_plan, i);
        if (target && target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (!target || target->operation >= operation_count ||
            target_by_operation[target->operation] !=
                XR_SEMANTIC_INDEX_NONE)
            goto invalid_authority;
        target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan->semantic_plan, &operand_count);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid_authority;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand =
                &operands[use->operand_begin + a];
            if (operand->value >= value_count)
                goto invalid_authority;
            uint32_t source_index = definition[operand->value];
            const XrSemanticOperationRecord *source =
                source_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_operation(plan->semantic_plan,
                                                 source_index);
            if (!source || source->opcode != XI_GET_SHARED)
                continue;
            uint32_t target_index = target_by_operation[i];
            const XrSemanticCallTargetRecord *target =
                target_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_call_target(plan->semantic_plan,
                                                   target_index);
            uint32_t value = source->result_value;
            bool use_is_exact =
                a == 0 &&
                (use->opcode == XI_CALL || use->opcode == XI_TAIL_CALL) &&
                use->function == source->function && target &&
                target->operation == i &&
                target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
                operand->role == XR_SEM_OPERAND_CALLEE &&
                operand->parameter == -1 &&
                (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 &&
                operand->type == source->result_type &&
                value == operand->value;
            if (!use_is_exact ||
                (target_by_value[value] != XR_SEMANTIC_INDEX_NONE &&
                 target_by_value[value] != target->function) ||
                use_count[value] == UINT32_MAX) {
                invalid[value] = 1;
                continue;
            }
            target_by_value[value] = target->function;
            use_count[value]++;
        }
    }
    for (uint32_t i = 0;
         i < (uint32_t) xr_semantic_plan_block_count(plan->semantic_plan);
         i++) {
        const XrSemanticBlockRecord *block =
            xr_semantic_plan_block(plan->semantic_plan, i);
        if (!block || block->control_value == XR_SEMANTIC_INDEX_NONE ||
            block->control_value >= value_count)
            continue;
        uint32_t source_index = definition[block->control_value];
        const XrSemanticOperationRecord *source =
            source_index == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_operation(plan->semantic_plan,
                                             source_index);
        if (source && source->opcode == XI_GET_SHARED)
            invalid[block->control_value] = 1;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!operation || operation->opcode != XI_GET_SHARED ||
            operation->result_value >= value_count ||
            target_by_value[operation->result_value] ==
                XR_SEMANTIC_INDEX_NONE)
            continue;
        uint32_t value = operation->result_value;
        if (invalid[value] || use_count[value] == 0 ||
            !verifier_direct_local_callee_type_is_exact(
                plan->semantic_plan, operation, target_by_value[value]))
            goto invalid_authority;
        exact[value] = 1;
    }
    xr_free(definition);
    xr_free(target_by_operation);
    xr_free(use_count);
    xr_free(invalid);
    *out_exact = exact;
    *out_targets = target_by_value;
    return true;

invalid_authority:
    xr_free(definition);
    xr_free(target_by_operation);
    xr_free(target_by_value);
    xr_free(use_count);
    xr_free(invalid);
    xr_free(exact);
    return report(error, error_size, "XR_TARGET_1001",
                  "direct-local callee storage authority is not exact");
}

typedef struct XrVerifierGoStore {
    uint32_t function;
    uint32_t operation;
    uint16_t slot;
    uint8_t occupied;
    uint8_t ambiguous;
} XrVerifierGoStore;

static uint32_t verifier_go_store_hash(uint32_t function, uint16_t slot) {
    uint32_t mixed = function * UINT32_C(2246822519) + (uint32_t) slot;
    mixed ^= mixed >> 15;
    return mixed;
}

static XrVerifierGoStore *verifier_go_find_store(
    XrVerifierGoStore *stores, uint32_t capacity, uint32_t function,
    uint16_t slot, bool insert) {
    uint32_t cursor = verifier_go_store_hash(function, slot) & (capacity - 1u);
    for (uint32_t probe = 0; probe < capacity; probe++) {
        XrVerifierGoStore *entry = &stores[cursor];
        if (!entry->occupied) {
            if (!insert)
                return NULL;
            entry->occupied = 1;
            entry->function = function;
            entry->slot = slot;
            entry->operation = XR_SEMANTIC_INDEX_NONE;
            return entry;
        }
        if (entry->function == function && entry->slot == slot)
            return entry;
        cursor = (cursor + 1u) & (capacity - 1u);
    }
    return NULL;
}

static bool verifier_go_store_before_activation(
    const XrSemanticPlan *semantic, uint32_t function_index,
    uint32_t store_index) {
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(semantic, function_index);
    const XrSemanticOperationRecord *store =
        xr_semantic_plan_operation(semantic, store_index);
    const XrSemanticBlockRecord *entry =
        function ? xr_semantic_plan_block(semantic, function->block_begin) : NULL;
    if (!function || !store || !entry || entry->function != function_index ||
        store->block != function->block_begin ||
        store_index < entry->operation_begin ||
        store_index >= entry->operation_begin + entry->operation_count)
        return false;
    for (uint32_t i = entry->operation_begin; i < store_index; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->opcode == XI_CALL ||
            operation->opcode == XI_TAIL_CALL ||
            operation->opcode == XI_CALL_METHOD ||
            operation->opcode == XI_CALL_METHOD_DIRECT ||
            operation->opcode == XI_CALL_BUILTIN ||
            operation->opcode == XI_GO ||
            operation->opcode == XI_THREAD_SPAWN)
            return false;
    }
    return true;
}

/* Independent verifier reconstruction for the GO-only shared callable family.
 * It deliberately does not call the builder collector or its predicates. */
static bool collect_exact_direct_local_go_callee_values(
    const XrTargetPlan *plan, uint8_t **out_exact, uint32_t **out_targets,
    char *error, size_t error_size) {
    if (out_exact)
        *out_exact = NULL;
    if (out_targets)
        *out_targets = NULL;
    const XrSemanticPlan *semantic = plan ? plan->semantic_plan : NULL;
    size_t operations_size = xr_semantic_plan_operation_count(semantic);
    size_t functions_size = xr_semantic_plan_function_count(semantic);
    if (!semantic || !out_exact || !out_targets || operations_size > UINT32_MAX ||
        functions_size > UINT32_MAX || operations_size > (1u << 24))
        return report(error, error_size, "XR_EXEC_5003",
                      "direct-local go callee verifier budget is exhausted");
    uint32_t value_count = 0;
    if (functions_size) {
        const XrSemanticFunctionRecord *last = xr_semantic_plan_function(
            semantic, (uint32_t) functions_size - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return report(error, error_size, "XR_EXEC_5003",
                          "direct-local go callee value budget overflow");
        value_count = last->value_begin + last->value_count;
    }
    uint32_t operation_count = (uint32_t) operations_size;
    uint32_t store_capacity = 1;
    while (store_capacity < operation_count * 2u)
        store_capacity <<= 1u;
    uint32_t *definition = value_count
                               ? (uint32_t *) xr_malloc(
                                     (size_t) value_count * sizeof(*definition))
                               : NULL;
    XrVerifierGoStore *stores = (XrVerifierGoStore *) xr_calloc(
        store_capacity, sizeof(*stores));
    uint32_t *targets = value_count
                            ? (uint32_t *) xr_malloc(
                                  (size_t) value_count * sizeof(*targets))
                            : NULL;
    uint32_t *uses = value_count
                         ? (uint32_t *) xr_calloc(value_count, sizeof(*uses))
                         : NULL;
    uint8_t *candidate = value_count
                             ? (uint8_t *) xr_calloc(value_count, 1)
                             : NULL;
    uint8_t *invalid = value_count ? (uint8_t *) xr_calloc(value_count, 1)
                                   : NULL;
    uint8_t *exact = value_count ? (uint8_t *) xr_calloc(value_count, 1)
                                 : NULL;
    if (!stores || (value_count && (!definition || !targets || !uses ||
                                    !candidate || !invalid || !exact)))
        goto allocation_failed;
    for (uint32_t i = 0; i < value_count; i++) {
        definition[i] = XR_SEMANTIC_INDEX_NONE;
        targets[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->result_value >= value_count ||
            definition[operation->result_value] != XR_SEMANTIC_INDEX_NONE)
            goto invalid_authority;
        definition[operation->result_value] = i;
        if (operation->opcode == XI_SET_SHARED &&
            operation->semantic_immediate >= 0 &&
            operation->semantic_immediate <= UINT16_MAX) {
            XrVerifierGoStore *entry = verifier_go_find_store(
                stores, store_capacity, operation->function,
                (uint16_t) operation->semantic_immediate, true);
            if (!entry)
                goto invalid_authority;
            if (entry->operation != XR_SEMANTIC_INDEX_NONE)
                entry->ambiguous = 1;
            else
                entry->operation = i;
        }
    }
    XrSemanticGraph graph = {0};
    if (!xr_semantic_graph_build(semantic, &graph, error, error_size))
        goto invalid_authority;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin) {
            xr_semantic_graph_dispose(&graph);
            goto invalid_authority;
        }
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand =
                &operands[use->operand_begin + a];
            if (operand->value >= value_count) {
                xr_semantic_graph_dispose(&graph);
                goto invalid_authority;
            }
            const XrSemanticOperationRecord *load =
                definition[operand->value] < operation_count
                    ? xr_semantic_plan_operation(
                          semantic, definition[operand->value])
                    : NULL;
            if (!load || load->opcode != XI_GET_SHARED || use->opcode != XI_GO)
                continue;
            uint32_t value = load->result_value;
            candidate[value] = 1;
            XrVerifierGoStore *entry =
                load->semantic_immediate >= 0 &&
                        load->semantic_immediate <= UINT16_MAX
                    ? verifier_go_find_store(
                          stores, store_capacity, load->function,
                          (uint16_t) load->semantic_immediate, false)
                    : NULL;
            const XrSemanticOperationRecord *store =
                entry && !entry->ambiguous &&
                        entry->operation < operation_count
                    ? xr_semantic_plan_operation(semantic, entry->operation)
                    : NULL;
            const XrSemanticOperandRecord *stored =
                store && store->operand_count == 1 &&
                        store->operand_begin < operand_count
                    ? &operands[store->operand_begin]
                    : NULL;
            const XrSemanticOperationRecord *closure =
                stored && stored->value < value_count &&
                        definition[stored->value] < operation_count
                    ? xr_semantic_plan_operation(
                          semantic, definition[stored->value])
                    : NULL;
            uint32_t target = closure ? closure->callable_function
                                      : XR_SEMANTIC_INDEX_NONE;
            const XrSemanticFunctionRecord *callee =
                xr_semantic_plan_function(semantic, target);
            bool initialized = store &&
                               (store->block == load->block
                                    ? entry->operation < definition[value]
                                    : xr_semantic_graph_dominates(
                                          &graph, store->block, load->block));
            bool exact_use = entry && !entry->ambiguous && store && stored &&
                closure && callee && a == 0 && initialized &&
                verifier_go_store_before_activation(
                    semantic, store->function, entry->operation) &&
                store->opcode == XI_SET_SHARED &&
                store->function == load->function &&
                store->semantic_immediate == load->semantic_immediate &&
                !store->allocation_key && stable_id_is_zero(store->allocation_id) &&
                store->constant == XR_SEMANTIC_INDEX_NONE &&
                store->callable_function == XR_SEMANTIC_INDEX_NONE &&
                store->effects == xi_generated_op_effects(XI_SET_SHARED) &&
                store->result_ownership ==
                    xi_generated_op_result_ownership(XI_SET_SHARED) &&
                stored->role == XR_SEM_OPERAND_VALUE && stored->parameter == -1 &&
                stored->transfer_mode == XR_TRANSFER_SHARE &&
                stored->ownership_action == XR_SEM_OPERAND_CONSUME &&
                stored->parameter_mode == XR_PARAM_READ &&
                stored->access == XR_CALL_ARG_PLAIN &&
                stored->origin == XI_PLACE_ORIGIN_NONE &&
                stored->lifetime == XI_PLACE_LIFETIME_NONE &&
                stored->escape == XI_PLACE_ESCAPE_NONE && stored->flags == 0 &&
                closure->function == store->function &&
                semantic_heap_closure_is_exact(semantic, closure) &&
                use->function == load->function &&
                use->operand_count == (uint16_t) (callee->parameter_count + 1u) &&
                !use->allocation_key && stable_id_is_zero(use->allocation_id) &&
                use->constant == XR_SEMANTIC_INDEX_NONE &&
                use->callable_function == XR_SEMANTIC_INDEX_NONE &&
                use->effects == xi_generated_op_effects(XI_GO) &&
                operand->value == value && operand->type == load->result_type &&
                operand->role == XR_SEM_OPERAND_VALUE && operand->parameter == -1 &&
                operand->transfer_mode == XR_TRANSFER_SHARE &&
                operand->ownership_action == XR_SEM_OPERAND_BORROW &&
                operand->parameter_mode == XR_PARAM_READ &&
                operand->access == XR_CALL_ARG_PLAIN &&
                operand->origin == XI_PLACE_ORIGIN_NONE &&
                operand->lifetime == XI_PLACE_LIFETIME_NONE &&
                operand->escape == XI_PLACE_ESCAPE_NONE && operand->flags == 0 &&
                verifier_direct_local_callee_type_is_exact(
                    semantic, load, target);
            for (uint16_t argument = 1; exact_use && argument < use->operand_count;
                 argument++) {
                const XrSemanticOperandRecord *row =
                    &operands[use->operand_begin + argument];
                const XrSemanticParameterRecord *parameter =
                    xr_semantic_plan_parameter(
                        semantic, callee->parameter_begin + argument - 1u);
                exact_use = parameter && row->type == parameter->type &&
                            row->role == XR_SEM_OPERAND_VALUE && row->parameter == -1 &&
                            row->parameter_mode == XR_PARAM_READ &&
                            row->access == XR_CALL_ARG_PLAIN &&
                            row->origin == XI_PLACE_ORIGIN_NONE &&
                            row->lifetime == XI_PLACE_LIFETIME_NONE &&
                            row->escape == XI_PLACE_ESCAPE_NONE && row->flags == 0;
            }
            if (!exact_use ||
                (targets[value] != XR_SEMANTIC_INDEX_NONE &&
                 targets[value] != target) || uses[value] == UINT32_MAX) {
                invalid[value] = 1;
                continue;
            }
            targets[value] = target;
            uses[value]++;
        }
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(semantic, i);
        for (uint16_t a = 0; use && a < use->operand_count; a++) {
            uint32_t value = operands[use->operand_begin + a].value;
            if (value < value_count && candidate[value] &&
                (use->opcode != XI_GO || a != 0))
                invalid[value] = 1;
        }
    }
    for (uint32_t i = 0;
         i < (uint32_t) xr_semantic_plan_block_count(semantic); i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(semantic, i);
        if (block && block->control_value < value_count &&
            candidate[block->control_value])
            invalid[block->control_value] = 1;
    }
    xr_semantic_graph_dispose(&graph);
    for (uint32_t i = 0; i < value_count; i++) {
        if (!candidate[i])
            continue;
        if (invalid[i] || uses[i] == 0 ||
            targets[i] == XR_SEMANTIC_INDEX_NONE)
            goto invalid_authority;
        exact[i] = 1;
    }
    xr_free(definition);
    xr_free(stores);
    xr_free(uses);
    xr_free(candidate);
    xr_free(invalid);
    *out_exact = exact;
    *out_targets = targets;
    return true;

allocation_failed:
    xr_free(definition);
    xr_free(stores);
    xr_free(targets);
    xr_free(uses);
    xr_free(candidate);
    xr_free(invalid);
    xr_free(exact);
    return report(error, error_size, "XR_EXEC_5003",
                  "direct-local go callee verifier allocation failed");
invalid_authority:
    xr_free(definition);
    xr_free(stores);
    xr_free(targets);
    xr_free(uses);
    xr_free(candidate);
    xr_free(invalid);
    xr_free(exact);
    return report(error, error_size, "XR_TARGET_1001",
                  "direct-local go callee storage authority is not exact");
}

static bool verifier_source_namespace_operation_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation, uint16_t opcode) {
    const XrSemanticTypeRecord *type = operation
        ? xr_semantic_plan_type(semantic, operation->result_type)
        : NULL;
    return semantic && operation && type && operation->opcode == opcode &&
           !operation->allocation_key && stable_id_is_zero(operation->allocation_id) &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->auxiliary_kind == 0 &&
           operation->effects == xi_generated_op_effects(opcode) &&
           operation->flags == xi_generated_op_default_flags(opcode) &&
           operation->ownership_use == xi_generated_op_own_use(opcode) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
           operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->child_count == 0 && type->aggregate_extent == 0 &&
           type->aggregate_align == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE |
                           XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           ((opcode == XI_IMPORT_REF && operation->operand_count == 0 &&
             operation->semantic_immediate >= -1 &&
             operation->semantic_immediate <= UINT16_MAX &&
             operation->metadata_count == 2) ||
            (opcode == XI_GET_SHARED && operation->operand_count == 0 &&
             operation->semantic_immediate >= 0 &&
             operation->semantic_immediate <= UINT16_MAX &&
             operation->metadata_count == 0));
}

static bool verifier_source_namespace_identity_copy_is_exact(
    const XrSemanticPlan *semantic,
    const XrSemanticOperationRecord *operation,
    const XrSemanticOperandRecord *operands, uint32_t operand_count) {
    const XrSemanticTypeRecord *type = operation
        ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    if (!semantic || !operation || !operands || !type ||
        operation->opcode != XI_COPY || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY ||
        operation->allocation_key ||
        !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 || operation->metadata_count != 0 ||
        operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->ownership_use != xi_generated_op_own_use(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE |
                        XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const XrSemanticOperandRecord *source =
        &operands[operation->operand_begin];
    return source->role == XR_SEM_OPERAND_VALUE && source->parameter == -1 &&
           source->type == operation->result_type && source->flags == 0;
}

static bool collect_exact_source_namespace_values(
    const XrTargetPlan *plan, uint8_t **out_exact, char *error,
    size_t error_size) {
    if (out_exact)
        *out_exact = NULL;
    const XrSemanticPlan *semantic = plan ? plan->semantic_plan : NULL;
    size_t function_size = xr_semantic_plan_function_count(semantic);
    size_t operation_size = xr_semantic_plan_operation_count(semantic);
    size_t target_size = xr_semantic_plan_call_target_count(semantic);
    if (!out_exact || function_size > UINT32_MAX ||
        operation_size > UINT32_MAX || target_size > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003",
                      "source namespace verifier budget is exhausted");
    uint32_t value_count = 0;
    if (function_size) {
        const XrSemanticFunctionRecord *last = xr_semantic_plan_function(
            semantic, (uint32_t) function_size - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return report(error, error_size, "XR_EXEC_5003",
                          "source namespace value budget overflow");
        value_count = last->value_begin + last->value_count;
    }
    uint32_t operation_count = (uint32_t) operation_size;
    uint32_t *definition = value_count
        ? (uint32_t *) xr_calloc(value_count, sizeof(*definition)) : NULL;
    uint32_t *target_by_operation = operation_count
        ? (uint32_t *) xr_calloc(operation_count, sizeof(*target_by_operation)) : NULL;
    uint32_t *dependency = value_count
        ? (uint32_t *) xr_calloc(value_count, sizeof(*dependency)) : NULL;
    uint32_t *expected_uses = value_count
        ? (uint32_t *) xr_calloc(value_count, sizeof(*expected_uses)) : NULL;
    uint32_t *retain_uses = value_count
        ? (uint32_t *) xr_calloc(value_count, sizeof(*retain_uses)) : NULL;
    uint32_t *consumer = value_count
        ? (uint32_t *) xr_calloc(value_count, sizeof(*consumer)) : NULL;
    uint32_t *visit_epoch = value_count
        ? (uint32_t *) xr_calloc(value_count, sizeof(*visit_epoch)) : NULL;
    uint8_t *candidate = value_count
        ? (uint8_t *) xr_calloc(value_count, sizeof(*candidate)) : NULL;
    uint8_t *exact = value_count
        ? (uint8_t *) xr_calloc(value_count, sizeof(*exact)) : NULL;
    if ((value_count && (!definition || !dependency || !expected_uses ||
                         !retain_uses || !consumer || !visit_epoch ||
                         !candidate || !exact)) ||
        (operation_count && !target_by_operation))
        goto allocation_failed;
    for (uint32_t i = 0; i < value_count; i++) {
        definition[i] = XR_SEMANTIC_INDEX_NONE;
        dependency[i] = XR_SEMANTIC_INDEX_NONE;
        consumer[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->result_value >= value_count ||
            definition[operation->result_value] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        definition[operation->result_value] = i;
    }
    for (uint32_t i = 0; i < (uint32_t) target_size; i++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(semantic, i);
        if (!target || target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
            continue;
        if (target->operation >= operation_count ||
            target->dependency >= xr_semantic_plan_dependency_count(semantic) ||
            target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata =
        xr_semantic_plan_metadata(semantic, &metadata_count);
    uint32_t next_epoch = 1;
    for (uint32_t i = 0; i < operation_count; i++) {
        uint32_t target_index = target_by_operation[i];
        if (target_index == XR_SEMANTIC_INDEX_NONE)
            continue;
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(semantic, target_index);
        const XrSemanticOperationRecord *call =
            xr_semantic_plan_operation(semantic, i);
        if (!target || !call || call->opcode != XI_CALL_METHOD ||
            call->operand_count == 0 || call->operand_begin >= operand_count)
            goto invalid;
        const XrSemanticOperandRecord *receiver = &operands[call->operand_begin];
        if (receiver->role != XR_SEM_OPERAND_RECEIVER ||
            receiver->parameter != -1 ||
            receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
            receiver->parameter_mode != XR_PARAM_READ ||
            receiver->access != XR_CALL_ARG_PLAIN ||
            (receiver->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0)
            goto invalid;
        const XrSemanticOperationRecord *load = NULL;
        uint32_t current_value = receiver->value;
        uint32_t consumer_index = i;
        uint32_t namespace_type = receiver->type;
        uint32_t epoch = next_epoch++;
        if (epoch == 0) {
            memset(visit_epoch, 0, value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= operation_count || current_value >= value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index = definition[current_value];
            const XrSemanticOperationRecord *source =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(semantic, definition_index)
                    : NULL;
            if (!source || source->result_value != current_value ||
                source->result_type != namespace_type ||
                source->function != call->function ||
                (candidate[current_value] &&
                 (dependency[current_value] != target->dependency ||
                  consumer[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            dependency[current_value] = target->dependency;
            consumer[current_value] = consumer_index;
            if (verifier_source_namespace_operation_is_exact(
                    semantic, source, XI_GET_SHARED)) {
                load = source;
                break;
            }
            if (!verifier_source_namespace_identity_copy_is_exact(
                    semantic, source, operands, operand_count))
                goto invalid;
            const XrSemanticOperandRecord *input =
                &operands[source->operand_begin];
            consumer_index = definition_index;
            current_value = input->value;
        }
        uint32_t store_index = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t j = 0; j < operation_count; j++) {
            const XrSemanticOperationRecord *store =
                xr_semantic_plan_operation(semantic, j);
            if (!store || store->function != 0 ||
                store->opcode != XI_SET_SHARED ||
                store->semantic_immediate != load->semantic_immediate)
                continue;
            if (store_index != XR_SEMANTIC_INDEX_NONE)
                goto invalid;
            store_index = j;
        }
        const XrSemanticOperationRecord *store =
            store_index != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(semantic, store_index) : NULL;
        if (!store || store->operand_count != 1 ||
            store->operand_begin >= operand_count)
            goto invalid;
        const XrSemanticOperandRecord *stored = &operands[store->operand_begin];
        const XrSemanticOperationRecord *import = NULL;
        current_value = stored->value;
        consumer_index = store_index;
        namespace_type = stored->type;
        epoch = next_epoch++;
        if (epoch == 0) {
            memset(visit_epoch, 0, value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= operation_count || current_value >= value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index = definition[current_value];
            const XrSemanticOperationRecord *source =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(semantic, definition_index)
                    : NULL;
            if (!source || source->result_value != current_value ||
                source->result_type != namespace_type ||
                source->function != 0 ||
                (candidate[current_value] &&
                 (dependency[current_value] != target->dependency ||
                  consumer[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            dependency[current_value] = target->dependency;
            consumer[current_value] = consumer_index;
            if (verifier_source_namespace_operation_is_exact(
                    semantic, source, XI_IMPORT_REF)) {
                import = source;
                break;
            }
            if (!verifier_source_namespace_identity_copy_is_exact(
                    semantic, source, operands, operand_count))
                goto invalid;
            const XrSemanticOperandRecord *input =
                &operands[source->operand_begin];
            consumer_index = definition_index;
            current_value = input->value;
        }
        const XrSemanticDependencyRecord *dep =
            xr_semantic_plan_dependency(semantic, target->dependency);
        if (!import || !load || receiver->type != load->result_type ||
            import->function != 0 || load->function != call->function ||
            store->function != 0 ||
            store->semantic_immediate != load->semantic_immediate ||
            stored->type != import->result_type ||
            stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
            stored->ownership_action != XR_SEM_OPERAND_CONSUME ||
            stored->parameter_mode != XR_PARAM_READ ||
            stored->access != XR_CALL_ARG_PLAIN || stored->flags != 0 ||
            load->result_type != import->result_type || !dep || !metadata ||
            import->metadata_begin > metadata_count ||
            import->metadata_count > metadata_count - import->metadata_begin ||
            strcmp(metadata[import->metadata_begin], dep->module_path) != 0 ||
            metadata[import->metadata_begin + 1u][0] != '\0')
            goto invalid;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value >= value_count || !candidate[operand->value])
                continue;
            const XrSemanticOperationRecord *source =
                xr_semantic_plan_operation(semantic, definition[operand->value]);
            bool expected = i == consumer[operand->value] && a == 0;
            if (expected && use->opcode == XI_CALL_METHOD)
                expected = target_by_operation[i] != XR_SEMANTIC_INDEX_NONE &&
                           xr_semantic_plan_call_target(
                               semantic, target_by_operation[i])->dependency ==
                               dependency[operand->value] &&
                           operand->role == XR_SEM_OPERAND_RECEIVER;
            else if (expected)
                expected = (use->opcode == XI_COPY ||
                            use->opcode == XI_SET_SHARED) &&
                           operand->role == XR_SEM_OPERAND_VALUE;
            if (expected) {
                if (expected_uses[operand->value] != 0)
                    goto invalid;
                expected_uses[operand->value] = 1;
                continue;
            }
            bool retain = source && source->opcode == XI_IMPORT_REF &&
                          use->opcode == XI_RETAIN && a == 0 &&
                          use->function == source->function &&
                          operand->role == XR_SEM_OPERAND_VALUE &&
                          operand->type == source->result_type &&
                          operand->parameter == -1 && operand->flags == 0;
            if (!retain || retain_uses[operand->value] != 0)
                goto invalid;
            retain_uses[operand->value] = 1;
        }
    }
    uint32_t block_count =
        (uint32_t) xr_semantic_plan_block_count(semantic);
    for (uint32_t i = 0; i < block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(semantic, i);
        if (!block || (block->control_value != XR_SEMANTIC_INDEX_NONE &&
                       (block->control_value >= value_count ||
                        candidate[block->control_value])))
            goto invalid;
    }
    for (uint32_t i = 0; i < value_count; i++) {
        if (!candidate[i])
            continue;
        const XrSemanticOperationRecord *source =
            xr_semantic_plan_operation(semantic, definition[i]);
        if (!source || expected_uses[i] != 1)
            goto invalid;
        exact[i] = 1;
    }
    xr_free(definition); xr_free(target_by_operation); xr_free(dependency);
    xr_free(expected_uses); xr_free(retain_uses); xr_free(consumer);
    xr_free(visit_epoch); xr_free(candidate);
    *out_exact = exact;
    return true;

allocation_failed:
    xr_free(definition); xr_free(target_by_operation); xr_free(dependency);
    xr_free(expected_uses); xr_free(retain_uses); xr_free(consumer);
    xr_free(visit_epoch); xr_free(candidate); xr_free(exact);
    return report(error, error_size, "XR_EXEC_5003",
                  "source namespace verifier allocation failed");
invalid:
    xr_free(definition); xr_free(target_by_operation); xr_free(dependency);
    xr_free(expected_uses); xr_free(retain_uses); xr_free(consumer);
    xr_free(visit_epoch); xr_free(candidate); xr_free(exact);
    return report(error, error_size, "XR_TARGET_1001",
                  "source namespace storage authority is not exact");
}

static bool collect_exact_dynamic_types(const XrTargetPlan *plan,
                                        const uint8_t *exact_direct_callees,
                                        const uint8_t *exact_go_callees,
                                        const uint8_t *exact_channel_values,
                                        const uint8_t *exact_source_namespaces,
                                        uint8_t **out, char *error,
                                        size_t error_size) {
    if (out)
        *out = NULL;
    size_t type_count = xr_semantic_plan_type_count(plan->semantic_plan);
    size_t operation_count =
        xr_semantic_plan_operation_count(plan->semantic_plan);
    if (!out || type_count > UINT32_MAX || operation_count > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003",
                      "dynamic-type verification budget is exhausted");
    uint8_t *exact_types = type_count
                               ? (uint8_t *) xr_calloc(type_count, sizeof(*exact_types))
                               : NULL;
    if (type_count && !exact_types)
        return report(error, error_size, "XR_EXEC_5003",
                      "dynamic-type verification allocation failed");
    for (uint32_t i = 0; i < (uint32_t) operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!operation || operation->result_type >= type_count) {
            xr_free(exact_types);
            return report(error, error_size, "XR_TARGET_1001",
                          "dynamic-type verification input is invalid");
        }
        if (semantic_heap_closure_is_exact(plan->semantic_plan, operation) ||
            semantic_string_literal_is_exact(plan->semantic_plan, operation) ||
            semantic_stringbuilder_constructor_is_exact(plan->semantic_plan,
                                                         operation) ||
            (exact_direct_callees &&
             exact_direct_callees[operation->result_value] != 0) ||
            (exact_go_callees &&
             exact_go_callees[operation->result_value] != 0) ||
            (exact_channel_values &&
             exact_channel_values[operation->result_value] != 0) ||
            (exact_source_namespaces &&
             exact_source_namespaces[operation->result_value] != 0))
            exact_types[operation->result_type] = 1;
    }
    *out = exact_types;
    return true;
}

static bool verify_value_binding(const XrTargetPlan *plan, uint32_t semantic_value,
                                 uint32_t semantic_type, uint32_t semantic_function,
                                 const XrSemanticOperationRecord *operation,
                                 const uint8_t *exact_direct_callees,
                                 const uint8_t *exact_go_callees,
                                 const uint8_t *exact_channel_values,
                                 const uint8_t *exact_channel_receives,
                                 const uint8_t *exact_source_namespaces,
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
    bool exact_heap_closure =
        semantic_heap_closure_is_exact(plan->semantic_plan, operation);
    bool exact_string_literal =
        semantic_string_literal_is_exact(plan->semantic_plan, operation);
    bool exact_stringbuilder =
        semantic_stringbuilder_constructor_is_exact(plan->semantic_plan,
                                                     operation);
    bool exact_stringbuilder_append = operation_is_exact_stringbuilder_append_rune(
        plan->semantic_plan, operation, NULL, NULL);
    bool exact_direct_callee =
        exact_direct_callees && exact_direct_callees[semantic_value] != 0;
    bool exact_go_callee =
        exact_go_callees && exact_go_callees[semantic_value] != 0;
    bool exact_channel =
        exact_channel_values && exact_channel_values[semantic_value] != 0;
    bool exact_channel_receive = exact_channel_receives &&
                                 exact_channel_receives[semantic_value] != 0;
    bool exact_source_namespace = exact_source_namespaces &&
                                  exact_source_namespaces[semantic_value] != 0;
    bool exact_string_byte_view = semantic_string_byte_slice_view_is_exact(
        plan->semantic_plan, operation);
    const XrSemanticParameterRecord *parameter = operation
                                                     ? NULL
                                                     : semantic_parameter_for_value(
                                                           plan->semantic_plan,
                                                           semantic_function,
                                                           semantic_value);
    bool exact_string_byte_parameter = semantic_u8_slice_parameter_is_exact(
        plan->semantic_plan, parameter) && parameter->type == semantic_type;
    bool exact_unit_enum = semantic_unit_enum_type_is_exact(type);
    uint16_t receive_scalar_kind = XR_MACHINE_REP_COUNT;
    bool scalar_channel_receive =
        operation && operation->opcode == XI_CHAN_TRY_RECV && type &&
        semantic_type_expected_rep(type, &receive_scalar_kind) == 1 &&
        receive_scalar_kind != XR_MACHINE_REP_VOID;
    if (scalar_channel_receive && !exact_channel_receive)
        XR_VALUE_BINDING_FAIL(10);
    int eligibility = operation_result_void || exact_heap_closure ||
                              exact_string_literal || exact_stringbuilder ||
                              exact_stringbuilder_append ||
                              exact_direct_callee || exact_go_callee ||
                              exact_channel || exact_source_namespace || exact_string_byte_view ||
                              exact_string_byte_parameter || exact_unit_enum
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
    if (exact_heap_closure || exact_string_literal || exact_stringbuilder ||
        exact_stringbuilder_append ||
        exact_direct_callee ||
        exact_go_callee ||
        exact_channel || exact_source_namespace) {
        expected_kind = XR_MACHINE_REP_DYN_VALUE;
        expected_layout = target_plan_layout_for_type(plan, semantic_type);
        eligibility = expected_layout >= 0 &&
                              plan->layouts[expected_layout].kind ==
                                  XR_TARGET_LAYOUT_DYNAMIC
                          ? 1
                          : -1;
    } else if (exact_string_byte_view || exact_string_byte_parameter) {
        expected_kind = XR_MACHINE_REP_VIEW;
        expected_layout = target_plan_layout_for_type(plan, semantic_type);
        eligibility = expected_layout >= 0 &&
                              plan->layouts[expected_layout].kind == XR_TARGET_LAYOUT_VIEW
                          ? 1
                          : -1;
    } else if (exact_unit_enum) {
        expected_kind = XR_MACHINE_REP_ENUM_ORDINAL;
        expected_layout = target_plan_layout_for_type(plan, semantic_type);
        eligibility = expected_layout >= 0 &&
                              plan->layouts[expected_layout].kind == XR_TARGET_LAYOUT_SCALAR
                          ? 1
                          : -1;
    } else if (eligibility == 0 && deferred_operation) {
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
    if (expected_kind == XR_MACHINE_REP_DYN_VALUE) {
        uint8_t expected_ownership =
            (exact_direct_callee || exact_go_callee || exact_source_namespace ||
             (exact_channel && operation && operation->opcode == XI_COPY))
                ? XR_TARGET_OWNERSHIP_BORROWED
                : XR_TARGET_OWNERSHIP_OWNED;
        if (plan->machine_reps[record->register_rep].ownership !=
                expected_ownership ||
            plan->machine_reps[record->memory_rep].ownership !=
                expected_ownership ||
            plan->machine_reps[record->register_rep].root_kind !=
                XR_TARGET_ROOT_DYNAMIC ||
            plan->machine_reps[record->memory_rep].root_kind !=
                XR_TARGET_ROOT_DYNAMIC)
            XR_VALUE_BINDING_FAIL(9);
    }
    if (expected_kind == XR_MACHINE_REP_VIEW &&
        (plan->machine_reps[record->register_rep].detail != semantic_type ||
         plan->machine_reps[record->memory_rep].detail != semantic_type ||
         plan->machine_reps[record->register_rep].root_kind != XR_TARGET_ROOT_VIEW_OWNER ||
         plan->machine_reps[record->memory_rep].root_kind != XR_TARGET_ROOT_VIEW_OWNER ||
         plan->machine_reps[record->register_rep].ownership != XR_TARGET_OWNERSHIP_BORROWED ||
         plan->machine_reps[record->memory_rep].ownership != XR_TARGET_OWNERSHIP_BORROWED))
        XR_VALUE_BINDING_FAIL(11);
    if (expected_kind == XR_MACHINE_REP_ENUM_ORDINAL &&
        (plan->machine_reps[record->register_rep].detail != semantic_type ||
         plan->machine_reps[record->memory_rep].detail != semantic_type ||
         plan->machine_reps[record->register_rep].root_kind != XR_TARGET_ROOT_NONE ||
         plan->machine_reps[record->memory_rep].root_kind != XR_TARGET_ROOT_NONE ||
         plan->machine_reps[record->register_rep].ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
         plan->machine_reps[record->memory_rep].ownership != XR_TARGET_OWNERSHIP_TRIVIAL))
        XR_VALUE_BINDING_FAIL(12);
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

static bool verify_value_reps(const XrTargetPlan *plan,
                              const uint8_t *exact_direct_callees,
                              const uint8_t *exact_go_callees,
                              const uint8_t *exact_channel_values,
                              const uint8_t *exact_channel_receives,
                              const uint8_t *exact_source_namespaces,
                              char *error, size_t error_size) {
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
                                         parameter->function, NULL,
                                         exact_direct_callees,
                                         exact_go_callees,
                                         exact_channel_values,
                                         exact_channel_receives,
                                         exact_source_namespaces, bound_slots,
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
                                             exact_direct_callees,
                                             exact_go_callees,
                                             exact_channel_values,
                                             exact_channel_receives,
                                             exact_source_namespaces, bound_slots,
                                             deferred_functions,
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

static bool verify_layouts(const XrTargetPlan *plan,
                           const uint8_t *exact_dynamic_types,
                           char *error, size_t error_size) {
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
             layout->kind != XR_TARGET_LAYOUT_AGGREGATE &&
             layout->kind != XR_TARGET_LAYOUT_DYNAMIC &&
             layout->kind != XR_TARGET_LAYOUT_VIEW) ||
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
        if (semantic_unit_enum_type_is_exact(semantic_type)) {
            scalar = 1;
            expected_rep = XR_MACHINE_REP_ENUM_ORDINAL;
        }
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
                    (expected_rep != XR_MACHINE_REP_ENUM_ORDINAL ||
                     plan->machine_reps[r].detail == layout->semantic_type) &&
                    plan->machine_reps[r].memory_size == layout->fixed_prefix_size &&
                    plan->machine_reps[r].memory_align == layout->align)
                    physical_match = true;
            if (!physical_match)
                return report(error, error_size, "XR_TARGET_1002",
                              "scalar layout disagrees with its canonical machine representation");
        } else if (layout->kind == XR_TARGET_LAYOUT_DYNAMIC) {
            bool exact_dynamic_type = exact_dynamic_types &&
                                      exact_dynamic_types[layout->semantic_type] != 0;
            uint32_t representation_count = 0;
            for (uint32_t r = 0; r < plan->machine_reps_count; r++) {
                const XrTargetMachineRepRecord *rep = &plan->machine_reps[r];
                representation_count +=
                    rep->kind == XR_MACHINE_REP_DYN_VALUE &&
                    rep->memory_size == layout->fixed_prefix_size &&
                    rep->memory_align == layout->align;
            }
            if (scalar != 0 || !exact_dynamic_type || layout->field_count != 0 ||
                layout->root_field_count != 0 || representation_count == 0)
                return report(error, error_size, "XR_TARGET_1002",
                              "dynamic value layout semantic contract is incomplete");
        } else if (layout->kind == XR_TARGET_LAYOUT_VIEW) {
            uint32_t representation_count = 0;
            for (uint32_t r = 0; r < plan->machine_reps_count; r++) {
                const XrTargetMachineRepRecord *rep = &plan->machine_reps[r];
                representation_count += rep->kind == XR_MACHINE_REP_VIEW &&
                                        rep->detail == layout->semantic_type &&
                                        rep->memory_size == 16 && rep->memory_align == 8 &&
                                        rep->register_bits == 128 &&
                                        rep->root_kind == XR_TARGET_ROOT_VIEW_OWNER &&
                                        rep->ownership == XR_TARGET_OWNERSHIP_BORROWED;
            }
            if (scalar != 0 || semantic_type->kind != XR_KIND_SLICE ||
                semantic_type->child_count != 1 || layout->field_count != 0 ||
                layout->root_field_count != 0 || layout->fixed_prefix_size != 16 ||
                layout->align != 8 || representation_count != 1)
                return report(error, error_size, "XR_TARGET_1002",
                              "view layout semantic contract is incomplete");
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

/* Independent reconstruction of the sole non-static dispatch descriptor.
 * Keep this deliberately separate from
 * the builder and from every Xi/AOT method registry. */
static bool operation_is_exact_channel_close(
    const XrSemanticPlan *semantic, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_type) {
    if (receiver_type)
        *receiver_type = XR_SEMANTIC_INDEX_NONE;
    if (!semantic || !operation)
        return false;
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata =
        xr_semantic_plan_metadata(semantic, &metadata_count);
    if (operation->opcode != XI_CALL_METHOD ||
        operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & INT64_C(1)) != 0 ||
        (uint64_t) operation->semantic_immediate > UINT32_MAX ||
        operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count || !operands || !metadata ||
        !metadata[operation->metadata_begin] ||
        strcmp(metadata[operation->metadata_begin], "close") != 0 ||
        (operation->flags & XI_FLAG_MAY_SUSPEND) != 0)
        return false;
    const XrSemanticOperandRecord *receiver =
        &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_record =
        xr_semantic_plan_type(semantic, receiver->type);
    const XrSemanticTypeRecord *result =
        xr_semantic_plan_type(semantic, operation->result_type);
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(semantic, operation->function);
    if (!receiver_record || !result || !function ||
        receiver_record->kind != XR_KIND_CHANNEL ||
        result->kind != XR_KIND_UNIT ||
        result->scalar_rep != XR_SCALAR_REP_NONE ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        (receiver->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 ||
        receiver->value < function->value_begin ||
        receiver->value >= function->value_begin + function->value_count ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count)
        return false;
    if (receiver_type)
        *receiver_type = receiver->type;
    return true;
}

static bool operation_is_exact_stringbuilder_to_string(
    const XrSemanticPlan *semantic, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value) {
    uint32_t operands_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_TO_STRING ||
        operation->opcode != XI_CALL_METHOD || operation->operand_count != 1 ||
        operation->operand_begin >= operands_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "toString") != 0 ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(semantic, receiver->type);
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(semantic, operation->result_type);
    bool exact = semantic_stringbuilder_type_is_exact(receiver_type) && result_type &&
                 result_type->kind == XR_KIND_STRING &&
                 receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
                 receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT;
    if (exact && receiver_value) *receiver_value = receiver->value;
    return exact;
}

static bool operation_is_exact_stringbuilder_append_string(
    const XrSemanticPlan *semantic,const XrSemanticOperationRecord *operation,
    uint32_t *argument_value) {
    uint32_t count=0, metadata_count=0;
    const XrSemanticOperandRecord *operands=xr_semantic_plan_operands(semantic,&count);
    const char *const *metadata=xr_semantic_plan_metadata(semantic,&metadata_count);
    if (!operation || operation->intrinsic_kind!=XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_STRING ||
        operation->operand_count!=2 || operation->operand_begin+1u>=count ||
        operation->metadata_count!=1 || operation->metadata_begin>=metadata_count ||
        strcmp(metadata[operation->metadata_begin],"append")!=0 || operation->result_alias_operand!=0)
        return false;
    const XrSemanticOperandRecord *receiver=&operands[operation->operand_begin], *argument=receiver+1;
    const XrSemanticTypeRecord *rt=xr_semantic_plan_type(semantic,receiver->type);
    const XrSemanticTypeRecord *at=xr_semantic_plan_type(semantic,argument->type);
    bool exact=semantic_stringbuilder_type_is_exact(rt)&&at&&at->kind==XR_KIND_STRING&&
        operation->result_type==receiver->type&&receiver->role==XR_SEM_OPERAND_RECEIVER&&
        receiver->flags==XR_SEM_OPERAND_CALL_CONTRACT&&argument->role==XR_SEM_OPERAND_ARGUMENT&&
        argument->flags==XR_SEM_OPERAND_CALL_CONTRACT;
    if(exact&&argument_value)*argument_value=argument->value;
    return exact;
}

static bool operation_is_exact_stringbuilder_append_rune(
    const XrSemanticPlan *semantic, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value, uint32_t *argument_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_RUNE ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 2 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "append") != 0 ||
        operation->result_alias_operand != 0 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrSemanticTypeRecord *receiver_type =
        xr_semantic_plan_type(semantic, receiver->type);
    const XrSemanticTypeRecord *argument_type =
        xr_semantic_plan_type(semantic, argument->type);
    if (!semantic_stringbuilder_type_is_exact(receiver_type) ||
        operation->result_type != receiver->type || !argument_type ||
        argument_type->kind != XR_KIND_RUNE || argument_type->builtin_type != XR_TID_NULL ||
        argument_type->child_count != 0 || argument_type->scalar_rep != XR_SCALAR_REP_NONE ||
        argument_type->flags != 0 || receiver->role != XR_SEM_OPERAND_RECEIVER ||
        receiver->parameter != -1 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    if (argument_value)
        *argument_value = argument->value;
    return true;
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
    uint8_t *state_counts =
        (uint8_t *) xr_calloc(semantic_operations, sizeof(*state_counts));
    uint8_t *suspendable =
        (uint8_t *) xr_calloc(semantic_functions, sizeof(*suspendable));
    uint32_t *reverse_head = semantic_functions
                                 ? (uint32_t *) xr_malloc(
                                       (size_t) semantic_functions *
                                       sizeof(*reverse_head))
                                 : NULL;
    uint32_t *reverse_next = semantic_targets
                                 ? (uint32_t *) xr_malloc(
                                       (size_t) semantic_targets *
                                       sizeof(*reverse_next))
                                 : NULL;
    uint32_t *queue = semantic_functions
                          ? (uint32_t *) xr_malloc(
                                (size_t) semantic_functions * sizeof(*queue))
                          : NULL;
    if ((semantic_operations && (!covered || !state_counts)) ||
        (semantic_functions && (!suspendable || !reverse_head || !queue)) ||
        (semantic_targets && !reverse_next)) {
        xr_free(covered);
        xr_free(state_counts);
        xr_free(suspendable);
        xr_free(reverse_head);
        xr_free(reverse_next);
        xr_free(queue);
        return report(error, error_size, "XR_EXEC_5003", "call verifier allocation failed");
    }
    bool valid = true;
    uint32_t expected_calls = semantic_targets;
    uint32_t semantic_entities =
        (uint32_t) xr_semantic_plan_entity_count(semantic);
    for (uint32_t i = 0; valid && i < semantic_entities; i++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(semantic, i);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        if (entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
            entity->subject >= semantic_operations ||
            ++state_counts[entity->subject] != 1)
            valid = false;
    }
    for (uint32_t function = 0; function < semantic_functions; function++)
        reverse_head[function] = XR_SEMANTIC_INDEX_NONE;
    uint32_t queue_begin = 0;
    uint32_t queue_end = 0;
    for (uint32_t operation_index = 0;
         valid && operation_index < semantic_operations; operation_index++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, operation_index);
        if (!operation || operation->function >= semantic_functions) {
            valid = false;
            break;
        }
        if (((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 ||
             operation->opcode == XI_GO) &&
            !suspendable[operation->function]) {
            suspendable[operation->function] = 1;
            queue[queue_end++] = operation->function;
        }
        if (operation_is_exact_channel_close(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (semantic_stringbuilder_constructor_is_exact(semantic, operation)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (semantic_string_byte_slice_view_is_exact(semantic, operation)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (operation_is_exact_stringbuilder_append_rune(semantic, operation, NULL, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (operation_is_exact_stringbuilder_to_string(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) { valid = false; break; }
            expected_calls++;
        }
        if (operation_is_exact_stringbuilder_append_string(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) { valid=false; break; }
            expected_calls++;
        }
    }
    valid = valid && plan->calls_count == expected_calls;
    for (uint32_t target_index = 0;
         valid && target_index < semantic_targets; target_index++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(semantic, target_index);
        const XrSemanticOperationRecord *operation =
            target && target->operation < semantic_operations
                ? xr_semantic_plan_operation(semantic, target->operation)
                : NULL;
        bool direct = target && target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL;
        bool source = target && target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT;
        if (!target || !operation || (!direct && !source) ||
            (direct && target->function >= semantic_functions) ||
            (direct && operation->opcode != XI_CALL &&
             operation->opcode != XI_TAIL_CALL) ||
            (source && operation->opcode != XI_CALL_METHOD)) {
            valid = false;
            break;
        }
        reverse_next[target_index] = XR_SEMANTIC_INDEX_NONE;
        if (direct) {
            reverse_next[target_index] = reverse_head[target->function];
            reverse_head[target->function] = target_index;
        } else if (!suspendable[operation->function]) {
            suspendable[operation->function] = 1;
            queue[queue_end++] = operation->function;
        }
    }
    while (valid && queue_begin < queue_end) {
        uint32_t callee = queue[queue_begin++];
        for (uint32_t target_index = reverse_head[callee];
             target_index != XR_SEMANTIC_INDEX_NONE;
             target_index = reverse_next[target_index]) {
            const XrSemanticCallTargetRecord *target =
                xr_semantic_plan_call_target(semantic, target_index);
            const XrSemanticOperationRecord *operation =
                target ? xr_semantic_plan_operation(semantic, target->operation)
                       : NULL;
            if (!operation || operation->function >= semantic_functions) {
                valid = false;
                break;
            }
            if (!suspendable[operation->function]) {
                suspendable[operation->function] = 1;
                queue[queue_end++] = operation->function;
            }
        }
    }
    uint32_t next_argument = 0;
    uint32_t next_adapter = 0;
    uint32_t previous_operation = XR_SEMANTIC_INDEX_NONE;
    const XrTargetMachineFacts *machine =
        xr_target_profile_machine_facts(plan->profile);
    for (uint32_t i = 0; valid && i < plan->calls_count; i++) {
        const XrTargetCallRecord *call = &plan->calls[i];
        bool semantic_target =
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE;
        const XrSemanticCallTargetRecord *target = semantic_target
                                                       ? xr_semantic_plan_call_target(
                                                             semantic,
                                                             call->semantic_call_target)
                                                       : NULL;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, call->semantic_operation);
        bool direct = target && target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL;
        bool source = target && target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT;
        const XrSemanticFunctionRecord *callee = direct
                                                      ? xr_semantic_plan_function(
                                                            semantic, target->function)
                                                      : NULL;
        const XrSemanticPlan *dependency =
            source && target->dependency < plan->semantic_dependency_count
                ? plan->semantic_dependencies[target->dependency]
                : NULL;
        const XrSemanticSourceExportRecord *source_export =
            dependency && target->source_export <
                              xr_semantic_plan_source_export_count(dependency)
                ? xr_semantic_plan_source_export(dependency,
                                                 target->source_export)
                : NULL;
        const XrSemanticFunctionRecord *source_callee =
            source_export
                ? xr_semantic_plan_function(dependency,
                                            source_export->function)
                : NULL;
        XrStableId expected_identity;
        uint32_t receiver_type_index = XR_SEMANTIC_INDEX_NONE;
        bool channel_close =
            !semantic_target && operation_is_exact_channel_close(
                           semantic, operation, &receiver_type_index);
        bool stringbuilder_constructor =
            !semantic_target &&
            semantic_stringbuilder_constructor_is_exact(semantic, operation);
        bool string_byte_slice_view = !semantic_target &&
            semantic_string_byte_slice_view_is_exact(semantic, operation);
        uint32_t append_receiver = XR_SEMANTIC_INDEX_NONE;
        uint32_t append_argument = XR_SEMANTIC_INDEX_NONE;
        bool stringbuilder_append_rune = !semantic_target &&
            operation_is_exact_stringbuilder_append_rune(
                semantic, operation, &append_receiver, &append_argument);
        uint32_t to_string_receiver = XR_SEMANTIC_INDEX_NONE;
        bool stringbuilder_to_string = !semantic_target &&
            operation_is_exact_stringbuilder_to_string(semantic, operation, &to_string_receiver);
        uint32_t append_string_argument=XR_SEMANTIC_INDEX_NONE;
        bool stringbuilder_append_string=!semantic_target&&
            operation_is_exact_stringbuilder_append_string(semantic,operation,&append_string_argument);
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
        if (stringbuilder_constructor || stringbuilder_to_string || stringbuilder_append_string) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_DYN_VALUE;
        }
        if (string_byte_slice_view) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_VIEW;
        }
        if (stringbuilder_append_rune) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_DYN_VALUE;
        }
        bool suspends = operation && call->semantic_operation < semantic_operations &&
                        state_counts[call->semantic_operation] == 1;
        bool expected_suspend =
            operation && ((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 ||
                          operation->opcode == XI_GO ||
                          source ||
                          (operation->opcode == XI_CALL && target &&
                           target->function < semantic_functions &&
                           suspendable[target->function] != 0));
        valid = operation && machine && call->semantic_operation < semantic_operations &&
                !covered[call->semantic_operation] &&
                (previous_operation == XR_SEMANTIC_INDEX_NONE ||
                 call->semantic_operation > previous_operation) &&
                operation->function < semantic_functions && result_scalar == 1 &&
                result && result->register_rep < plan->machine_reps_count &&
                result->memory_rep < plan->machine_reps_count &&
                plan->machine_reps[result->register_rep].kind == result_kind &&
                plan->machine_reps[result->memory_rep].kind == result_kind &&
                slot_binds_value_in_function(plan, result, operation->function) &&
                operation->operand_begin <= operand_count &&
                operation->operand_count <= operand_count - operation->operand_begin &&
                call->id == i &&
                call->caller_function == operation->function &&
                call->result_value == operation->result_value &&
                call->result_slot == result->slot &&
                call->caller_storage_slot == XR_SEMANTIC_INDEX_NONE &&
                call->error_slot == XR_SEMANTIC_INDEX_NONE &&
                call->argument_begin == next_argument &&
                range_valid(call->argument_begin, call->argument_count,
                            plan->call_arguments_count) &&
                (!direct || (callee &&
                             call->argument_count == callee->parameter_count)) &&
                call->adapter_begin == next_adapter && call->adapter_count == 0 &&
                call->result_register_rep == result->register_rep &&
                call->result_memory_rep == result->memory_rep &&
                call->error_register_rep < plan->machine_reps_count &&
                call->error_memory_rep < plan->machine_reps_count &&
                plan->machine_reps[call->error_register_rep].kind == XR_MACHINE_REP_VOID &&
                plan->machine_reps[call->error_memory_rep].kind == XR_MACHINE_REP_VOID &&
                call->native_abi == machine->native_abi &&
                call->result_mode == XR_TARGET_CALL_VALUE &&
                call->result_ownership ==
                    (stringbuilder_constructor ? XR_TARGET_CALL_RETURN_OWNED
                     : string_byte_slice_view ? XR_TARGET_CALL_BORROW
                                              : XR_TARGET_CALL_NONE) &&
                call->error_mode == XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL &&
                call->reserved8 == 0;
        if (!valid)
            break;
        if (direct) {
            valid = target && callee && target->operation == call->semantic_operation &&
                    target->function < semantic_functions &&
                    target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
                    (operation->opcode == XI_CALL ||
                     operation->opcode == XI_TAIL_CALL) &&
                    suspends == expected_suspend &&
                    operation->result_type == callee->return_type &&
                    operation->operand_count ==
                        (uint32_t) callee->parameter_count + 1u &&
                    callee->parameter_begin <=
                        xr_semantic_plan_parameter_count(semantic) &&
                    callee->parameter_count <=
                        xr_semantic_plan_parameter_count(semantic) -
                            callee->parameter_begin &&
                    reconstruct_call_identity("xray-target-call-v5", target->id,
                                              operation->id, 0,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->callee_function == target->function &&
                    call->argument_count == callee->parameter_count &&
                    call->flags ==
                        ((suspends ? XR_TARGET_CALL_SUSPEND : 0) |
                         (operation->opcode == XI_TAIL_CALL
                              ? XR_TARGET_CALL_TAIL
                              : 0)) &&
                    call->calling_convention ==
                        XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
                    call->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity);
            if (!valid)
                break;
            const XrSemanticOperandRecord *callee_operand =
                &operands[operation->operand_begin];
            valid = callee_operand->role == XR_SEM_OPERAND_CALLEE &&
                    callee_operand->parameter == -1 &&
                    (callee_operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0;
            for (uint32_t ordinal = 0;
                 valid && ordinal < call->argument_count; ordinal++) {
                const XrTargetCallArgumentRecord *argument =
                    &plan->call_arguments[next_argument];
                uint32_t parameter_index = callee->parameter_begin + ordinal;
                uint32_t semantic_operand =
                    operation->operand_begin + ordinal + 1u;
                const XrSemanticParameterRecord *parameter =
                    xr_semantic_plan_parameter(semantic, parameter_index);
                const XrSemanticOperandRecord *operand =
                    &operands[semantic_operand];
                const XrTargetValueRepRecord *caller_value =
                    xr_target_plan_value_rep(plan, operand->value);
                const XrTargetValueRepRecord *callee_value = parameter
                                                                 ? xr_target_plan_value_rep(
                                                                       plan,
                                                                       parameter->value)
                                                                 : NULL;
                XrStableId argument_identity;
                uint16_t argument_kind = XR_MACHINE_REP_COUNT;
                int argument_scalar =
                    operand->type < xr_semantic_plan_type_count(semantic)
                        ? semantic_type_expected_rep(
                              xr_semantic_plan_type(semantic, operand->type),
                              &argument_kind)
                        : -1;
                bool argument_u8_slice =
                    semantic_u8_slice_parameter_is_exact(semantic, parameter) &&
                    semantic_u8_slice_type_is_exact(semantic, operand->type);
                bool argument_unit_enum = parameter && parameter->type == operand->type &&
                                          semantic_unit_enum_type_is_exact(
                                              xr_semantic_plan_type(semantic, operand->type));
                uint8_t ownership =
                    operand->ownership_action == XR_SEM_OPERAND_CONSUME
                        ? XR_TARGET_CALL_CONSUME
                        : XR_TARGET_CALL_READ;
                valid = parameter &&
                        operand->role == XR_SEM_OPERAND_ARGUMENT &&
                        operand->parameter == (int16_t) ordinal &&
                        operand->type == parameter->type &&
                        operand->parameter_mode == parameter->mode &&
                        operand->transfer_mode == parameter->transfer_mode &&
                        (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0 &&
                        (argument_scalar == 1 || argument_u8_slice || argument_unit_enum) &&
                        parameter->mode == XR_PARAM_READ &&
                        operand->access == XR_CALL_ARG_PLAIN &&
                        (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) == 0 &&
                        (parameter->ownership == XI_OWN_NONE ||
                         ((argument_u8_slice || argument_unit_enum) &&
                          parameter->ownership == XI_OWN_BORROWED)) &&
                        caller_value &&
                        callee_value &&
                        slot_binds_value_in_function(
                            plan, caller_value, operation->function) &&
                        slot_binds_value_in_function(
                            plan, callee_value, target->function) &&
                        caller_value->register_rep ==
                            callee_value->register_rep &&
                        caller_value->memory_rep == callee_value->memory_rep &&
                        reconstruct_call_identity(
                            "xray-target-call-argument-v1", target->id,
                            parameter->id, ordinal, &argument_identity) &&
                        xr_stable_id_equal(argument->identity,
                                           argument_identity) &&
                        argument->call == i &&
                        argument->semantic_operand == semantic_operand &&
                        argument->semantic_value == operand->value &&
                        argument->callee_parameter == parameter_index &&
                        argument->caller_slot == caller_value->slot &&
                        argument->callee_slot == callee_value->slot &&
                        argument->register_rep == caller_value->register_rep &&
                        argument->memory_rep == caller_value->memory_rep &&
                        plan->machine_reps[argument->register_rep].kind ==
                            (argument_u8_slice
                                 ? XR_MACHINE_REP_VIEW
                                 : argument_unit_enum ? XR_MACHINE_REP_ENUM_ORDINAL
                                                      : argument_kind) &&
                        argument->ordinal == ordinal &&
                        argument->mode == XR_TARGET_CALL_VALUE &&
                        argument->ownership == ownership &&
                        argument->transfer_mode == operand->transfer_mode &&
                        argument->flags == 0;
                next_argument++;
            }
        } else if (source) {
            valid = source_export && source_callee && suspends &&
                    expected_suspend && operation->opcode == XI_CALL_METHOD &&
                    operation->operand_count ==
                        (uint32_t) source_callee->parameter_count + 1u &&
                    reconstruct_call_identity("xray-target-call-v5", target->id,
                                              operation->id, 0,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == target->dependency &&
                    call->source_export == target->source_export &&
                    xr_stable_id_equal(call->source_export_identity,
                                       target->export_identity) &&
                    xr_stable_id_equal(call->source_callee_identity,
                                       target->callee_function) &&
                    call->argument_count == 0 &&
                    call->flags == XR_TARGET_CALL_SUSPEND &&
                    call->calling_convention ==
                        XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT &&
                    call->target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT;
            if (!valid)
                break;
        } else if (channel_close) {
            const XrSemanticTypeRecord *receiver_type =
                xr_semantic_plan_type(semantic, receiver_type_index);
            valid = channel_close && receiver_type && !suspends &&
                    reconstruct_call_identity(
                        "xray-target-call-v5", operation->id,
                        receiver_type->id,
                        (uint32_t) operation->semantic_immediate,
                        &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) &&
                    call->argument_count == 0 && call->flags == 0 &&
                    call->calling_convention ==
                        XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE &&
                    call->target_kind == XR_TARGET_CALL_TARGET_CHANNEL_CLOSE;
            if (!valid)
                break;
        } else if (stringbuilder_constructor) {
            valid = stringbuilder_constructor && !suspends &&
                    reconstruct_call_identity(
                        "xray-target-stringbuilder-constructor-v1",
                        operation->id, operation->allocation_id, 0,
                        &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) &&
                    call->argument_count == 0 && call->flags == 0 &&
                    call->calling_convention ==
                        XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR &&
                    call->target_kind ==
                        XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR &&
                    result && result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind ==
                        XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership ==
                        XR_TARGET_OWNERSHIP_OWNED;
            if (!valid)
                break;
        } else if (stringbuilder_append_rune) {
            const XrSemanticTypeRecord *receiver_type =
                xr_semantic_plan_type(semantic, operation->result_type);
            valid = receiver_type && !suspends &&
                    reconstruct_call_identity(
                        "xray-target-stringbuilder-append-rune-v1",
                        operation->id, receiver_type->id, append_argument,
                        &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) &&
                    call->argument_count == 0 && call->flags == 0 &&
                    call->calling_convention ==
                        XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_RUNE &&
                    call->target_kind ==
                        XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_RUNE &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
                    result && result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            if (!valid)
                break;
        } else if (stringbuilder_to_string) {
            valid = result_type && !suspends &&
                    reconstruct_call_identity("xray-target-stringbuilder-to-string-v1",
                                              operation->id, result_type->id,
                                              to_string_receiver, &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) &&
                    call->argument_count == 0 && call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_STRINGBUILDER_TO_STRING &&
                    call->target_kind == XR_TARGET_CALL_TARGET_STRINGBUILDER_TO_STRING &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            if (!valid) break;
        } else if (stringbuilder_append_string) {
            valid=result_type&&!suspends&&reconstruct_call_identity(
                "xray-target-stringbuilder-append-string-v1",operation->id,result_type->id,
                append_string_argument,&expected_identity)&&xr_stable_id_equal(call->identity,expected_identity)&&
                call->semantic_call_target==XR_SEMANTIC_INDEX_NONE&&call->argument_count==0&&call->flags==0&&
                call->calling_convention==XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_STRING&&
                call->target_kind==XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_STRING&&
                call->result_ownership==XR_TARGET_CALL_RETURN_OWNED&&result&&result->slot<plan->slots_count&&
                plan->slots[result->slot].ownership==XR_TARGET_OWNERSHIP_OWNED;
            if(!valid)break;
        } else {
            const XrSemanticTypeRecord *view_type =
                xr_semantic_plan_type(semantic, operation->result_type);
            valid = string_byte_slice_view && view_type && !suspends &&
                    reconstruct_call_identity("xray-target-string-byte-slice-view-v1",
                                              operation->id, view_type->id, 0,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) &&
                    call->argument_count == 0 && call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_STRING_BYTE_SLICE_VIEW &&
                    call->target_kind == XR_TARGET_CALL_TARGET_STRING_BYTE_SLICE_VIEW && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_VIEW_OWNER &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_BORROWED;
            if (!valid)
                break;
        }
        covered[call->semantic_operation] = 1;
        previous_operation = call->semantic_operation;
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
    xr_free(state_counts);
    xr_free(suspendable);
    xr_free(reverse_head);
    xr_free(reverse_next);
    xr_free(queue);
    return valid || report(error, error_size, "XR_TARGET_1003",
                           "call/adapter tables do not exactly cover target authority");
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

static bool verify_coroutine_resume_shape(const XrSemanticPlan *semantic,
                                          uint32_t operation_index,
                                          const uint32_t *edge_by_block,
                                          const uint8_t *edge_counts,
                                          uint32_t block_count,
                                          uint32_t suspend_block,
                                          uint32_t resume_block,
                                          uint32_t resume_predecessor,
                                          uint16_t predecessor_ordinal) {
    uint32_t predecessor_count = 0;
    const uint32_t *predecessors =
        xr_semantic_plan_predecessors(semantic, &predecessor_count);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    const XrSemanticBlockRecord *suspend = operation
                                               ? xr_semantic_plan_block(
                                                     semantic, operation->block)
                                               : NULL;
    if (!operation || operation->block >= block_count || !suspend ||
        suspend_block != operation->block ||
        suspend->function != operation->function ||
        suspend->operation_begin != operation_index ||
        suspend->operation_count != 1 || suspend->predecessor_count != 1 ||
        suspend->predecessor_begin >= predecessor_count ||
        suspend->successors[0] == XR_SEMANTIC_INDEX_NONE ||
        resume_block != suspend->successors[0] ||
        resume_predecessor != suspend_block ||
        (suspend->successors[1] != XR_SEMANTIC_INDEX_NONE &&
         suspend->successors[1] != suspend->successors[0]))
        return false;
    const XrSemanticBlockRecord *before = xr_semantic_plan_block(
        semantic, predecessors[suspend->predecessor_begin]);
    const XrSemanticBlockRecord *resume =
        xr_semantic_plan_block(semantic, resume_block);
    if (!before || !resume || before->function != operation->function ||
        resume->function != operation->function ||
        (before->successors[0] != suspend_block &&
         before->successors[1] != suspend_block) ||
        resume->predecessor_count != 1 ||
        resume->predecessor_begin >= predecessor_count ||
        predecessor_ordinal != 0 ||
        predecessors[resume->predecessor_begin] != suspend_block ||
        edge_counts[suspend_block] != 1)
        return false;
    const XrSemanticEdgeRecord *edge =
        xr_semantic_plan_edge(semantic, edge_by_block[suspend_block]);
    return edge && edge->function == operation->function &&
           edge->from_block == suspend_block && edge->to_block == resume_block &&
           edge->operation == XR_SEMANTIC_INDEX_NONE &&
           edge->kind == XR_SEM_EDGE_NORMAL && edge->flags == 0;
}

static bool verify_coroutines(const XrTargetPlan *plan, char *error, size_t error_size) {
    const XrSemanticPlan *semantic = plan->semantic_plan;
    uint32_t function_count =
        (uint32_t) xr_semantic_plan_function_count(semantic);
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(semantic);
    uint32_t entity_count = (uint32_t) xr_semantic_plan_entity_count(semantic);
    uint32_t block_count = (uint32_t) xr_semantic_plan_block_count(semantic);
    uint32_t *function_states = function_count
                                    ? (uint32_t *) xr_calloc(
                                          function_count,
                                          sizeof(*function_states))
                                    : NULL;
    uint32_t *state_by_operation = operation_count
                                       ? (uint32_t *) xr_malloc(
                                             (size_t) operation_count *
                                             sizeof(*state_by_operation))
                                       : NULL;
    uint32_t *call_by_operation = operation_count
                                      ? (uint32_t *) xr_malloc(
                                            (size_t) operation_count *
                                            sizeof(*call_by_operation))
                                      : NULL;
    uint8_t *expected_by_operation = operation_count
                                         ? (uint8_t *) xr_calloc(
                                               operation_count,
                                               sizeof(*expected_by_operation))
                                         : NULL;
    uint8_t *call_state_counts = plan->calls_count
                                     ? (uint8_t *) xr_calloc(
                                           plan->calls_count,
                                           sizeof(*call_state_counts))
                                     : NULL;
    uint32_t *edge_by_block = block_count
                                  ? (uint32_t *) xr_malloc(
                                        (size_t) block_count *
                                        sizeof(*edge_by_block))
                                  : NULL;
    uint8_t *edge_counts = block_count
                               ? (uint8_t *) xr_calloc(
                                     block_count, sizeof(*edge_counts))
                               : NULL;
    if ((function_count && !function_states) ||
        (operation_count && (!state_by_operation || !call_by_operation ||
                             !expected_by_operation)) ||
        (plan->calls_count && !call_state_counts) ||
        (block_count && (!edge_by_block || !edge_counts))) {
        xr_free(function_states);
        xr_free(state_by_operation);
        xr_free(call_by_operation);
        xr_free(expected_by_operation);
        xr_free(call_state_counts);
        xr_free(edge_by_block);
        xr_free(edge_counts);
        return report(error, error_size, "XR_EXEC_5003",
                      "coroutine verifier allocation failed");
    }
    for (uint32_t operation = 0; operation < operation_count; operation++) {
        state_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
        call_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t block = 0; block < block_count; block++)
        edge_by_block[block] = XR_SEMANTIC_INDEX_NONE;
    bool valid = plan->functions_count == function_count;
    uint32_t semantic_edges = (uint32_t) xr_semantic_plan_edge_count(semantic);
    for (uint32_t edge_index = 0; valid && edge_index < semantic_edges;
         edge_index++) {
        const XrSemanticEdgeRecord *edge =
            xr_semantic_plan_edge(semantic, edge_index);
        if (!edge || edge->from_block >= block_count) {
            valid = false;
            break;
        }
        if (edge_counts[edge->from_block] == 0)
            edge_by_block[edge->from_block] = edge_index;
        if (edge_counts[edge->from_block] < 2)
            edge_counts[edge->from_block]++;
    }
    uint32_t expected_states = 0;
    for (uint32_t entity_index = 0; valid && entity_index < entity_count;
         entity_index++) {
        const XrSemanticEntityRecord *entity =
            xr_semantic_plan_entity(semantic, entity_index);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, entity->subject);
        if (entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
            !operation || operation->function >= function_count ||
            entity->ordinal == 0 ||
            function_states[operation->function] == UINT32_MAX) {
            valid = false;
            break;
        }
        function_states[operation->function]++;
        if (++expected_by_operation[entity->subject] != 1) {
            valid = false;
            break;
        }
        expected_states++;
    }
    for (uint32_t call = 0; valid && call < plan->calls_count; call++) {
        uint32_t operation = plan->calls[call].semantic_operation;
        if (operation >= operation_count ||
            call_by_operation[operation] != XR_SEMANTIC_INDEX_NONE) {
            valid = false;
            break;
        }
        call_by_operation[operation] = call;
    }
    valid = valid && expected_states == plan->coroutines_count;
    uint32_t next_state = 0;
    for (uint32_t function = 0; valid && function < function_count; function++) {
        const XrTargetFunctionRecord *record = &plan->functions[function];
        valid = record->coroutine_begin == next_state &&
                record->coroutine_count == function_states[function] &&
                range_valid(record->coroutine_begin, record->coroutine_count,
                            plan->coroutines_count);
        next_state += record->coroutine_count;
    }
    valid = valid && next_state == plan->coroutines_count;
    for (uint32_t state_index = 0; valid &&
                                  state_index < plan->coroutines_count;
         state_index++) {
        const XrTargetCoroutineStateRecord *state =
            &plan->coroutines[state_index];
        const XrSemanticEntityRecord *entity =
            xr_semantic_plan_entity(semantic, state->semantic_entity);
        const XrSemanticOperationRecord *operation =
            entity ? xr_semantic_plan_operation(semantic, entity->subject) : NULL;
        if (!entity || !operation ||
            entity->kind != XR_SEM_ENTITY_COROUTINE_STATE ||
            entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
            state->id != state_index || state->function != operation->function ||
            state->semantic_operation != entity->subject ||
            state->logical_state != entity->ordinal ||
            state->function >= function_count || entity->ordinal == 0 ||
            entity->ordinal > function_states[state->function] ||
            state_index != plan->functions[state->function].coroutine_begin +
                               entity->ordinal - 1u ||
            entity->subject >= operation_count ||
            state_by_operation[entity->subject] != XR_SEMANTIC_INDEX_NONE ||
            !verify_coroutine_resume_shape(
                semantic, entity->subject, edge_by_block, edge_counts,
                block_count, state->suspend_block,
                state->resume_block, state->resume_predecessor,
                state->resume_predecessor_ordinal)) {
            valid = false;
            break;
        }
        state_by_operation[entity->subject] = state_index;
        uint32_t expected_call = call_by_operation[entity->subject];
        if (expected_call != XR_SEMANTIC_INDEX_NONE &&
            (expected_call >= plan->calls_count ||
             (plan->calls[expected_call].target_kind ==
                      XR_TARGET_CALL_TARGET_SOURCE_EXPORT
                  ? operation->opcode != XI_CALL_METHOD
                  : operation->opcode != XI_CALL) ||
             plan->calls[expected_call].flags != XR_TARGET_CALL_SUSPEND)) {
            valid = false;
            break;
        }
        uint32_t expected_slot = XR_SEMANTIC_INDEX_NONE;
        uint16_t expected_flags = 0;
        const XrTargetValueRepRecord *result =
            xr_target_plan_value_rep(plan, operation->result_value);
        if (result) {
            if (result->memory_rep >= plan->machine_reps_count) {
                valid = false;
                break;
            }
            if (plan->machine_reps[result->memory_rep].kind !=
                XR_MACHINE_REP_VOID) {
                if (!slot_binds_value_in_function(plan, result,
                                                  operation->function)) {
                    valid = false;
                    break;
                }
                expected_slot = result->slot;
                expected_flags |= XR_TARGET_COROUTINE_RESULT_SLOT_BOUND;
            }
        }
        if (expected_call != XR_SEMANTIC_INDEX_NONE) {
            expected_flags |= XR_TARGET_COROUTINE_DIRECT_CHILD;
            if (plan->calls[expected_call].target_kind ==
                XR_TARGET_CALL_TARGET_SOURCE_EXPORT)
                expected_flags |= XR_TARGET_COROUTINE_SOURCE_CHILD;
            if (plan->calls[expected_call].result_slot != expected_slot ||
                plan->calls[expected_call].caller_storage_slot !=
                    XR_SEMANTIC_INDEX_NONE) {
                valid = false;
                break;
            }
            call_state_counts[expected_call]++;
        }
        valid = state->direct_call == expected_call &&
                state->result_slot == expected_slot &&
                state->flags == expected_flags;
    }
    for (uint32_t operation = 0; valid && operation < operation_count; operation++) {
        valid = (state_by_operation[operation] != XR_SEMANTIC_INDEX_NONE) ==
                (expected_by_operation[operation] == 1);
    }
    for (uint32_t call = 0; valid && call < plan->calls_count; call++) {
        uint8_t expected =
            (plan->calls[call].flags & XR_TARGET_CALL_SUSPEND) != 0;
        valid = call_state_counts[call] == expected;
    }
    xr_free(function_states);
    xr_free(state_by_operation);
    xr_free(call_by_operation);
    xr_free(expected_by_operation);
    xr_free(call_state_counts);
    xr_free(edge_by_block);
    xr_free(edge_counts);
    return valid || report(error, error_size, "XR_CORO_4000",
                           "coroutine state-call table is not exact");
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
    bool semantic_verified = plan->semantic_dependency_count == 0
                                 ? xr_semantic_plan_verify(
                                       plan->semantic_plan, nested_error,
                                       sizeof(nested_error))
                                 : xr_semantic_plan_verify_module_set(
                                       plan->semantic_plan,
                                       (const XrSemanticPlan *const *)
                                           plan->semantic_dependencies,
                                       plan->semantic_dependency_count,
                                       nested_error, sizeof(nested_error));
    if (!semantic_verified ||
        plan->semantic_dependency_count !=
            xr_semantic_plan_dependency_count(plan->semantic_plan) ||
        !xr_fingerprint_equal(plan->semantic_fingerprint,
                              xr_semantic_plan_fingerprint(plan->semantic_plan)))
        return report(error, error_size, "XR_TARGET_1000",
                      "TargetPlan semantic fingerprint does not match its exact input");
    if (!xr_target_profile_verify(plan->profile, error, error_size) ||
        !verify_resource_budgets(plan, error, error_size) ||
        !verify_machine_reps(plan, error, error_size))
        return false;
    uint8_t *exact_direct_callees = NULL;
    uint32_t *direct_callee_targets = NULL;
    if (!collect_exact_direct_local_callee_values(
            plan, &exact_direct_callees, &direct_callee_targets, error,
            error_size))
        return false;
    uint8_t *exact_go_callees = NULL;
    uint32_t *go_callee_targets = NULL;
    if (!collect_exact_direct_local_go_callee_values(
            plan, &exact_go_callees, &go_callee_targets, error,
            error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        return false;
    }
    uint8_t *exact_channel_values = NULL;
    if (!collect_exact_channel_values(plan, &exact_channel_values, error,
                                      error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        return false;
    }
    uint8_t *exact_channel_receives = NULL;
    if (!collect_exact_channel_receive_values(
            plan, exact_channel_values, &exact_channel_receives, error,
            error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        xr_free(exact_channel_values);
        return false;
    }
    uint8_t *exact_source_namespaces = NULL;
    if (!collect_exact_source_namespace_values(
            plan, &exact_source_namespaces, error, error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        xr_free(exact_channel_values);
        xr_free(exact_channel_receives);
        return false;
    }
    if (!verify_value_reps(plan, exact_direct_callees, exact_go_callees,
                           exact_channel_values, exact_channel_receives,
                           exact_source_namespaces,
                           error, error_size) ||
        !verify_extents(plan, error, error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        xr_free(exact_channel_values);
        xr_free(exact_channel_receives);
        xr_free(exact_source_namespaces);
        return false;
    }
    uint8_t *exact_dynamic_types = NULL;
    if (!collect_exact_dynamic_types(plan, exact_direct_callees,
                                     exact_go_callees,
                                     exact_channel_values,
                                     exact_source_namespaces,
                                     &exact_dynamic_types, error, error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        xr_free(exact_channel_values);
        xr_free(exact_channel_receives);
        xr_free(exact_source_namespaces);
        return false;
    }
    bool verified =
        verify_layouts(plan, exact_dynamic_types, error, error_size) &&
        verify_extent_references(plan, error, error_size) &&
        verify_storage_and_allocations(plan, error, error_size) &&
        verify_functions_and_slots(plan, error, error_size) &&
        xr_target_instruction_program_verify(plan, error, error_size) &&
        verify_calls(plan, error, error_size) &&
        verify_roots_and_cleanups(plan, error, error_size) &&
        verify_adapters_and_capabilities(plan, error, error_size) &&
        verify_coroutines(plan, error, error_size);
    xr_free(exact_dynamic_types);
    xr_free(exact_direct_callees);
    xr_free(direct_callee_targets);
    xr_free(exact_go_callees);
    xr_free(go_callee_targets);
    xr_free(exact_channel_values);
    xr_free(exact_channel_receives);
    xr_free(exact_source_namespaces);
    if (!verified)
        return false;
    XrFingerprint actual;
    xr_target_plan_compute_fingerprint(plan, &actual);
    if (!xr_fingerprint_equal(actual, plan->fingerprint))
        return report(error, error_size, "XR_TARGET_1000",
                      "TargetPlan fingerprint changed after freeze");
    return true;
}
