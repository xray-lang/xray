/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xi_intrinsic.c - Verify xi_intrinsic.def and xi_method_sym.def
 *
 * Self-contained test that generates enum/name/arity checks directly
 * from the .def files without linking against AOT sentinel symbols that
 * are unavailable in unit-test builds.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "../../src/ir/xi_intrinsic_flags.h"
#include "../../src/frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../../src/frontend/analyzer/xa_intrinsic_registry.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/value/xtype.h"
#include "../../src/shared/xr_core_intrinsic.h"
#include "../../src/shared/xr_assertion_plan.h"
#include "../../src/shared/xr_print_plan.h"
#include "../../src/shared/xr_exact_scalar_registry.h"

/* Generate enum from xi_intrinsic.def — mirrors xm_intrinsic.h */
typedef enum {
    XR_INTRIN_NONE = 0,
#define XI_INTRINSIC(name, id, arity, helper, eff, rep) XR_INTRIN_##name = id,
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
    XR_INTRIN_COUNT
} TestIntrinsicId;

/* Generate enum from xi_method_sym.def — mirrors xrt_method_symbols.h */
enum {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) TEST_SYM_##aot_name = id,
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
    TEST_SYM_COUNT_
};

static const char *g_method_sym_display_names[] = {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) display_name,
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
};

static const char *g_builtin_receiver_method_names[] = {
#define XB_RECEIVER_METHOD(id, source_name, receiver, result, p0, p1, p2, param_count, min_params, \
                           type_params, effect, receiver_mode, allocation, unsafe_requirement,     \
                           lowering)                                                               \
    source_name,
#define XB_RECEIVER_VARIADIC_METHOD(id, source_name, receiver, result, p0, p1, p2, param_count,    \
                                    min_params, type_params, effect, receiver_mode, allocation,    \
                                    unsafe_requirement, lowering)                                  \
    source_name,
#include "../../src/frontend/analyzer/xbuiltin_receiver_method.def"
#undef XB_RECEIVER_VARIADIC_METHOD
#undef XB_RECEIVER_METHOD
};

static const XaBuiltinReceiverMethodSpec *find_receiver_method(XaBuiltinReceiverKind receiver,
                                                               const char *source_name) {
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        const XaBuiltinReceiverMethodSpec *spec = &xa_builtin_receiver_methods[i];
        if (spec->receiver == receiver && strcmp(spec->source_name, source_name) == 0)
            return spec;
    }
    return NULL;
}

static bool receiver_has_method(XaBuiltinReceiverKind receiver, const char *source_name) {
    return find_receiver_method(receiver, source_name) != NULL;
}

static bool registry_has_source_name(const char *source_name) {
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        if (strcmp(xa_builtin_receiver_methods[i].source_name, source_name) == 0)
            return true;
    }
    return false;
}

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL: %s\n", msg);                                                  \
            g_failed++;                                                                            \
        } else {                                                                                   \
            g_passed++;                                                                            \
        }                                                                                          \
    } while (0)

/* ========== xi_intrinsic.def tests ========== */

/* Verify every intrinsic ID is unique and matches its expected value. */
static void test_enum_values(void) {
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                                            \
    ASSERT_TRUE(XR_INTRIN_##name == id, "XR_INTRIN_" #name " enum value mismatch");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Verify all IDs are positive and below the sentinel. */
static void test_id_range(void) {
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                                            \
    ASSERT_TRUE(id > 0 && id < XR_INTRIN_COUNT, "XR_INTRIN_" #name " ID must be in (0, COUNT)");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Verify no duplicate IDs by checking a boolean table. */
static void test_no_duplicate_ids(void) {
    bool seen[256];
    memset(seen, 0, sizeof(seen));
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                                            \
    do {                                                                                           \
        ASSERT_TRUE(id < 256, "XR_INTRIN_" #name " ID out of table range");                        \
        if (id < 256) {                                                                            \
            ASSERT_TRUE(!seen[id], "XR_INTRIN_" #name " duplicate ID detected");                   \
            seen[id] = true;                                                                       \
        }                                                                                          \
    } while (0);
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Verify arity is sensible (-1 for variadic, >= 0 otherwise). */
static void test_arity_valid(void) {
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                                            \
    ASSERT_TRUE(arity >= -1 && arity <= 16, "XR_INTRIN_" #name " arity out of range");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Count total intrinsics for a sanity check. */
static void test_intrinsic_count(void) {
    int count = 0;
#define XI_INTRINSIC(name, id, arity, helper, eff, rep) count++;
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
    ASSERT_TRUE(count >= 20, "expected at least 20 intrinsics in .def");
    printf("  intrinsic count: %d\n", count);
}

/* Verify effect flags are valid (no undefined bits). */
static void test_effect_flags(void) {
    const int VALID_MASK = IEFF_R | IEFF_W | IEFF_T | IEFF_IO | IEFF_A;
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                                            \
    ASSERT_TRUE(((eff) & ~VALID_MASK) == 0, "XR_INTRIN_" #name " has invalid effect bits");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* Verify return reps are valid. */
static void test_ret_rep_valid(void) {
#define XI_INTRINSIC(name, id, arity, helper, eff, rep)                                            \
    ASSERT_TRUE((rep) >= IREP_VAL && (rep) <= IREP_I64, "XR_INTRIN_" #name " invalid ret_rep");
#include "../../src/ir/xi_intrinsic.def"
#undef XI_INTRINSIC
}

/* ========== xi_method_sym.def tests ========== */

/* Verify method symbol IDs are positive. */
static void test_method_sym_ids(void) {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name)                                         \
    ASSERT_TRUE(TEST_SYM_##aot_name == id, "TEST_SYM_" #aot_name " value mismatch");               \
    ASSERT_TRUE(id > 0, "TEST_SYM_" #aot_name " ID must be positive");
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
}

/* Verify no duplicate method symbol IDs. */
static void test_method_sym_no_dups(void) {
    bool seen[256];
    memset(seen, 0, sizeof(seen));
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name)                                         \
    do {                                                                                           \
        if (id < 256) {                                                                            \
            ASSERT_TRUE(!seen[id], "TEST_SYM_" #aot_name " duplicate ID");                         \
            seen[id] = true;                                                                       \
        }                                                                                          \
    } while (0);
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
}

/* Verify display names are non-NULL and non-empty. */
static void test_method_sym_names(void) {
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name)                                         \
    ASSERT_TRUE(display_name != NULL && display_name[0] != '\0',                                   \
                "TEST_SYM_" #aot_name " display name empty");
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
}

/* Count method symbols. */
static void test_method_sym_count(void) {
    int count = 0;
#define XI_METHOD_SYM(aot_name, id, rt_name, display_name) count++;
#include "../../src/ir/xi_method_sym.def"
#undef XI_METHOD_SYM
    ASSERT_TRUE(count >= 50, "expected at least 50 method symbols in .def");
    printf("  method symbol count: %d\n", count);
}

static bool method_sym_display_name_exists(const char *name) {
    if (!name)
        return false;
    for (size_t i = 0;
         i < sizeof(g_method_sym_display_names) / sizeof(g_method_sym_display_names[0]); i++) {
        if (strcmp(g_method_sym_display_names[i], name) == 0)
            return true;
    }
    return false;
}

static void test_string_builder_append_symbol_identity(void) {
    ASSERT_TRUE(TEST_SYM_APPEND == 253,
                "StringBuilder.append must keep stable method symbol 253");
    ASSERT_TRUE(SYMBOL_APPEND == 253 && xr_builtin_symbol_from_name("append") == SYMBOL_APPEND,
                "runtime and Xi must share the stable append symbol");
    ASSERT_TRUE(method_sym_display_name_exists("append"),
                "stable append identity must project its source spelling");
}

static void test_builtin_receiver_registry_method_symbols(void) {
    for (size_t i = 0;
         i < sizeof(g_builtin_receiver_method_names) / sizeof(g_builtin_receiver_method_names[0]);
         i++) {
        char msg[192];
        snprintf(msg, sizeof(msg), "receiver registry method '%s' missing xi_method_sym.def entry",
                 g_builtin_receiver_method_names[i]);
        ASSERT_TRUE(method_sym_display_name_exists(g_builtin_receiver_method_names[i]), msg);
    }
}

static void test_builtin_receiver_registry_method_ids(void) {
    ASSERT_TRUE(xa_builtin_receiver_method_count() == XA_BUILTIN_RECEIVER_METHOD_COUNT,
                "receiver registry method id count must match table count");
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        char msg[192];
        const XaBuiltinReceiverMethodSpec *spec =
            xa_builtin_receiver_method_by_id((XaBuiltinReceiverMethodId) i);
        snprintf(msg, sizeof(msg), "receiver registry method id %zu missing table entry", i);
        ASSERT_TRUE(spec != NULL, msg);
        if (!spec)
            continue;
        snprintf(msg, sizeof(msg), "receiver registry method '%s' id/table order mismatch",
                 spec->id);
        ASSERT_TRUE(spec->method_id == (XaBuiltinReceiverMethodId) i, msg);
    }

    const XaBuiltinReceiverMethodSpec *append =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM);
    ASSERT_TRUE(append && append->receiver == XA_BUILTIN_RECEIVER_U8_ARRAY &&
                    strcmp(append->source_name, "appendFrom") == 0,
                "U8_ARRAY_APPEND_FROM registry id must resolve appendFrom");
    const XaBuiltinReceiverMethodSpec *repeat =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM);
    ASSERT_TRUE(repeat && repeat->receiver == XA_BUILTIN_RECEIVER_U8_ARRAY &&
                    strcmp(repeat->source_name, "repeatFrom") == 0,
                "U8_ARRAY_REPEAT_FROM registry id must resolve repeatFrom");
}

static void test_builtin_receiver_registry_metadata(void) {
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        char msg[192];
        const XaBuiltinReceiverMethodSpec *spec =
            xa_builtin_receiver_method_by_id((XaBuiltinReceiverMethodId) i);
        if (!spec)
            continue;

        XaBuiltinMethodDocumentationGroup group =
            xa_builtin_receiver_method_documentation_group(spec);
        XaBuiltinMethodProfileAvailability profile =
            xa_builtin_receiver_method_profile_availability(spec);
        snprintf(msg, sizeof(msg), "receiver registry method '%s' missing documentation group",
                 spec->id);
        ASSERT_TRUE(xa_builtin_receiver_documentation_group_label(group)[0] != '\0', msg);
        snprintf(msg, sizeof(msg), "receiver registry method '%s' missing profile label", spec->id);
        ASSERT_TRUE(xa_builtin_receiver_profile_availability_label(profile)[0] != '\0', msg);
        snprintf(msg, sizeof(msg), "receiver registry method '%s' missing effect label", spec->id);
        ASSERT_TRUE(xa_builtin_receiver_effect_label(spec->effect)[0] != '\0', msg);
        snprintf(msg, sizeof(msg), "receiver registry method '%s' has invalid receiver mode",
                 spec->id);
        ASSERT_TRUE(xr_param_mode_is_valid(spec->receiver_mode), msg);
        snprintf(msg, sizeof(msg), "receiver registry method '%s' missing allocation label",
                 spec->id);
        ASSERT_TRUE(xa_builtin_receiver_allocation_label(spec->allocation)[0] != '\0', msg);
        snprintf(msg, sizeof(msg), "receiver registry method '%s' missing unsafe label", spec->id);
        ASSERT_TRUE(
            xa_builtin_receiver_unsafe_requirement_label(spec->unsafe_requirement)[0] != '\0', msg);
    }

    const XaBuiltinReceiverMethodSpec *append =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM);
    const XaBuiltinReceiverMethodSpec *popcount =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_EXACT_INT_POPCOUNT);
    ASSERT_TRUE(append && append->receiver_mode == XR_PARAM_REF,
                "appendFrom must explicitly require a REF receiver");
    ASSERT_TRUE(popcount && popcount->receiver_mode == XR_PARAM_READ,
                "popcount must explicitly preserve a READ receiver");
    ASSERT_TRUE(append && xa_builtin_receiver_method_documentation_group(append) ==
                              XA_BUILTIN_DOC_GROUP_U8_ARRAY,
                "appendFrom must document under Array<u8> byte bulk methods");
    ASSERT_TRUE(append && xa_builtin_receiver_method_profile_availability(append) ==
                              XA_BUILTIN_PROFILE_HEAP_CAPABLE,
                "appendFrom must be marked heap-capable");

    const XaBuiltinReceiverMethodSpec *common_prefix =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COMMON_PREFIX);
    ASSERT_TRUE(common_prefix && xa_builtin_receiver_method_documentation_group(common_prefix) ==
                                     XA_BUILTIN_DOC_GROUP_U8_SLICE,
                "commonPrefix must document under Slice<u8> byte range methods");
    ASSERT_TRUE(common_prefix && xa_builtin_receiver_method_profile_availability(common_prefix) ==
                                     XA_BUILTIN_PROFILE_ALL,
                "commonPrefix must be available in all profiles");
}

static void test_builtin_receiver_method_placement(void) {
    const XaBuiltinReceiverMethodSpec *map_entries =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_MAP_ENTRIES_ITERATOR);
    ASSERT_TRUE(map_entries && map_entries->receiver == XA_BUILTIN_RECEIVER_MAP &&
                    map_entries->result == XA_BUILTIN_TYPE_ITERATOR_OF_MAP_ENTRY_TUPLE &&
                    map_entries->param_count == 0 && map_entries->min_params == 0 &&
                    map_entries->effect == XA_BUILTIN_EFFECT_READS_RECEIVER &&
                    map_entries->allocation == XA_BUILTIN_ALLOCATION_MAY_HEAP &&
                    strcmp(map_entries->source_name, "entriesIterator") == 0,
                "Map entriesIterator must have one generated exact K/V iterator row");
    ASSERT_TRUE(find_receiver_method(XA_BUILTIN_RECEIVER_MAP, "entriesIterator") == map_entries,
                "Map entriesIterator lookup must resolve its generated typed row");
    ASSERT_TRUE(find_receiver_method(XA_BUILTIN_RECEIVER_U8_ARRAY, "entriesIterator") == NULL,
                "a different receiver family must not acquire the Map entry row");

    const char *exact_bit_methods[] = {"rotateLeft",   "rotateRight",   "byteswap", "popcount",
                                       "leadingZeros", "trailingZeros", NULL};
    for (int i = 0; exact_bit_methods[i]; i++) {
        char msg[192];
        snprintf(msg, sizeof(msg), "exact integer registry must contain %s", exact_bit_methods[i]);
        ASSERT_TRUE(receiver_has_method(XA_BUILTIN_RECEIVER_EXACT_INTEGER, exact_bit_methods[i]),
                    msg);
    }
    const XaBuiltinReceiverMethodSpec *rotate =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_EXACT_INT_ROTATE_LEFT);
    ASSERT_TRUE(rotate && rotate->result == XA_BUILTIN_TYPE_RECEIVER &&
                    rotate->params[0] == XA_BUILTIN_TYPE_INT &&
                    rotate->allocation == XA_BUILTIN_ALLOCATION_NO_HEAP,
                "rotateLeft must preserve its exact receiver and be no-heap");
    const XaBuiltinReceiverMethodSpec *popcount =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_EXACT_INT_POPCOUNT);
    ASSERT_TRUE(popcount && popcount->result == XA_BUILTIN_TYPE_INT,
                "popcount must return language i64");
    const XaBuiltinReceiverMethodSpec *mul_high =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_EXACT_UINT_MUL_HIGH);
    ASSERT_TRUE(mul_high && mul_high->receiver == XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER &&
                    mul_high->result == XA_BUILTIN_TYPE_RECEIVER &&
                    mul_high->params[0] == XA_BUILTIN_TYPE_RECEIVER &&
                    mul_high->allocation == XA_BUILTIN_ALLOCATION_NO_HEAP,
                "mulHigh must be unsigned-only, exact-width, receiver-preserving and no-heap");

    const char *u8_array_methods[] = {"appendFrom", "repeatFrom", NULL};
    for (int i = 0; u8_array_methods[i]; i++) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Array<u8> registry must contain %s", u8_array_methods[i]);
        ASSERT_TRUE(receiver_has_method(XA_BUILTIN_RECEIVER_U8_ARRAY, u8_array_methods[i]), msg);
    }

    const char *array_forbidden_range_methods[] = {
        "load", "store", "copyFrom", "compare", "commonPrefix", "reinterpret", NULL};
    for (int i = 0; array_forbidden_range_methods[i]; i++) {
        char msg[192];
        snprintf(msg, sizeof(msg), "Array<u8> registry must not own range method %s",
                 array_forbidden_range_methods[i]);
        ASSERT_TRUE(
            !receiver_has_method(XA_BUILTIN_RECEIVER_U8_ARRAY, array_forbidden_range_methods[i]),
            msg);
    }

    const char *u8_slice_methods[] = {"load",       "store",       "fill",
                                      "copyFrom",   "compare",     "commonPrefix",
                                      "repeatFrom", "reinterpret", NULL};
    for (int i = 0; u8_slice_methods[i]; i++) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Slice<u8> registry must contain %s", u8_slice_methods[i]);
        ASSERT_TRUE(receiver_has_method(XA_BUILTIN_RECEIVER_U8_SLICE, u8_slice_methods[i]), msg);
    }

    const char *slice_forbidden_grow_methods[] = {"appendFrom", "push", "reserve", "resize", NULL};
    for (int i = 0; slice_forbidden_grow_methods[i]; i++) {
        char msg[192];
        snprintf(msg, sizeof(msg), "Slice<u8> registry must not own grow method %s",
                 slice_forbidden_grow_methods[i]);
        ASSERT_TRUE(
            !receiver_has_method(XA_BUILTIN_RECEIVER_U8_SLICE, slice_forbidden_grow_methods[i]),
            msg);
    }

    const char *domain_methods[] = {"fromUtf8", "fromUtf8Lossy", "base64",  "hex",
                                    "compress", "decompress",    "hash",    "md5",
                                    "sha256",   "encrypt",       "decrypt", NULL};
    for (int i = 0; domain_methods[i]; i++) {
        char msg[192];
        snprintf(msg, sizeof(msg), "domain algorithm %s must stay out of receiver registry",
                 domain_methods[i]);
        ASSERT_TRUE(!registry_has_source_name(domain_methods[i]), msg);
    }
}

static void test_semantic_intrinsic_registry(void) {
    char error[192];
    ASSERT_TRUE(xa_intrinsic_registry_validate(error, sizeof(error)),
                "semantic intrinsic registry must be internally consistent");
    ASSERT_TRUE(xa_intrinsic_count() >= 51,
                "semantic intrinsic registry must contain the portable SIMD surface");

    const XaIntrinsicDesc *xor_desc = xa_intrinsic_by_key("simd.U32x4.bitXor");
    ASSERT_TRUE(xor_desc && xor_desc->id == XA_INTRINSIC_SIMD_U32X4_BIT_XOR,
                "canonical SIMD identity must resolve by declaration key");
    ASSERT_TRUE(xor_desc && xor_desc->lowering == XA_INTRINSIC_LOWERING_VEC_BIT_XOR &&
                    xor_desc->effect == XA_INTRINSIC_EFFECT_PURE &&
                    xor_desc->shape_rule.input_lanes == 4 &&
                    xor_desc->shape_rule.result_native_type == XR_NATIVE_U32,
                "SIMD descriptor must publish lowering/effect/shape before Xi construction");
    ASSERT_TRUE(xa_intrinsic_by_id(XA_INTRINSIC_SIMD_U32X4_BIT_XOR) == xor_desc,
                "semantic intrinsic id and key lookup must share one descriptor");

    const XaIntrinsicDesc *cap = xa_intrinsic_by_id(XA_INTRINSIC_SIMD_CAPABILITIES_NATIVE_BYTES);
    ASSERT_TRUE(cap && cap->family == XA_INTRINSIC_FAMILY_TARGET &&
                    cap->lowering == XA_INTRINSIC_LOWERING_TARGET_SIMD_BYTES &&
                    (cap->flags & XA_INTRINSIC_FLAG_STATIC_RECEIVER) != 0,
                "target capability must be an explicit semantic identity, not a late name match");

    const XaIntrinsicDesc *accelerated =
        xa_intrinsic_by_id(XA_INTRINSIC_SIMD_CAPABILITIES_IS_ACCELERATED);
    ASSERT_TRUE(accelerated && accelerated->family == XA_INTRINSIC_FAMILY_TARGET &&
                    accelerated->lowering == XA_INTRINSIC_LOWERING_TARGET_SIMD_ACCELERATED &&
                    (accelerated->flags & XA_INTRINSIC_FLAG_STATIC_RECEIVER) != 0,
                "SIMD acceleration availability must be a compile-target semantic identity");

    const XaIntrinsicDesc *runtime_selected =
        xa_intrinsic_by_id(XA_INTRINSIC_SIMD_CAPABILITIES_IS_RUNTIME_SELECTED);
    ASSERT_TRUE(runtime_selected && runtime_selected->family == XA_INTRINSIC_FAMILY_TARGET &&
                    runtime_selected->lowering ==
                        XA_INTRINSIC_LOWERING_TARGET_SIMD_RUNTIME_SELECTED &&
                    runtime_selected->effect == XA_INTRINSIC_EFFECT_PURE &&
                    (runtime_selected->flags & XA_INTRINSIC_FLAG_STATIC_RECEIVER) != 0,
                "runtime SIMD selection must be an explicit target-mode semantic identity");

    const XaIntrinsicDesc *rotl = xa_intrinsic_by_id(XA_INTRINSIC_BITS_ROTATE_LEFT);
    ASSERT_TRUE(rotl && rotl->family == XA_INTRINSIC_FAMILY_BITS &&
                    rotl->lowering == XA_INTRINSIC_LOWERING_BIT_ROTL &&
                    rotl->effect == XA_INTRINSIC_EFFECT_PURE && rotl->min_arity == 1 &&
                    rotl->max_arity == 1,
                "exact integer bit semantics must be represented in the canonical registry");

    const XaIntrinsicDesc *slice_copy = xa_intrinsic_by_id(XA_INTRINSIC_BYTE_SLICE_COPY);
    ASSERT_TRUE(slice_copy && slice_copy->family == XA_INTRINSIC_FAMILY_MEMORY &&
                    slice_copy->lowering == XA_INTRINSIC_LOWERING_BYTE_SLICE_COPY &&
                    slice_copy->effect == XA_INTRINSIC_EFFECT_WRITE_MAY_THROW &&
                    slice_copy->min_arity == 1 && slice_copy->max_arity == 1,
                "Slice<u8>.copyFrom must have one stable memory identity");

    const XaIntrinsicDesc *pod_ptr = xa_intrinsic_by_id(XA_INTRINSIC_POD_SLICE_PTR);
    ASSERT_TRUE(pod_ptr && pod_ptr->family == XA_INTRINSIC_FAMILY_MEMORY &&
                    pod_ptr->lowering == XA_INTRINSIC_LOWERING_SLICE_DATA_PTR &&
                    pod_ptr->effect == XA_INTRINSIC_EFFECT_PURE,
                "Slice<POD>.ptr must be canonical before Xi construction");

    const XaIntrinsicDesc *atomic_fetch_add = xa_intrinsic_by_id(XA_INTRINSIC_ATOMIC_FETCH_ADD);
    ASSERT_TRUE(atomic_fetch_add && atomic_fetch_add->family == XA_INTRINSIC_FAMILY_ATOMIC &&
                    atomic_fetch_add->lowering == XA_INTRINSIC_LOWERING_ATOMIC_RMW &&
                    atomic_fetch_add->effect == XA_INTRINSIC_EFFECT_READ_WRITE &&
                    atomic_fetch_add->allocation == XA_INTRINSIC_ALLOCATION_NO_ALLOC,
                "Atomic.fetchAdd must publish stable nothrow RMW semantics");

    const XaIntrinsicDesc *builder_append =
        xa_intrinsic_by_id(XA_INTRINSIC_STRING_BUILDER_APPEND);
    ASSERT_TRUE(builder_append && builder_append->id == 6007 &&
                    builder_append->family == XA_INTRINSIC_FAMILY_CORE &&
                    builder_append->lowering == XA_INTRINSIC_LOWERING_STRING_BUILDER_APPEND &&
                    builder_append->effect == XA_INTRINSIC_EFFECT_WRITE_ONLY &&
                    builder_append->allocation == XA_INTRINSIC_ALLOCATION_MAY_ALLOC &&
                    builder_append->min_arity == 1 && builder_append->max_arity == 1,
                "StringBuilder.append must publish one typed core intrinsic identity");
    ASSERT_TRUE(builder_append &&
                    strcmp(xa_intrinsic_source_member(builder_append),
                           XA_INTRINSIC_STRING_BUILDER_APPEND_SOURCE_MEMBER) == 0,
                "the registry and pointer-free verifier must share one append projection");
    XrType builtin_builder = {
        .kind = XR_KIND_INSTANCE,
        .instance = {.class_name = "StringBuilder"},
    };
    XrType shadow_builder = {
        .kind = XR_KIND_INSTANCE,
        .instance = {.class_name = "StringBuilder", .class_ref = (XrClassInfo *) (uintptr_t) 1},
    };
    ASSERT_TRUE(xa_intrinsic_compiler_receiver_method(&builtin_builder, "append") ==
                    XA_INTRINSIC_STRING_BUILDER_APPEND,
                "builtin receiver binding must select append by typed receiver and stable symbol");
    ASSERT_TRUE(xa_intrinsic_compiler_receiver_method(&shadow_builder, "append") ==
                    XA_INTRINSIC_NONE,
                "a source class with the same spelling must not acquire builtin append semantics");

    const XaIntrinsicDesc *par_map_into = xa_intrinsic_by_id(XA_INTRINSIC_PARALLEL_PLAN_MAP_INTO);
    ASSERT_TRUE(par_map_into && par_map_into->family == XA_INTRINSIC_FAMILY_PARALLEL &&
                    par_map_into->lowering == XA_INTRINSIC_LOWERING_PAR_MAP_INTO &&
                    (par_map_into->flags & XA_INTRINSIC_FLAG_PLAN_RECEIVER) != 0 &&
                    par_map_into->min_arity == 3 && par_map_into->max_arity == 3,
                "parallel Plan.mapInto must publish receiver, arity, and lowering identity");

    const XaIntrinsicDesc *i64_parse = xa_intrinsic_by_id(XA_INTRINSIC_I64_PARSE);
    const XaIntrinsicDesc *i64_try_parse = xa_intrinsic_by_id(XA_INTRINSIC_I64_TRY_PARSE);
    const XaIntrinsicDesc *f64_parse = xa_intrinsic_by_id(XA_INTRINSIC_F64_PARSE);
    const XaIntrinsicDesc *f64_try_parse = xa_intrinsic_by_id(XA_INTRINSIC_F64_TRY_PARSE);
    ASSERT_TRUE(i64_parse && f64_parse &&
                    i64_parse->effect == XA_INTRINSIC_EFFECT_MAY_THROW &&
                    f64_parse->effect == XA_INTRINSIC_EFFECT_MAY_THROW &&
                    i64_parse->allocation == XA_INTRINSIC_ALLOCATION_MAY_ALLOC &&
                    f64_parse->allocation == XA_INTRINSIC_ALLOCATION_MAY_ALLOC,
                "required scalar parse must account for typed-error aggregate allocation");
    ASSERT_TRUE(i64_try_parse && f64_try_parse &&
                    i64_try_parse->effect == XA_INTRINSIC_EFFECT_PURE &&
                    f64_try_parse->effect == XA_INTRINSIC_EFFECT_PURE &&
                    i64_try_parse->allocation == XA_INTRINSIC_ALLOCATION_NO_ALLOC &&
                    f64_try_parse->allocation == XA_INTRINSIC_ALLOCATION_NO_ALLOC,
                "optional scalar parse must stay nothrow and allocation-free");
}

static void test_core_intrinsic_registry(void) {
    char error[192];
    ASSERT_TRUE(xr_core_intrinsic_registry_validate(error, sizeof(error)),
                "core intrinsic registry must be complete and internally consistent");
    ASSERT_TRUE(xr_core_intrinsic_count() == XR_CORE_BUILTIN_COUNT,
                "core intrinsic registry must expose every stable builtin ID");

    const XrCoreIntrinsicDesc *assert_equal =
        xr_core_intrinsic_by_source_name("assertEqual", strlen("assertEqual"));
    ASSERT_TRUE(assert_equal && assert_equal->id == XR_CORE_BUILTIN_ASSERT_EQUAL &&
                    assert_equal->parameter_shape ==
                        XR_CORE_INTRINSIC_PARAMETER_SHAPE_SAME_TYPE_PAIR_OPTIONAL_MESSAGE &&
                    assert_equal->semantic_op == XR_CORE_INTRINSIC_SEMANTIC_OP_ASSERT_EQUAL,
                "assertEqual must expose one typed source binding descriptor");
    ASSERT_TRUE(xr_core_intrinsic_by_id(XR_CORE_BUILTIN_ASSERT_EQUAL) == assert_equal,
                "core builtin ID and source-name lookup must share one descriptor");

    const XrCoreIntrinsicDesc *assert_throws =
        xr_core_intrinsic_by_id(XR_CORE_BUILTIN_ASSERT_THROWS);
    const XrCoreIntrinsicDesc *assert_panics =
        xr_core_intrinsic_by_id(XR_CORE_BUILTIN_ASSERT_PANICS);
    ASSERT_TRUE(assert_throws && assert_panics &&
                    assert_throws->expected_failure_channel ==
                        XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR &&
                    assert_panics->expected_failure_channel ==
                        XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC &&
                    assert_throws->expected_failure_channel !=
                        assert_panics->expected_failure_channel,
                "typed-error and panic assertions must carry distinct failure channels");

    const XrCoreIntrinsicDesc *print =
        xr_core_intrinsic_by_source_name("print", strlen("print"));
    ASSERT_TRUE(print && print->call_form == XR_CORE_INTRINSIC_CALL_FORM_DIRECT_ONLY &&
                    print->min_arity == 0 && print->max_arity == UINT16_MAX &&
                    print->target_applicability == XR_CORE_INTRINSIC_TARGET_OUTPUT_ALL,
                "print must be direct-only because heterogeneous arguments need one call-site plan");

    const char *removed[] = {"likely",       "unlikely",   "assert_true",  "assert_false",
                             "assert_eq",    "assert_ne",   "assert_throws", NULL};
    for (size_t i = 0; removed[i]; i++) {
        ASSERT_TRUE(!xr_core_intrinsic_by_source_name(removed[i], strlen(removed[i])),
                    "removed source name must not enter the core intrinsic registry");
    }
}

static XrLocation assertion_test_location(void) {
    XrLocation source = {"assertion_contract.xr", 7, 3, 7, 24};
    return source;
}

static void test_assertion_plans_are_registry_projections(void) {
    XrAssertionPlan condition;
    XrAssertionPlan equal;
    ASSERT_TRUE(xr_assertion_plan_build(XR_CORE_BUILTIN_ASSERT, 2, assertion_test_location(),
                                        XR_CORE_INTRINSIC_TARGET_VM, 0, &condition) ==
                    XR_ASSERTION_PLAN_OK,
                "assert plan must derive from the stable builtin row");
    ASSERT_TRUE(condition.kind == XR_ASSERTION_KIND_CONDITION &&
                    condition.flow_rule == XR_CORE_INTRINSIC_FLOW_ASSERT_TRUE &&
                    condition.message_operand == 1 && condition.evaluation_count == 2 &&
                    condition.evaluation_order[0] == XR_ASSERTION_EVAL_CONDITION &&
                    condition.evaluation_order[1] == XR_ASSERTION_EVAL_MESSAGE,
                "assert must retain left-to-right message evaluation and flow refinement");

    ASSERT_TRUE(xr_assertion_plan_build(XR_CORE_BUILTIN_ASSERT_EQUAL, 3,
                                        assertion_test_location(),
                                        XR_CORE_INTRINSIC_TARGET_AOT_HOSTED, 0, &equal) ==
                    XR_ASSERTION_PLAN_OK,
                "assertEqual plan must derive from the stable builtin row");
    ASSERT_TRUE(equal.kind == XR_ASSERTION_KIND_EQUAL &&
                    equal.equality_authority == XR_ASSERTION_EQUALITY_LANGUAGE_DEEP &&
                    equal.evaluation_order[0] == XR_ASSERTION_EVAL_ACTUAL &&
                    equal.evaluation_order[1] == XR_ASSERTION_EVAL_EXPECTED &&
                    equal.evaluation_order[2] == XR_ASSERTION_EVAL_MESSAGE,
                "assertEqual must select the language deep-equality authority exactly once");

    XrAssertionPlan mutation = equal;
    mutation.builtin_id = XR_CORE_BUILTIN_ASSERT;
    ASSERT_TRUE(!xr_assertion_plan_validate(&mutation),
                "a plan must reject a builtin ID and assertion-kind mismatch");
    mutation = equal;
    mutation.evaluation_order[1] = XR_ASSERTION_EVAL_MESSAGE;
    ASSERT_TRUE(!xr_assertion_plan_validate(&mutation),
                "a plan must reject reordered assertion operands");
    mutation = condition;
    mutation.source.end_column = 0;
    ASSERT_TRUE(!xr_assertion_plan_validate(&mutation),
                "a plan must reject an incomplete source span");
    mutation = condition;
    mutation.evaluation_order[2] = XR_ASSERTION_EVAL_MESSAGE;
    ASSERT_TRUE(!xr_assertion_plan_validate(&mutation),
                "a plan must evaluate an optional message exactly once");

    ASSERT_TRUE(xr_assertion_plan_build(
                    XR_CORE_BUILTIN_ASSERT, 1, assertion_test_location(),
                    XR_CORE_INTRINSIC_TARGET_AOT_FREESTANDING_ASSERTION_PROVIDER,
                    XR_ASSERTION_CAPABILITY_NONE, &condition) ==
                    XR_ASSERTION_PLAN_MISSING_CAPABILITY,
                "freestanding assertion planning must reject a missing failure provider");
}

/* A print plan states that the group is indivisible.  That claim is only worth
 * something if a plan without it is refused, so each mutation below first
 * proves it actually changed the field it names — a mutation that assigns the
 * value already there tests nothing while looking like a rejection test. */
static void test_print_plans_declare_the_atomic_group(void) {
    XrLocation source = {"print_contract.xr", 11, 5, 11, 19};
    XrPrintPlan group;
    ASSERT_TRUE(xr_print_plan_build(XR_CORE_BUILTIN_PRINT, 3, source, XR_CORE_INTRINSIC_TARGET_VM,
                                    0, &group) == XR_PRINT_PLAN_OK,
                "print plan must derive from the stable builtin row");
    ASSERT_TRUE(group.flags == XR_PRINT_PLAN_FLAG_ATOMIC_GROUP,
                "a built print plan must declare whole-group rendering");
    ASSERT_TRUE(group.separator == XR_PRINT_SEPARATOR_SPACE &&
                    group.terminator == XR_PRINT_TERMINATOR_NEWLINE && group.arity == 3,
                "the group owns its separator, terminator and arity");

    XrPrintPlan mutation = group;
    mutation.flags = XR_PRINT_PLAN_FLAG_NONE;
    ASSERT_TRUE(mutation.flags != group.flags,
                "clearing the atomic-group flag must change the plan");
    ASSERT_TRUE(!xr_print_plan_validate(&mutation),
                "a plan that does not claim whole-group rendering must be refused");

    mutation = group;
    mutation.flags = XR_PRINT_PLAN_FLAG_ATOMIC_GROUP | (1u << 7);
    ASSERT_TRUE(mutation.flags != group.flags, "setting an unknown flag must change the plan");
    ASSERT_TRUE(!xr_print_plan_validate(&mutation),
                "a plan carrying a flag this schema does not define must be refused");

    mutation = group;
    mutation.required_capabilities = XR_PRINT_CAPABILITY_NONE;
    ASSERT_TRUE(mutation.required_capabilities != group.required_capabilities,
                "dropping the output capability must change the plan");
    ASSERT_TRUE(!xr_print_plan_validate(&mutation),
                "a print plan must keep requiring the output capability");

    /* Framing is a fact of the group, so the buffer size and the bytes written
     * are derived from one place rather than counted twice. */
    ASSERT_TRUE(xr_print_plan_framing_bytes(&group) == 3,
                "three operands frame as two separators plus one terminator");
}

static void test_assertion_action_channels_are_not_exchangeable(void) {
    const uint32_t target = XR_CORE_INTRINSIC_TARGET_AOT_FREESTANDING_ASSERTION_PROVIDER;
    XrAssertionPlan throws_plan;
    XrAssertionPlan panics_plan;
    ASSERT_TRUE(xr_assertion_plan_build(
                    XR_CORE_BUILTIN_ASSERT_THROWS, 1, assertion_test_location(), target,
                    XR_ASSERTION_CAPABILITY_FAILURE_REPORT |
                        XR_ASSERTION_CAPABILITY_TYPED_ERROR_BOUNDARY,
                    &throws_plan) == XR_ASSERTION_PLAN_OK,
                "assertThrows requires a typed-error boundary");
    ASSERT_TRUE(xr_assertion_plan_build(
                    XR_CORE_BUILTIN_ASSERT_PANICS, 1, assertion_test_location(), target,
                    XR_ASSERTION_CAPABILITY_FAILURE_REPORT |
                        XR_ASSERTION_CAPABILITY_PANIC_BOUNDARY,
                    &panics_plan) == XR_ASSERTION_PLAN_OK,
                "assertPanics requires a panic boundary");
    ASSERT_TRUE(xr_assertion_plan_build(XR_CORE_BUILTIN_ASSERT_THROWS, 1,
                                        assertion_test_location(), target,
                                        XR_ASSERTION_CAPABILITY_FAILURE_REPORT |
                                            XR_ASSERTION_CAPABILITY_PANIC_BOUNDARY,
                                        &throws_plan) == XR_ASSERTION_PLAN_MISSING_CAPABILITY,
                "a panic boundary cannot satisfy assertThrows planning");
    ASSERT_TRUE(xr_assertion_plan_build(XR_CORE_BUILTIN_ASSERT_PANICS, 1,
                                        assertion_test_location(), target,
                                        XR_ASSERTION_CAPABILITY_FAILURE_REPORT |
                                            XR_ASSERTION_CAPABILITY_TYPED_ERROR_BOUNDARY,
                                        &panics_plan) == XR_ASSERTION_PLAN_MISSING_CAPABILITY,
                "a typed-error boundary cannot satisfy assertPanics planning");

    ASSERT_TRUE(xr_assertion_plan_build(XR_CORE_BUILTIN_ASSERT_THROWS, 1,
                                        assertion_test_location(),
                                        XR_CORE_INTRINSIC_TARGET_VM, 0, &throws_plan) ==
                    XR_ASSERTION_PLAN_OK &&
                    xr_assertion_plan_build(XR_CORE_BUILTIN_ASSERT_PANICS, 1,
                                            assertion_test_location(),
                                            XR_CORE_INTRINSIC_TARGET_VM, 0, &panics_plan) ==
                        XR_ASSERTION_PLAN_OK,
                "hosted executors provide both action boundaries");

    XrAssertionFailureKind failure = XR_ASSERTION_FAILURE_COUNT;
    ASSERT_TRUE(xr_assertion_classify_action_outcome(
                    &throws_plan, XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR, &failure) &&
                    failure == XR_ASSERTION_FAILURE_NONE,
                "assertThrows succeeds only for a pending typed error");
    ASSERT_TRUE(xr_assertion_classify_action_outcome(
                    &throws_plan, XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC, &failure) &&
                    failure == XR_ASSERTION_FAILURE_UNEXPECTED_PANIC,
                "assertThrows must report panic as the wrong channel");
    ASSERT_TRUE(xr_assertion_classify_action_outcome(
                    &throws_plan, XR_CORE_INTRINSIC_FAILURE_CHANNEL_NONE, &failure) &&
                    failure == XR_ASSERTION_FAILURE_EXPECTED_TYPED_ERROR,
                "assertThrows must distinguish normal return from wrong-channel panic");
    ASSERT_TRUE(xr_assertion_classify_action_outcome(
                    &panics_plan, XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC, &failure) &&
                    failure == XR_ASSERTION_FAILURE_NONE,
                "assertPanics succeeds only for a current panic");
    ASSERT_TRUE(xr_assertion_classify_action_outcome(
                    &panics_plan, XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR, &failure) &&
                    failure == XR_ASSERTION_FAILURE_UNEXPECTED_TYPED_ERROR,
                "assertPanics must report typed error as the wrong channel");
    ASSERT_TRUE(xr_assertion_classify_action_outcome(
                    &panics_plan, XR_CORE_INTRINSIC_FAILURE_CHANNEL_NONE, &failure) &&
                    failure == XR_ASSERTION_FAILURE_EXPECTED_PANIC,
                "assertPanics must distinguish normal return from wrong-channel typed error");
    ASSERT_TRUE(!xr_assertion_classify_action_outcome(
                    &panics_plan, (XrCoreIntrinsicExpectedFailureChannel) 99, &failure),
                "unknown action channels must fail closed");

    XrAssertionActionOutcome typed_error = {false, true, false};
    ASSERT_TRUE(xr_assertion_classify_action_result(&throws_plan, typed_error, &failure) &&
                    failure == XR_ASSERTION_FAILURE_NONE,
                "a mutually exclusive typed-error observation must satisfy assertThrows");
    XrAssertionActionOutcome conflicting = {false, true, true};
    ASSERT_TRUE(xr_assertion_classify_action_result(&throws_plan, conflicting, &failure) &&
                    failure == XR_ASSERTION_FAILURE_CONFLICTING_CHANNELS,
                "simultaneous typed error and panic observations need a fail-closed failure row");
    XrAssertionActionOutcome missing = {false, false, false};
    ASSERT_TRUE(!xr_assertion_classify_action_result(&panics_plan, missing, &failure),
                "an executor must explicitly report normal return");
}

static void test_assertion_failure_schema_and_renderer(void) {
    char rendered[512];
    XrAssertionFailure failure = {
        .kind = XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL,
        .source = assertion_test_location(),
        .message = "records differ",
        .actual = "Point{x: 1, y: 2}",
        .expected = "Point{x: 1, y: 3}",
    };
    ASSERT_TRUE(xr_assertion_failure_render(rendered, sizeof(rendered), &failure) > 0,
                "a valid structured assertion failure must render");
    ASSERT_TRUE(strcmp(rendered,
                       "AssertionFailure[values-not-equal] at assertion_contract.xr:7:3\n"
                       "  message: records differ\n"
                       "  actual: Point{x: 1, y: 2}\n"
                       "  expected: Point{x: 1, y: 3}") == 0,
                "the canonical renderer must preserve field order and labels");
    size_t rendered_length = strlen(rendered);
    char exact[512];
    char short_buffer[512];
    ASSERT_TRUE(xr_assertion_failure_render(exact, rendered_length + 1u, &failure) ==
                    (int) rendered_length &&
                    strcmp(exact, rendered) == 0,
                "the renderer must accept the exact capacity including the terminator");
    ASSERT_TRUE(xr_assertion_failure_render(short_buffer, rendered_length, &failure) < 0 &&
                    short_buffer[0] == '\0',
                "the renderer must reject and erase truncated failure bytes");

    failure.kind = XR_ASSERTION_FAILURE_UNEXPECTED_PANIC;
    failure.actual = NULL;
    failure.expected = NULL;
    failure.caught_panic = "division by zero";
    ASSERT_TRUE(xr_assertion_failure_render(rendered, sizeof(rendered), &failure) > 0 &&
                    strstr(rendered, "caught_panic: division by zero") != NULL,
                "wrong-channel panic must retain its structured payload");
    failure.caught_error = "NumberParseError.InvalidSyntax";
    ASSERT_TRUE(xr_assertion_failure_render(rendered, sizeof(rendered), &failure) < 0,
                "a failure cannot carry both typed-error and panic channels");
    failure.kind = XR_ASSERTION_FAILURE_CONFLICTING_CHANNELS;
    ASSERT_TRUE(xr_assertion_failure_render(rendered, sizeof(rendered), &failure) > 0 &&
                    strstr(rendered, "conflicting-failure-channels") != NULL &&
                    strstr(rendered, "caught_error: NumberParseError.InvalidSyntax") != NULL &&
                    strstr(rendered, "caught_panic: division by zero") != NULL,
                "the conflicting-channel failure must retain both observations explicitly");
    ASSERT_TRUE(xr_assertion_failure_render_size(&failure) == (int) strlen(rendered),
                "renderer measurement must match the exact canonical byte count");
}

static void test_exact_scalar_registry(void) {
    char error[192];
    const XrExactScalarDesc *i64 = xr_exact_scalar_by_id(XR_EXACT_SCALAR_I64);
    const XrExactScalarDesc *u8 = xr_exact_scalar_by_native_type(XR_NATIVE_U8);
    const XrExactScalarDesc *f64 = xr_exact_scalar_by_native_type(XR_NATIVE_F64);

    ASSERT_TRUE(xr_exact_scalar_registry_validate(error, sizeof(error)),
                "exact scalar registry must be internally consistent");
    ASSERT_TRUE(xr_exact_scalar_count() == 12,
                "exact scalar registry must contain twelve representations");
    ASSERT_TRUE(XR_EXACT_SCALAR_I8 == 1 && XR_EXACT_SCALAR_I16 == 2 &&
                    XR_EXACT_SCALAR_I32 == 3 && XR_EXACT_SCALAR_I64 == 4 &&
                    XR_EXACT_SCALAR_U8 == 5 && XR_EXACT_SCALAR_U16 == 6 &&
                    XR_EXACT_SCALAR_U32 == 7 && XR_EXACT_SCALAR_U64 == 8 &&
                    XR_EXACT_SCALAR_F32 == 9 && XR_EXACT_SCALAR_F64 == 10 &&
                    XR_EXACT_SCALAR_ISIZE == 11 && XR_EXACT_SCALAR_USIZE == 12,
                "exact scalar stable IDs must not be renumbered or reused");
    ASSERT_TRUE(i64 && i64->native_type == XR_NATIVE_I64 &&
                    strcmp(i64->source_name, "i64") == 0,
                "i64 stable identity must own the default integer representation");
    ASSERT_TRUE(u8 && strcmp(u8->source_name, "u8") == 0 &&
                    (u8->flags & XR_EXACT_SCALAR_FLAG_BYTE_ELEMENT) != 0,
                "u8 must be the sole byte element spelling");
    ASSERT_TRUE(f64 && strcmp(f64->source_name, "f64") == 0 &&
                    (f64->flags & XR_EXACT_SCALAR_FLAG_DEFAULT_DECIMAL) != 0,
                "f64 must own the default decimal representation");
    ASSERT_TRUE(xr_exact_scalar_by_source_name("int", 3) == NULL,
                "int must not have an exact scalar identity");
    ASSERT_TRUE(xr_exact_scalar_by_source_name("byte", 4) == NULL,
                "byte must not have an exact scalar identity");
    ASSERT_TRUE(xr_exact_scalar_by_source_name("float", 5) == NULL,
                "float must not have an exact scalar identity");
}

int main(void) {
    printf("--- xi_intrinsic.def ---\n");
    test_enum_values();
    test_id_range();
    test_no_duplicate_ids();
    test_arity_valid();
    test_intrinsic_count();
    test_effect_flags();
    test_ret_rep_valid();

    printf("--- xi_method_sym.def ---\n");
    test_method_sym_ids();
    test_method_sym_no_dups();
    test_method_sym_names();
    test_method_sym_count();
    test_string_builder_append_symbol_identity();
    test_builtin_receiver_registry_method_symbols();
    test_builtin_receiver_registry_method_ids();
    test_builtin_receiver_registry_metadata();
    test_builtin_receiver_method_placement();
    test_semantic_intrinsic_registry();
    test_core_intrinsic_registry();
    test_assertion_plans_are_registry_projections();
    test_print_plans_declare_the_atomic_group();
    test_assertion_action_channels_are_not_exchangeable();
    test_assertion_failure_schema_and_renderer();
    test_exact_scalar_registry();

    printf("\n=== test_xi_intrinsic: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
