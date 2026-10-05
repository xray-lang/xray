/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_atomic_runtime_core.c - Shared atomic commits and real owned release
 */
#include "base/xmalloc.h"
#include "base/xcompile_resources.h"
#include "module/xmodule_identity.h"
#include "shared/xnative_declaration.h"
#include "xir/xxir_atomic.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_tuple.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct CoreBlock { void *pointer; size_t bytes; } CoreBlock;
typedef struct CoreObserver {
    CoreBlock blocks[512];
    size_t attempts, fail_at, live, bytes, peak;
} CoreObserver;
static CoreObserver compiler_observer = {.fail_at = SIZE_MAX};
static CoreObserver runtime_observer = {.fail_at = SIZE_MAX};
static void *core_allocate(CoreObserver *observer, size_t bytes) {
    if (observer->attempts++ == observer->fail_at) return NULL;
    void *memory = xr_malloc(bytes); CHECK(memory);
    size_t slot = 0; while (slot < 512 && observer->blocks[slot].pointer) ++slot;
    CHECK(slot < 512); observer->blocks[slot] = (CoreBlock){memory, bytes};
    ++observer->live; observer->bytes += bytes;
    if (observer->bytes > observer->peak) observer->peak = observer->bytes;
    return memory;
}
static void core_free(CoreObserver *observer, void *memory) {
    if (!memory) return;
    size_t slot = 0; while (slot < 512 && observer->blocks[slot].pointer != memory) ++slot;
    CHECK(slot < 512 && observer->live);
    observer->bytes -= observer->blocks[slot].bytes; --observer->live;
    observer->blocks[slot] = (CoreBlock){0}; xr_free(memory);
}
static void *compiler_allocate(size_t bytes) { return core_allocate(&compiler_observer, bytes); }
static void compiler_free(void *memory) { core_free(&compiler_observer, memory); }
static void *runtime_allocate(size_t bytes) { return core_allocate(&runtime_observer, bytes); }
static void runtime_free(void *memory) { core_free(&runtime_observer, memory); }
static void *runtime_calloc(size_t count, size_t bytes) {
    CHECK(!count || bytes <= SIZE_MAX / count);
    void *memory = runtime_allocate(count * bytes);
    if (memory) memset(memory, 0, count * bytes);
    return memory;
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#pragma push_macro("xr_calloc")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) compiler_allocate(bytes)
#define xr_free(memory) compiler_free(memory)
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) runtime_allocate(bytes)
#define xr_free(memory) runtime_free(memory)
static bool force_unsupported;
static unsigned capability_probes, unsupported_probe;
static size_t bits_initializations;
static bool lock_free32(_Atomic(uint32_t) *cell) {
    ++capability_probes; return !force_unsupported && capability_probes != unsupported_probe && atomic_is_lock_free(cell);
}
static bool lock_free64(_Atomic(uint64_t) *cell) {
    ++capability_probes; return !force_unsupported && capability_probes != unsupported_probe && atomic_is_lock_free(cell);
}
static void core_init_bool(_Atomic(bool) *cell, bool value) { atomic_init(cell, value); }
static void core_init32(_Atomic(uint32_t) *cell, uint32_t value) { atomic_init(cell, value); }
static void core_init64(_Atomic(uint64_t) *cell, uint64_t value) {
    ++bits_initializations; atomic_init(cell, value);
}
static size_t cell_cas_attempts;
static bool force_collision;
static uint64_t collision_value;
static bool core_cell_cas(_Atomic(uint64_t) *cell, uint64_t *expected, uint64_t desired,
    memory_order success, memory_order failure) {
    CHECK(failure != memory_order_release && failure != memory_order_acq_rel);
    ++cell_cas_attempts;
    if (force_collision) {
        force_collision = false;
        atomic_store_explicit(cell, collision_value, memory_order_relaxed);
    }
    return atomic_compare_exchange_strong_explicit(cell, expected, desired, success, failure);
}
#undef atomic_is_lock_free
#define atomic_is_lock_free(cell) _Generic((cell), \
    _Atomic(uint32_t) *: lock_free32, _Atomic(uint64_t) *: lock_free64)(cell)
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit(cell, expected, desired, success, failure) \
    core_cell_cas(cell, expected, desired, success, failure)
#undef atomic_init
#define atomic_init(cell, value) _Generic((cell), _Atomic(bool) *: core_init_bool, \
    _Atomic(uint32_t) *: core_init32, _Atomic(uint64_t) *: core_init64)(cell, value)
#include "xir/xxir_value.c"
#undef xr_calloc
#define xr_calloc(count, bytes) runtime_calloc(count, bytes)
#include "xir/xxir_call.c"
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

enum { CORE_I64 = 256, CORE_F64, CORE_BOOL, CORE_CAS_I64, CORE_CAS_F64,
    CORE_CAS_BOOL, CORE_ORDER, CORE_OPTIONAL_ORDER, CORE_ARRAY, CORE_TUPLE, CORE_NULLABLE };
typedef struct CoreOwner {
    XrXirCompileContext context;
    XrXirTypeArena *arena;
    XrXirDomain *domain;
    XrXirValueAdmission admission;
} CoreOwner;
static XrXirValue core_scalar(XrXirType type, uint64_t bits) {
    XrXirValue value = {(uint32_t)type, 0, 0}; memcpy(&value.payload, &bits, sizeof(bits)); return value;
}
static XrXirValueStatus core_arena(const XrXirCompileContext *context, XrXirTypeArena **output) {
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_STDLIB, "prelude", NULL};
    char *module = NULL;
    XrModuleStatus module_status = xr_compile_module_identity_from_logical(context->resources,
        &authority, "prelude/builtin_symbols.def", &module);
    if (module_status != XR_MODULE_OK) return module_status == XR_MODULE_OUT_OF_MEMORY ?
        XR_XIR_VALUE_OOM : module_status == XR_MODULE_BUDGET ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
    const char *names[] = {"Relaxed", "Acquire", "Release", "AcquireRelease", "SeqCst"};
    XrXirNominalVariant variants[5] = {0};
    for (unsigned i = 0; i < 5; ++i)
        variants[i].name = (XrXirLiteral){names[i], (uint32_t)strlen(names[i])};
    XrXirNominalDeclaration declaration = {.module = {module, (uint32_t)strlen(module)},
        .name = {"Ordering", 8}, .exported = 1, .kind = XR_XIR_NOMINAL_ENUM,
        .variants = variants, .variant_count = 5};
    const XrNativeTypeDeclaration *registry = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ORDERING);
    CHECK(registry && registry->kind == XR_NATIVE_DECLARATION_VALUE);
    declaration.native.native_id = XR_NATIVE_DECLARATION_ORDERING;
    memcpy(declaration.native.source_fingerprint, registry->source_fingerprint.bytes, 32);
    XrXirNominalTable table = {.declarations = &declaration, .count = 1};
    XrXirCallableParameter pairs[4][2] = {{{.type = XR_XIR_I64}, {.type = XR_XIR_BOOL}},
        {{.type = XR_XIR_F64}, {.type = XR_XIR_BOOL}},
        {{.type = XR_XIR_BOOL}, {.type = XR_XIR_BOOL}},
        {{.type = (XrXirType)CORE_I64}, {.type = XR_XIR_BOOL}}};
    XrXirTypeNode nodes[11] = {0};
    const XrXirType elements[] = {XR_XIR_I64, XR_XIR_F64, XR_XIR_BOOL};
    for (unsigned i = 0; i < 3; ++i) {
        nodes[i].kind = XR_XIR_TYPE_ATOMIC; nodes[i].element = elements[i];
        nodes[3 + i].kind = XR_XIR_TYPE_TUPLE; nodes[3 + i].parameters = pairs[i];
        nodes[3 + i].parameter_count = 2;
    }
    nodes[6].kind = XR_XIR_TYPE_NOMINAL;
    nodes[7].kind = XR_XIR_TYPE_NULLABLE; nodes[7].element = (XrXirType)CORE_ORDER;
    nodes[8].kind = XR_XIR_TYPE_ARRAY; nodes[8].element = (XrXirType)CORE_I64;
    nodes[9].kind = XR_XIR_TYPE_TUPLE; nodes[9].parameters = pairs[3]; nodes[9].parameter_count = 2;
    nodes[10].kind = XR_XIR_TYPE_NULLABLE; nodes[10].element = (XrXirType)CORE_I64;
    XrXirTypes types = {nodes, 11, &table, NULL};
    XrXirNominalTable *copy = NULL, *projected = NULL;
    XrXirStatus status = xr_xir_compile_nominal_clone(context, &table, &types, &copy);
    if (status != XR_XIR_OK) CHECK(!copy);
    if (status == XR_XIR_OK) status = xr_xir_compile_nominal_project(context, copy, &projected);
    if (status != XR_XIR_OK) CHECK(!projected);
    xr_xir_compile_nominal_free(copy); xr_compile_resources_free(module);
    XrXirValueStatus result = status == XR_XIR_OK ? XR_XIR_VALUE_OK :
        status == XR_XIR_OUT_OF_MEMORY ? XR_XIR_VALUE_OOM :
        status == XR_XIR_BUDGET ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
    if (result == XR_XIR_VALUE_OK) {
        types.nominals = projected; result = xr_xir_compile_type_arena_new(context, &types, output);
        if (result != XR_XIR_VALUE_OK) CHECK(!*output);
    }
    xr_xir_compile_nominal_free(projected); return result;
}
static void core_owner_new(CoreOwner *owner) {
    const XrCompileResourceLimits limits = {UINT64_C(32) << 20, UINT64_C(8) << 20, UINT64_C(64) << 20};
    CHECK(xr_compile_resources_new(&limits, &owner->context.resources) == XR_COMPILE_RESOURCE_OK);
    owner->context.limits = xr_xir_compile_default_limits();
    CHECK(core_arena(&owner->context, &owner->arena) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(UINT64_C(1) << 20, &owner->domain) == XR_XIR_VALUE_OK);
    owner->admission = (XrXirValueAdmission){owner->arena, owner->domain, NULL, NULL, 1048576, 65536};
}
static void core_owner_free(CoreOwner *owner) {
    xr_xir_compile_type_arena_drop(owner->arena); xr_xir_domain_drop(owner->domain);
    xr_compile_resources_release(owner->context.resources); *owner = (CoreOwner){0};
    CHECK(!compiler_observer.live && !compiler_observer.bytes && !runtime_observer.live && !runtime_observer.bytes);
}
static XrXirValue core_order(CoreOwner *owner, uint32_t ordinal) {
    XrXirValue enumeration = {0}, optional = {0};
    CHECK(xr_xir_enum_new((XrXirType)CORE_ORDER, ordinal, NULL, 0, &owner->admission, &enumeration) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)CORE_OPTIONAL_ORDER, &enumeration,
        &owner->admission, &optional) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&enumeration); return optional;
}
static XrXirAtomicOutcome core_execute(CoreOwner *owner, XrXirValue *receiver,
    XrXirAtomicOperation operation, XrXirValue *operands, const XrXirValue *ordering, XrXirValue *output) {
    XrXirType element = xr_xir_atomic_element(xr_xir_compile_type_arena_types(owner->arena), (XrXirType)receiver->type);
    uint32_t count = atomic_operand_count(operation);
    XrXirType result = operation == XR_XIR_ATOMIC_OPERATION_STORE || operation == XR_XIR_ATOMIC_OPERATION_ADD || operation == XR_XIR_ATOMIC_OPERATION_SUB ?
        XR_XIR_UNIT : operation == XR_XIR_ATOMIC_OPERATION_TO_STRING ? XR_XIR_STRING : element;
    if (operation == XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE)
        result = (XrXirType)(element == XR_XIR_I64 ? CORE_CAS_I64 : element == XR_XIR_F64 ? CORE_CAS_F64 : CORE_CAS_BOOL);
    XrXirAtomicRequest request = {receiver, operands, ordering, count, operation, result};
    XrXirAtomicProgress progress = {0};
    XrXirAtomicOutcome outcome = xr_xir_atomic_start(&request, &owner->admission, NULL, &progress, output);
    CHECK(!outcome.continuing); xr_xir_atomic_progress_clear(&progress); return outcome;
}
static uint64_t core_load(CoreOwner *owner, XrXirValue *receiver) {
    XrXirValue value = {0}; XrXirAtomicOutcome outcome = core_execute(owner, receiver, XR_XIR_ATOMIC_OPERATION_LOAD, NULL, NULL, &value);
    CHECK(outcome.status == XR_XIR_RUN_OK && outcome.permission == XR_XIR_CALL_READY);
    uint64_t bits = (uint64_t)value.payload; xr_xir_value_drop(&value); return bits;
}
static void core_methods(void) {
    CoreOwner owner = {0}; core_owner_new(&owner);
    CHECK(xr_xir_atomic_capability());
    const XrXirType elements[] = {XR_XIR_I64, XR_XIR_F64, XR_XIR_BOOL};
    for (unsigned type = 0; type < 3; ++type) for (unsigned method = 0; method < 10; ++method) {
        bool numeric = method >= XR_XIR_ATOMIC_OPERATION_ADD && method <= XR_XIR_ATOMIC_OPERATION_FETCH_SUB;
        if ((type == 2 && numeric) || (type != 2 && method == XR_XIR_ATOMIC_OPERATION_TOGGLE)) continue;
        for (unsigned ordering = 0; ordering < 5; ++ordering) {
            if (method == XR_XIR_ATOMIC_OPERATION_TO_STRING && ordering) continue;
            XrXirValue initial = core_scalar(elements[type], type == 0 ? 7 : type == 1 ? UINT64_C(0x401c000000000000) : 1);
            XrXirValue operands[2] = {core_scalar(elements[type], type == 0 ? 2 : type == 1 ? UINT64_C(0x4000000000000000) : 0), initial};
            if (method == XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE) { operands[0] = initial; operands[1] = core_scalar(elements[type], type == 0 ? 2 : type == 1 ? UINT64_C(0x4000000000000000) : 0); }
            XrXirValue receiver = {0}, result = {0}, order = core_order(&owner, ordering);
            CHECK(xr_xir_atomic_new((XrXirType)(CORE_I64 + type), &initial, &owner.admission, &receiver) == XR_XIR_VALUE_OK);
            bool invalid = (method == XR_XIR_ATOMIC_OPERATION_LOAD && (ordering == 2 || ordering == 3)) ||
                (method == XR_XIR_ATOMIC_OPERATION_STORE && (ordering == 1 || ordering == 3));
            XrXirAtomicOutcome outcome = core_execute(&owner, &receiver, (XrXirAtomicOperation)method, operands,
                method == XR_XIR_ATOMIC_OPERATION_TO_STRING ? NULL : &order, &result);
            CHECK(outcome.status == (invalid ? XR_XIR_RUN_ATOMIC_ARGUMENT : XR_XIR_RUN_OK));
            if (invalid) CHECK(!result.type && !result.payload && core_load(&owner, &receiver) == (uint64_t)initial.payload);
            else if (method == XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE) {
                XrXirValue old = {0}, exchanged = {0};
                CHECK(xr_xir_tuple_get(&result, 0, &old) == XR_XIR_VALUE_OK && old.payload == initial.payload);
                CHECK(xr_xir_tuple_get(&result, 1, &exchanged) == XR_XIR_VALUE_OK && exchanged.payload == 1);
                xr_xir_value_drop(&old); xr_xir_value_drop(&exchanged);
                CHECK(core_load(&owner, &receiver) == (uint64_t)operands[1].payload);
            } else if (method == XR_XIR_ATOMIC_OPERATION_TO_STRING) {
                const char *bytes = NULL; size_t length = 0; const char *expected = type == 0 ? "7" : type == 1 ? "7.0" : "true";
                CHECK(xr_xir_string_view(&result, &bytes, &length) && length == strlen(expected) && !memcmp(bytes, expected, length));
            } else if (method == XR_XIR_ATOMIC_OPERATION_LOAD || method == XR_XIR_ATOMIC_OPERATION_SWAP ||
                method == XR_XIR_ATOMIC_OPERATION_FETCH_ADD || method == XR_XIR_ATOMIC_OPERATION_FETCH_SUB) CHECK(result.payload == initial.payload);
            else if (method == XR_XIR_ATOMIC_OPERATION_TOGGLE) CHECK(result.type == XR_XIR_BOOL && result.payload == initial.payload && !core_load(&owner, &receiver));
            if (!invalid && numeric) {
                bool subtract = method == XR_XIR_ATOMIC_OPERATION_SUB || method == XR_XIR_ATOMIC_OPERATION_FETCH_SUB;
                uint64_t expected = type == 0 ? (subtract ? 5 : 9) :
                    subtract ? UINT64_C(0x4014000000000000) : UINT64_C(0x4022000000000000);
                CHECK(core_load(&owner, &receiver) == expected);
            }
            xr_xir_value_drop(&result); xr_xir_value_drop(&receiver); xr_xir_value_drop(&order);
        }
    }
    core_owner_free(&owner);
}
static void core_f64_transport_and_retry(void) {
    CoreOwner owner = {0}; core_owner_new(&owner);
    const uint64_t bits[] = {0, UINT64_C(0x8000000000000000), 1, UINT64_C(0x8000000000000001),
        UINT64_C(0x0010000000000000), UINT64_C(0x7fefffffffffffff), UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000000000), UINT64_C(0xfff8000000000000),
        UINT64_C(0x7ff0000000000001), UINT64_C(0xfff0000000000001), UINT64_C(0x7ff8123456789abc)};
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        XrXirValue initial = core_scalar(XR_XIR_F64, bits[i]), receiver = {0}, alias = {0}, result = {0};
        CHECK(xr_xir_value_valid(&initial));
        CHECK(xr_xir_atomic_new((XrXirType)CORE_F64, &initial, &owner.admission, &receiver) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_value_copy(&receiver, &alias) == XR_XIR_VALUE_OK && alias.payload == receiver.payload);
        CHECK(core_load(&owner, &receiver) == bits[i]);
        XrXirValue operands[2] = {initial, core_scalar(XR_XIR_F64, bits[i] ^ UINT64_C(0x8000000000000000))};
        CHECK(core_execute(&owner, &alias, XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE, operands, NULL, &result).status == XR_XIR_RUN_OK);
        XrXirValue exchanged = {0}; CHECK(xr_xir_tuple_get(&result, 1, &exchanged) == XR_XIR_VALUE_OK && exchanged.payload == 1);
        xr_xir_value_drop(&exchanged); xr_xir_value_drop(&result);
        CHECK(core_load(&owner, &receiver) == (bits[i] ^ UINT64_C(0x8000000000000000)));
        CHECK(core_execute(&owner, &alias, XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE, operands, NULL, &result).status == XR_XIR_RUN_OK);
        CHECK(xr_xir_tuple_get(&result, 1, &exchanged) == XR_XIR_VALUE_OK && !exchanged.payload);
        xr_xir_value_drop(&exchanged); xr_xir_value_drop(&result); xr_xir_value_drop(&receiver); xr_xir_value_drop(&alias);
    }
    XrXirValue initial = core_scalar(XR_XIR_F64, UINT64_C(0x3ff0000000000000));
    XrXirValue delta = initial, receiver = {0}, result = {0};
    CHECK(xr_xir_atomic_new((XrXirType)CORE_F64, &initial, &owner.admission, &receiver) == XR_XIR_VALUE_OK);
    XrXirAtomicRequest request = {&receiver, &delta, NULL, 1, XR_XIR_ATOMIC_OPERATION_FETCH_ADD, XR_XIR_F64};
    XrXirAtomicProgress progress = {0}; cell_cas_attempts = 0;
    force_collision = true; collision_value = UINT64_C(0x4008000000000000);
    XrXirAtomicOutcome outcome = xr_xir_atomic_start(&request, &owner.admission, NULL, &progress, &result);
    CHECK(outcome.status == XR_XIR_RUN_OK && outcome.continuing && !result.type && !result.payload && cell_cas_attempts == 1);
    CHECK(progress.observed == collision_value);
    delta = core_scalar(XR_XIR_F64, UINT64_C(0x4024000000000000));
    outcome = xr_xir_atomic_resume(&progress, &owner.admission, NULL, &result);
    CHECK(outcome.status == XR_XIR_RUN_OK && !outcome.continuing && cell_cas_attempts == 2);
    CHECK((uint64_t)result.payload == UINT64_C(0x4008000000000000));
    CHECK(core_load(&owner, &receiver) == UINT64_C(0x4010000000000000));
    xr_xir_atomic_progress_clear(&progress); xr_xir_value_drop(&receiver); xr_xir_value_drop(&result);
    core_owner_free(&owner);
}
#include "xir_atomic_runtime_owner_cases.h"
#include "xir_scalar_compile_owner.h"
#include "xir_float_runtime_cases.h"
static XrXirValue floating_argument(XrXirType type, uint64_t bits) { return core_scalar(type, bits); }
#include "xir_float_admission_cases.h"
int main(void) {
    _Static_assert(XR_XIR_VALUE_UNSUPPORTED == 7 && XR_XIR_RUN_ATOMIC_ARGUMENT == 11 &&
        XR_XIR_RUN_UNSUPPORTED == 12 && XR_XIR_CALL_UNSUPPORTED == 22, "append-only runtime statuses");
    _Static_assert(XR_XIR_I8 == 5 && XR_XIR_RUNE == 16, "primitive hole preserves every other identity");
    XrXirValue retired = {4, 0, 0}; CHECK(!xr_xir_value_valid(&retired));
    core_methods(); core_f64_transport_and_retry(); core_runtime_faults();
    core_compiler_faults(); core_carriers_and_capability(); core_call_statuses();
    float_runtime_cases();
    scalar_compile_owner_new(&scalar_owner,
        (XrCompileResourceLimits){UINT64_C(32) << 20, UINT64_C(8) << 20, UINT64_C(64) << 20},
        xr_xir_compile_default_limits());
    floating_value_admission(); scalar_compile_owner_free(&scalar_owner);
    CHECK(!compiler_observer.live && !compiler_observer.bytes && !runtime_observer.live && !runtime_observer.bytes);
    puts("Atomic runtime core methods and bounded binary64 retry passed");
    return 0;
}
