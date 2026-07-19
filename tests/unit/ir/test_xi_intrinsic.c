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
#include <string.h>
#include <stdbool.h>
#include "../../src/ir/xi_intrinsic_flags.h"
#include "../../src/frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../../src/frontend/analyzer/xa_intrinsic_registry.h"

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
                           type_params, effect, allocation, unsafe_requirement, lowering)          \
    source_name,
#define XB_RECEIVER_VARIADIC_METHOD(id, source_name, receiver, result, p0, p1, p2, param_count,    \
                                    min_params, type_params, effect, allocation,                   \
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
        snprintf(msg, sizeof(msg), "receiver registry method '%s' missing allocation label",
                 spec->id);
        ASSERT_TRUE(xa_builtin_receiver_allocation_label(spec->allocation)[0] != '\0', msg);
        snprintf(msg, sizeof(msg), "receiver registry method '%s' missing unsafe label", spec->id);
        ASSERT_TRUE(
            xa_builtin_receiver_unsafe_requirement_label(spec->unsafe_requirement)[0] != '\0', msg);
    }

    const XaBuiltinReceiverMethodSpec *append =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM);
    ASSERT_TRUE(append && xa_builtin_receiver_method_documentation_group(append) ==
                              XA_BUILTIN_DOC_GROUP_U8_ARRAY,
                "appendFrom must document under Array<byte> byte bulk methods");
    ASSERT_TRUE(append && xa_builtin_receiver_method_profile_availability(append) ==
                              XA_BUILTIN_PROFILE_HEAP_CAPABLE,
                "appendFrom must be marked heap-capable");

    const XaBuiltinReceiverMethodSpec *common_prefix =
        xa_builtin_receiver_method_by_id(XA_BUILTIN_RECEIVER_METHOD_U8_SLICE_COMMON_PREFIX);
    ASSERT_TRUE(common_prefix && xa_builtin_receiver_method_documentation_group(common_prefix) ==
                                     XA_BUILTIN_DOC_GROUP_U8_SLICE,
                "commonPrefix must document under Slice<byte> byte range methods");
    ASSERT_TRUE(common_prefix && xa_builtin_receiver_method_profile_availability(common_prefix) ==
                                     XA_BUILTIN_PROFILE_ALL,
                "commonPrefix must be available in all profiles");
}

static void test_builtin_receiver_method_placement(void) {
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
                "popcount must return language int");

    const char *u8_array_methods[] = {"appendFrom", "repeatFrom", NULL};
    for (int i = 0; u8_array_methods[i]; i++) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Array<byte> registry must contain %s", u8_array_methods[i]);
        ASSERT_TRUE(receiver_has_method(XA_BUILTIN_RECEIVER_U8_ARRAY, u8_array_methods[i]), msg);
    }

    const char *array_forbidden_range_methods[] = {
        "load", "store", "copyFrom", "compare", "commonPrefix", "reinterpret", NULL};
    for (int i = 0; array_forbidden_range_methods[i]; i++) {
        char msg[192];
        snprintf(msg, sizeof(msg), "Array<byte> registry must not own range method %s",
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
        snprintf(msg, sizeof(msg), "Slice<byte> registry must contain %s", u8_slice_methods[i]);
        ASSERT_TRUE(receiver_has_method(XA_BUILTIN_RECEIVER_U8_SLICE, u8_slice_methods[i]), msg);
    }

    const char *slice_forbidden_grow_methods[] = {"appendFrom", "push", "reserve", "resize", NULL};
    for (int i = 0; slice_forbidden_grow_methods[i]; i++) {
        char msg[192];
        snprintf(msg, sizeof(msg), "Slice<byte> registry must not own grow method %s",
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

    const XaIntrinsicDesc *rotl = xa_intrinsic_by_id(XA_INTRINSIC_BITS_ROTATE_LEFT);
    ASSERT_TRUE(rotl && rotl->family == XA_INTRINSIC_FAMILY_BITS &&
                    rotl->lowering == XA_INTRINSIC_LOWERING_BIT_ROTL &&
                    rotl->effect == XA_INTRINSIC_EFFECT_PURE && rotl->min_arity == 1 &&
                    rotl->max_arity == 1,
                "exact integer bit semantics must be represented in the canonical registry");
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
    test_builtin_receiver_registry_method_symbols();
    test_builtin_receiver_registry_method_ids();
    test_builtin_receiver_registry_metadata();
    test_builtin_receiver_method_placement();
    test_semantic_intrinsic_registry();

    printf("\n=== test_xi_intrinsic: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
