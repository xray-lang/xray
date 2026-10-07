/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_coroutine_panic_group2_native.c - Original sealed and indirect coroutine panic with conditional defer
 *
 * KEY CONCEPT:
 *   Each process preserves one original source and executes only same-module tests.
 *   Copied intrinsic UNIT facts outlive owners; canonical text uses fresh caller scratch.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include "base/xsha256.h"
#include "xir/xxir_types.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_panic.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked input");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Current public execution contracts");

static const char source_sealed_coroutine_panic[] =
    "fn quotient(divisor: i64) -> i64 {\n"
    "Coro.yield()\n"
    "return 84 / divisor\n"
    "}\n"
    "var cleanupCount: i64 = 0\n"
    "fn readCleanupCount() -> i64 { return cleanupCount }\n"
    "class Cell {\n"
    "value: i64\n"
    "constructor(value: i64) { this.value = value }\n"
    "}\n"
    "fn checked(ok: bool) -> i64 {\n"
    "var first = Cell(20)\n"
    "var second = Cell(22)\n"
    "defer { if (ok) { cleanupCount = 11 } else { cleanupCount = 22 } }\n"
    "var divisor: i64 = 0\n"
    "if (ok) { divisor = 2 }\n"
    "var result = quotient(divisor)\n"
    "return first.value + second.value + result - 42\n"
    "}\n"
    "fn answer() -> i64 { return checked(false) + readCleanupCount() }\n"
    "@test\n"
    "fn checkFreshCleanup() { assert(readCleanupCount() == 0) }\n"
    "@test\n"
    "fn checkPanicFailure() { answer() }\n"
    "@test\n"
    "fn checkFailureCleanup() { assert(readCleanupCount() == 22) }\n"
    "@test\n"
    "fn checkPanicSuccess() { assert(checked(true) == 42); assert(readCleanupCount() == 11) }\n";

static const char source_indirect_coroutine_panic[] =
    "fn quotient(divisor: i64) -> i64 {\n"
    "Coro.yield()\n"
    "return 84 / divisor\n"
    "}\n"
    "var cleanupCount: i64 = 0\n"
    "fn readCleanupCount() -> i64 { return cleanupCount }\n"
    "class Cell {\n"
    "value: i64\n"
    "constructor(value: i64) { this.value = value }\n"
    "}\n"
    "fn checked(ok: bool) -> i64 {\n"
    "var first = Cell(20)\n"
    "var second = Cell(22)\n"
    "defer { if (ok) { cleanupCount = 11 } else { cleanupCount = 22 } }\n"
    "var divisor: i64 = 0\n"
    "if (ok) { divisor = 2 }\n"
    "var selected = quotient\n"
    "var result = selected(divisor)\n"
    "return first.value + second.value + result - 42\n"
    "}\n"
    "fn answer() -> i64 { return checked(false) + readCleanupCount() }\n"
    "@test\n"
    "fn checkFreshCleanup() { assert(readCleanupCount() == 0) }\n"
    "@test\n"
    "fn checkPanicFailure() { answer() }\n"
    "@test\n"
    "fn checkFailureCleanup() { assert(readCleanupCount() == 22) }\n"
    "@test\n"
    "fn checkPanicSuccess() { assert(checked(true) == 42); assert(readCleanupCount() == 11) }\n";

typedef struct PanicFixture {
    const char *id, *source;
    size_t source_length;
    uint32_t code;
    const char *message;
    size_t message_length;
    bool indirect;
    const char *symbol;
} PanicFixture;
static const PanicFixture panic_fixtures[] = {
    {"sealed_coroutine_panic", source_sealed_coroutine_panic, sizeof(source_sealed_coroutine_panic) - 1,
        420u, "division by zero", 16u, false, "source_coroutine_panic_sealed"},
    {"indirect_coroutine_panic", source_indirect_coroutine_panic, sizeof(source_indirect_coroutine_panic) - 1,
        420u, "division by zero", 16u, true, "source_coroutine_panic_indirect"}
};

enum PanicRole {
    ROLE_ANSWER, ROLE_CHECKED, ROLE_READ, ROLE_QUOTIENT,
    ROLE_FRESH, ROLE_FAILURE, ROLE_FAILURE_CLEANUP, ROLE_SUCCESS, ROLE_COUNT
};
static const char *const role_names[ROLE_COUNT] = {
    "answer", "checked", "readCleanupCount", "quotient",
    "checkFreshCleanup", "checkPanicFailure", "checkFailureCleanup", "checkPanicSuccess"
};
typedef struct PanicRoles {
    uint32_t entry, initializer, functions[ROLE_COUNT];
} PanicRoles;

static uint32_t role_parameter_count(unsigned role) {
    return role == ROLE_CHECKED || role == ROLE_QUOTIENT ? 1u : 0u;
}

static const PanicFixture *select_fixture(const char *id) {
    for (size_t i = 0; i < sizeof(panic_fixtures) / sizeof(panic_fixtures[0]); ++i)
        if (!strcmp(id, panic_fixtures[i].id)) return &panic_fixtures[i];
    return NULL;
}

typedef struct PanicRun {
    const PanicFixture *fixture;
    XrXirCompileContext context;
    XrCompileResourceStats baseline, final;
    XrCompilerSession *session;
    XrXirSourceProduct *product;
    XrXirSourceProductDiagnostic diagnostic;
    XrXirDiagnostic xir;
    XrXirArtifact *source_checked, *closed_checked, *lowered;
    XrXirCheckedPacket retained;
    XrXirProgram *program;
    XrXirEffects *effects;
    XrXirInstance *instances[2];
    XrXirValue value;
    PanicRoles roles;
    XrXirCallResult escaped[4];
    unsigned escaped_count, canonical_count, Unit_count, permission_count;
    unsigned yield_count, resume_count, wait_guard_count, driver_calls;
    unsigned call_yields, expected_yields, current_instance, current_repeat;
    XrXirCSource emitted, repeated;
    char c_digest[65];
    const char *operation;
    XrXirStatus status;
    XrXirCallStatus call_status;
    uint8_t *input;
    size_t input_length;
} PanicRun;

#define OBSERVE(condition) do { if (!(condition)) { \
    fprintf(stderr, "coroutine-panic-native fixture=%s check line=%d operation=%s condition=%s\n", \
        run->fixture->id, __LINE__, run->operation, #condition); return false; } } while (0)

static bool phase(PanicRun *run, const char *name) {
    XrCompileResourceStats stats = {0};
    run->operation = name;
    OBSERVE(xr_compile_resources_stats(run->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(stats.live_bytes == instance_compile_bytes);
    printf("coroutine-panic-native fixture=%s phase=%s sites=%zu allocations=%" PRIu64 " allocated=%" PRIu64
        " live=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 " physical=%zu/%zu\n",
        run->fixture->id, name, instance_compile_attempts, stats.allocation_count, stats.allocated_bytes,
        stats.live_bytes, stats.peak_bytes, stats.work, instance_compile_live, instance_compile_bytes);
    return true;
}

static bool named(const XrXirFunction *function, const char *name) {
    size_t size = strlen(name);
    return function->name_length == size && !memcmp(function->name, name, size);
}

static bool integer_return(const XrXirFunction *function, int64_t expected) {
    if (function->parameter_count || function->result != XR_XIR_I64) return false;
    uint32_t returns = 0;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op != XR_XIR_RETURN) continue;
        if (op->args[0] >= function->instruction_count) return false;
        const XrXirInstruction *value = &function->instructions[op->args[0]];
        if (value->op != XR_XIR_CONST_INT || value->type != XR_XIR_I64 || value->immediate != expected)
            return false;
        ++returns;
    }
    return returns == 1;
}

static bool inspect_effects(PanicRun *run, const XrXirArtifact *artifact,
                            XrXirStage stage, const PanicRoles *roles) {
    OBSERVE(!run->effects);
    run->status = xr_xir_compile_effects_analyze(artifact, &run->effects);
    OBSERVE(run->status == XR_XIR_OK && run->effects);
    uint32_t functions[10] = {roles->entry, roles->initializer,
        roles->functions[ROLE_QUOTIENT], roles->functions[ROLE_READ],
        roles->functions[ROLE_FRESH], roles->functions[ROLE_FAILURE_CLEANUP],
        roles->functions[ROLE_CHECKED], roles->functions[ROLE_ANSWER],
        roles->functions[ROLE_FAILURE], roles->functions[ROLE_SUCCESS]};
    XrXirFunctionEffects facts[10] = {0};
    for (unsigned i = 0; i < 10; ++i) {
        const XrXirFunctionEffects *actual = xr_xir_effects_function(run->effects, functions[i]);
        OBSERVE(actual);
        facts[i] = *actual;
        XrXirEffect expected_suspend = XR_XIR_EFFECT_NONE;
        XrXirEffect expected_throws = XR_XIR_EFFECT_NONE;
        if (i == 2u) expected_suspend = XR_XIR_EFFECT_MAY;
        else if (i >= 6u) {
            expected_suspend = run->fixture->indirect ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_MAY;
            expected_throws = run->fixture->indirect ? XR_XIR_EFFECT_UNKNOWN : XR_XIR_EFFECT_NONE;
        }
        OBSERVE(facts[i].suspend == expected_suspend && facts[i].throws == expected_throws);
    }
    printf("coroutine-panic-native fixture=%s effects stage=%u roles=10 entry=%u/%u initializer=%u/%u "
        "quotient=%u/%u read-cleanup=%u/%u fresh=%u/%u failure-cleanup=%u/%u "
        "checked=%u/%u answer=%u/%u failure=%u/%u success=%u/%u\n",
        run->fixture->id, (unsigned)stage,
        (unsigned)facts[0].suspend, (unsigned)facts[0].throws,
        (unsigned)facts[1].suspend, (unsigned)facts[1].throws,
        (unsigned)facts[2].suspend, (unsigned)facts[2].throws,
        (unsigned)facts[3].suspend, (unsigned)facts[3].throws,
        (unsigned)facts[4].suspend, (unsigned)facts[4].throws,
        (unsigned)facts[5].suspend, (unsigned)facts[5].throws,
        (unsigned)facts[6].suspend, (unsigned)facts[6].throws,
        (unsigned)facts[7].suspend, (unsigned)facts[7].throws,
        (unsigned)facts[8].suspend, (unsigned)facts[8].throws,
        (unsigned)facts[9].suspend, (unsigned)facts[9].throws);
    xr_xir_compile_effects_free(run->effects); run->effects = NULL;
    return true;
}

static bool inspect_cleanup_helpers(PanicRun *run, const XrXirModule *module,
                                    const PanicRoles *roles, uint32_t *count) {
    const XrXirDeclarations *d = module->declarations;
    *count = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *identity = &d->functions[f];
        if (!identity->cleanup_owner) continue;
        OBSERVE(function->result == XR_XIR_UNIT);
        OBSERVE(!identity->exported && !identity->test_role && !identity->test_timeout_seconds);
        OBSERVE(identity->cleanup_owner == roles->functions[ROLE_CHECKED] + 1u);
        OBSERVE(identity->module == d->root_module);
        OBSERVE(f != roles->entry && f != roles->initializer);
        for (unsigned r = 0; r < ROLE_COUNT; ++r) OBSERVE(f != roles->functions[r]);
        ++*count;
    }
    return true;
}

static bool inspect_quotient_reference(PanicRun *run, const XrXirModule *module,
                                       XrXirStage stage, const PanicRoles *roles) {
    if (!run->fixture->indirect) return true;
    unsigned matches = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_FUNCTION_REF || op->immediate != (int64_t)roles->functions[ROLE_QUOTIENT]) continue;
            const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, op->type);
            OBSERVE(signature && signature->kind == XR_XIR_TYPE_CALLABLE && !signature->flags);
            OBSERVE(signature->parameter_count == 1u && signature->parameters &&
                signature->parameters[0].type == XR_XIR_I64 && signature->result == XR_XIR_I64);
            OBSERVE(!op->args[1]);
            ++matches;
        }
    }
    if (stage == XR_XIR_CHECKED) OBSERVE(matches > 0u);
    return true;
}

static bool inspect_module(PanicRun *run, const XrXirArtifact *artifact,
                           XrXirStage stage, PanicRoles *roles) {
    const PanicFixture *chosen = run->fixture;
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *d = module ? module->declarations : NULL;
    OBSERVE(module && module->stage == stage && d && d->root_module < d->module_count);
    roles->entry = d->entry_function;
    roles->initializer = d->modules[d->root_module].initializer;
    for (unsigned r = 0; r < ROLE_COUNT; ++r) roles->functions[r] = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *identity = &d->functions[f];
        if (identity->module != d->root_module) continue;
        for (unsigned r = 0; r < ROLE_COUNT; ++r) {
            if (!named(fn, role_names[r])) continue;
            OBSERVE(roles->functions[r] == UINT32_MAX && !identity->exported);
            OBSERVE(fn->result == (r < ROLE_FRESH ? XR_XIR_I64 : XR_XIR_UNIT));
            uint32_t parameter_count = role_parameter_count(r);
            OBSERVE(fn->parameter_count == parameter_count);
            for (uint32_t p = 0; p < parameter_count; ++p)
                OBSERVE(fn->parameters && fn->parameters[p] == (r == ROLE_CHECKED ? XR_XIR_BOOL : XR_XIR_I64));
            uint32_t expected_role = r < ROLE_FRESH ? XR_XIR_TEST_ROLE_NONE : XR_XIR_TEST_ROLE_TEST;
            OBSERVE(identity->test_role == expected_role && !identity->test_timeout_seconds);
            roles->functions[r] = f;
        }
    }
    OBSERVE(roles->entry < module->function_count && roles->initializer < module->function_count);
    OBSERVE(roles->entry != roles->initializer);
    unsigned private_count = 0, test_count = 0;
    for (unsigned r = 0; r < ROLE_COUNT; ++r) {
        OBSERVE(roles->functions[r] != UINT32_MAX);
        OBSERVE(roles->functions[r] != roles->entry && roles->functions[r] != roles->initializer);
        for (unsigned other = r + 1; other < ROLE_COUNT; ++other)
            OBSERVE(roles->functions[r] != roles->functions[other]);
        if (r < ROLE_FRESH) ++private_count; else ++test_count;
    }
    if (!inspect_quotient_reference(run, module, stage, roles)) return false;
    uint32_t cleanup_helpers = 0;
    if (!inspect_cleanup_helpers(run, module, roles, &cleanup_helpers)) return false;
    uint32_t root_slots = 0;
    for (uint32_t s = 0; s < d->slot_count; ++s) {
        if (d->slots[s].module != d->root_module) continue;
        OBSERVE(d->slots[s].type == XR_XIR_I64 && d->slots[s].mutable == 1u);
        ++root_slots;
    }
    OBSERVE(root_slots == 1u && private_count == 4u && test_count == 4u);
    OBSERVE(integer_return(&module->functions[roles->entry], 0));
    OBSERVE(module->functions[roles->initializer].result == XR_XIR_UNIT);
    OBSERVE(!d->functions[roles->initializer].exported && !d->functions[roles->initializer].test_role);
    printf("coroutine-panic-native fixture=%s metadata stage=%u root=%u entry=%u initializer=%u "
        "private-functions=%u tests=%u root-mutable-I64-slots=%u expected-entry=0 cleanup-helpers=%u\n",
        chosen->id, (unsigned)stage, d->root_module, roles->entry, roles->initializer,
        private_count, test_count, root_slots, cleanup_helpers);
    return inspect_effects(run, artifact, stage, roles);
}

static bool inspect_product(PanicRun *run, const char *file) {
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(run->product);
    const XrXirSourceView *view = xr_xir_compile_source_product_view(run->product);
    const XrXirSourceTests *tests = xr_xir_compile_source_product_tests(run->product);
    const XrXirModule *module = xr_xir_compile_artifact_module(run->closed_checked);
    OBSERVE(facts && view && view->complete && tests && module);
    const XrXirCompileContext *context = xr_xir_compile_source_product_context(run->product);
    OBSERVE(context && context->resources == run->context.resources);
    OBSERVE(facts->target.architecture == XR_XIR_ARCH_X86_64 && facts->target.abi_version == XR_XIR_VALUE_ABI_VERSION);
    OBSERVE(facts->entry == run->roles.entry && facts->function_count == module->function_count);
    OBSERVE(facts->module_count == module->declarations->module_count);
    OBSERVE(tests->count == 4u && tests->entries);
    uint32_t t = 0;
    for (unsigned r = ROLE_FRESH; r < ROLE_COUNT; ++r) {
        const char *name = role_names[r];
        OBSERVE(t < tests->count && tests->entries[t].function == run->roles.functions[r]);
        OBSERVE(tests->entries[t].role == XR_XIR_TEST_ROLE_TEST && !tests->entries[t].timeout_seconds);
        OBSERVE(tests->entries[t].name_length == strlen(name) && !memcmp(tests->entries[t].name, name, strlen(name)));
        ++t;
    }
    OBSERVE(t == tests->count);
    uint32_t root_matches = 0;
    for (uint32_t m = 0; m < view->module_count; ++m) {
        const char *path = view->modules[m].path;
        if (!path || strlen(path) != strlen(file)) continue;
        bool same = true;
        for (size_t i = 0; path[i]; ++i)
            if ((path[i] == '\\' ? '/' : path[i]) != (file[i] == '\\' ? '/' : file[i])) same = false;
        if (same) ++root_matches;
    }
    OBSERVE(root_matches == 1);
    return true;
}

static bool produce(PanicRun *run, const char *root, const char *file) {
    const PanicFixture *chosen = run->fixture;
    XrOsIoPolicy policy = xr_compile_io_policy(run->context.resources);
    run->operation = "original-input";
    XrOsIoStatus io = xr_os_io_read_regular_file(&policy, file, chosen->source_length,
        &run->input, &run->input_length);
    if (io != XR_OS_IO_OK) fprintf(stderr, "coroutine-panic-native fixture=%s input io-status=%u\n", chosen->id, (unsigned)io);
    OBSERVE(io == XR_OS_IO_OK && run->input_length == chosen->source_length);
    OBSERVE(!memcmp(run->input, chosen->source, run->input_length));
    xr_compile_resources_free(run->input); run->input = NULL;
    if (!phase(run, "original-input-verified")) return false;
    run->operation = "session";
    OBSERVE(xr_compile_session_new(run->context.resources, &run->session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{run->session, file, &authority, &run->context,
        NULL, NULL, XR_XIR_PROGRAM, NULL}, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    run->operation = "source-product";
    run->status = xr_xir_compile_source_product_build(&request, &run->product, &run->diagnostic);
    OBSERVE(run->status == XR_XIR_OK && run->product);
    return phase(run, "source-product-complete");
}

static bool read_packets(PanicRun *run, const char *file) {
    XrXirSourceProductPacketView source = {0}, closed = {0};
    PanicRoles source_roles = {0};
    run->operation = "source-packet";
    run->status = xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_SOURCE, &source);
    OBSERVE(run->status == XR_XIR_OK && source.bytes && source.length);
    run->status = xr_xir_compile_checked_read(&run->context, source.bytes, source.length,
        &run->source_checked, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    if (!inspect_module(run, run->source_checked, XR_XIR_CHECKED, &source_roles)) return false;
    if (!phase(run, "source-Checked-reader")) return false;
    run->operation = "closed-packet";
    run->status = xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_CLOSED, &closed);
    OBSERVE(run->status == XR_XIR_OK && closed.bytes && closed.length);
    run->status = xr_xir_compile_checked_read(&run->context, closed.bytes, closed.length,
        &run->closed_checked, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    if (!inspect_module(run, run->closed_checked, XR_XIR_CHECKED, &run->roles)) return false;
    if (!inspect_product(run, file)) return false;
    run->status = xr_xir_compile_checked_write(run->closed_checked, &run->retained, &run->xir);
    OBSERVE(run->status == XR_XIR_OK && run->retained.length == closed.length);
    OBSERVE(run->retained.bytes != closed.bytes && !memcmp(run->retained.bytes, closed.bytes, closed.length));
    return phase(run, "closed-Checked-retained");
}

static bool detach_lower(PanicRun *run) {
    xr_compile_session_free(run->session); run->session = NULL;
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    if (!phase(run, "producers-destroyed")) return false;
    run->status = xr_xir_compile_artifact_verify(run->source_checked, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    run->status = xr_xir_compile_artifact_verify(run->closed_checked, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    XrXirArtifact *reread = NULL;
    run->status = xr_xir_compile_checked_read(&run->context, run->retained.bytes,
        run->retained.length, &reread, &run->xir);
    if (run->status != XR_XIR_OK) { xr_xir_compile_artifact_free(reread); return false; }
    run->status = xr_xir_compile_artifact_verify(reread, &run->xir);
    PanicRoles reread_roles = {0};
    bool same_roles = run->status == XR_XIR_OK && inspect_module(run, reread, XR_XIR_CHECKED, &reread_roles);
    if (same_roles) same_roles = !memcmp(&run->roles, &reread_roles, sizeof(reread_roles));
    xr_xir_compile_artifact_free(reread);
    OBSERVE(same_roles);
    OBSERVE(run->status == XR_XIR_OK);
    run->operation = "Lowered";
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run->status = xr_xir_compile_lower(run->closed_checked, &target, &run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    PanicRoles lowered_roles = {0};
    if (!inspect_module(run, run->lowered, XR_XIR_LOWERED, &lowered_roles)) return false;
    OBSERVE(run->roles.entry == lowered_roles.entry && run->roles.initializer == lowered_roles.initializer);
    for (unsigned r = 0; r < ROLE_COUNT; ++r)
        OBSERVE(run->roles.functions[r] == lowered_roles.functions[r]);
    run->status = xr_xir_compile_artifact_verify(run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&run->retained);
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    xr_xir_compile_artifact_free(run->closed_checked); run->closed_checked = NULL;
    return phase(run, "detached-Lowered-verified");
}

#ifdef XR_SOURCE_COROUTINE_PANIC_NATIVE
extern const XrXirProgramSpec source_coroutine_panic_sealed_program;
extern const char source_coroutine_panic_sealed_c_sha[65];
extern const XrXirProgramSpec source_coroutine_panic_indirect_program;
extern const char source_coroutine_panic_indirect_c_sha[65];
#endif

#ifdef XR_SOURCE_COROUTINE_PANIC_NATIVE
static const XrXirProgramSpec *native_spec(const PanicFixture *chosen) {
    if (!strcmp(chosen->id, "sealed_coroutine_panic")) return &source_coroutine_panic_sealed_program;
    if (!strcmp(chosen->id, "indirect_coroutine_panic")) return &source_coroutine_panic_indirect_program;
    return NULL;
}
#else
static const XrXirProgramSpec *native_spec(const PanicFixture *chosen) { (void)chosen; return NULL; }
#endif

#ifdef XR_SOURCE_COROUTINE_PANIC_NATIVE
static const char *native_sha(const PanicFixture *chosen) {
    if (!strcmp(chosen->id, "sealed_coroutine_panic")) return source_coroutine_panic_sealed_c_sha;
    if (!strcmp(chosen->id, "indirect_coroutine_panic")) return source_coroutine_panic_indirect_c_sha;
    return NULL;
}
#else
static const char *native_sha(const PanicFixture *chosen) { (void)chosen; return NULL; }
#endif

static void hexadecimal(const uint8_t bytes[32], char output[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        output[i * 2] = digits[bytes[i] >> 4];
        output[i * 2 + 1] = digits[bytes[i] & 15u];
    }
    output[64] = '\0';
}

static bool emit_owned(PanicRun *run) {
    run->operation = "emit-first";
    run->status = xr_xir_compile_emit_c(run->lowered, run->fixture->symbol, 16777216u, &run->emitted);
    OBSERVE(run->status == XR_XIR_OK && run->emitted.text && run->emitted.length);
    if (!phase(run, "emission-first")) return false;
    run->operation = "emit-repeat";
    run->status = xr_xir_compile_emit_c(run->lowered, run->fixture->symbol, 16777216u, &run->repeated);
    OBSERVE(run->status == XR_XIR_OK && run->repeated.text && run->repeated.length);
    OBSERVE(run->emitted.text != run->repeated.text && run->emitted.length == run->repeated.length);
    OBSERVE(!memcmp(run->emitted.text, run->repeated.text, run->emitted.length));
    uint8_t digest[32];
    xr_sha256((const uint8_t *)run->emitted.text, run->emitted.length, digest);
    hexadecimal(digest, run->c_digest);
    printf("coroutine-panic-native fixture=%s owned-C count=2 bytes=%zu identical=1 sha256=%s\n",
        run->fixture->id, run->emitted.length, run->c_digest);
    xr_xir_compile_c_source_free(&run->repeated);
    return phase(run, "emission-repeat-freed");
}

static bool write_translation_unit(PanicRun *run, const char *output) {
    run->operation = "write-complete-native-TU";
    FILE *file = fopen(output, "wb");
    OBSERVE(file != NULL);
    bool written = fwrite(run->emitted.text, 1, run->emitted.length, file) == run->emitted.length;
    if (written) written = fprintf(file, "\nconst char %s_c_sha[65]=\"%s\";\n", run->fixture->symbol, run->c_digest) > 0;
    int closed = fclose(file);
    OBSERVE(written && closed == 0);
    printf("coroutine-panic-native fixture=%s emitted-TU payload-sha=%s output=%s callback-ProgramSpec=1 extern-exports=OPEN\n",
        run->fixture->id, run->c_digest, output);
    return true;
}

static bool same_layout(const XrXirFunctionLayout *actual, const XrXirFunctionLayout *expected,
                        uint32_t parameters) {
    if (actual->slot_count != expected->slot_count || actual->frame_bytes != expected->frame_bytes ||
        actual->owned_count != expected->owned_count || actual->outgoing_count != expected->outgoing_count ||
        actual->path_count != expected->path_count || actual->result.size != expected->result.size ||
        actual->result.alignment != expected->result.alignment) return false;
    if (expected->slot_count && (!actual->offsets || !expected->offsets ||
        memcmp(actual->offsets, expected->offsets, (size_t)expected->slot_count * sizeof(uint32_t)))) return false;
    if (expected->owned_count && (!actual->owned_offsets || !expected->owned_offsets ||
        memcmp(actual->owned_offsets, expected->owned_offsets, (size_t)expected->owned_count * sizeof(uint32_t)))) return false;
    if (parameters && (!actual->parameters || !expected->parameters ||
        memcmp(actual->parameters, expected->parameters, (size_t)parameters * sizeof(XrXirLayout)))) return false;
    return true;
}

static bool generated_correspondence(PanicRun *run, const XrXirProgramSpec *spec) {
    const XrXirModule *module = xr_xir_compile_artifact_module(run->lowered);
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    OBSERVE(spec && module && module->declarations && proof.bytes && proof.identity && proof.layouts);
    OBSERVE(native_sha(run->fixture) && !strcmp(native_sha(run->fixture), run->c_digest));
    OBSERVE(spec->abi_version == XR_XIR_PROGRAM_ABI_VERSION && spec->target.architecture == XR_XIR_ARCH_X86_64);
    OBSERVE(spec->target.abi_version == XR_XIR_VALUE_ABI_VERSION && spec->entry_count == module->function_count);
    OBSERVE(spec->entries && spec->declarations && !spec->code.owner && !spec->code.release);
    OBSERVE(spec->types && module->types && spec->types->count == module->types->count);
    OBSERVE(spec->proof.bytes && spec->proof.identity && spec->proof.layouts && proof.length == spec->proof.length);
    OBSERVE(!memcmp(proof.bytes, spec->proof.bytes, proof.length) && !memcmp(proof.identity, spec->proof.identity, 32));
    OBSERVE(spec->declarations->root_module == module->declarations->root_module);
    OBSERVE(spec->declarations->module_count == module->declarations->module_count);
    OBSERVE(spec->declarations->entry_function == run->roles.entry && spec->declarations->functions);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        OBSERVE(!memcmp(&spec->declarations->functions[f], &module->declarations->functions[f], sizeof(XrXirFunctionIdentity)));
        OBSERVE(same_layout(&spec->proof.layouts[f], &proof.layouts[f], module->functions[f].parameter_count));
        OBSERVE(spec->entries[f].abi_version == XR_XIR_CALL_ABI_VERSION && spec->entries[f].resume && spec->entries[f].release);
        OBSERVE(spec->entries[f].parameter_count == module->functions[f].parameter_count && spec->entries[f].result == module->functions[f].result);
        if (module->functions[f].parameter_count)
            OBSERVE(spec->entries[f].parameters && module->functions[f].parameters &&
                !memcmp(spec->entries[f].parameters, module->functions[f].parameters,
                    (size_t)module->functions[f].parameter_count * sizeof(XrXirType)));
    }
    printf("coroutine-panic-native fixture=%s compiled-correspondence functions=%u all-native=1 proof-bytes=%zu layouts-exact=1 permissions-exact=1\n",
        run->fixture->id, spec->entry_count, proof.length);
    return true;
}

static bool seal_native_instances(PanicRun *run) {
    run->operation = "native-Program-seal";
    const XrXirProgramSpec *spec = native_spec(run->fixture);
    if (!generated_correspondence(run, spec)) return false;
    run->status = xr_xir_compile_program_seal(&run->context, spec, &run->program);
    OBSERVE(run->status == XR_XIR_OK && run->program);
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    xr_xir_compile_c_source_free(&run->emitted);
    if (!phase(run, "native-Program-sealed-C-Lowered-dropped")) return false;
    XrXirInstanceConfig config = {0};
    run->call_status = xr_xir_instance_config_init(&config, sizeof(config));
    OBSERVE(run->call_status == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        run->call_status = xr_xir_instance_new(run->program, &config, &run->instances[i]);
        OBSERVE(run->call_status == XR_XIR_CALL_READY && run->instances[i]);
    }
    OBSERVE(run->instances[0] != run->instances[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    return phase(run, "native-Program-caller-dropped-two-instances-retain");
}

typedef struct WaitSnapshot {
    XrCompileResourceStats ledger;
    size_t compiler_attempts, compiler_live, compiler_bytes;
    size_t runtime_attempts, runtime_live, runtime_bytes, capacity;
    const void *table;
} WaitSnapshot;

static bool snapshot_wait(PanicRun *run, WaitSnapshot *saved) {
    OBSERVE(xr_compile_resources_stats(run->context.resources, &saved->ledger) == XR_COMPILE_RESOURCE_OK);
    saved->compiler_attempts = instance_compile_attempts;
    saved->compiler_live = instance_compile_live; saved->compiler_bytes = instance_compile_bytes;
    saved->runtime_attempts = runtime_attempts;
    saved->runtime_live = runtime_live; saved->runtime_bytes = runtime_bytes;
    saved->table = runtime_owned; saved->capacity = runtime_owned_capacity;
    return true;
}

static bool verify_wait_guard(PanicRun *run, XrXirInstance *instance, const WaitSnapshot *saved,
                              uint64_t epoch, uint64_t wake, const XrXirWaitRequest *wait) {
    XrCompileResourceStats after = {0};
    OBSERVE(xr_compile_resources_stats(run->context.resources, &after) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(saved->ledger.allocation_count == after.allocation_count && saved->ledger.allocated_bytes == after.allocated_bytes);
    OBSERVE(saved->ledger.live_bytes == after.live_bytes && saved->ledger.peak_bytes == after.peak_bytes && saved->ledger.work == after.work);
    OBSERVE(instance_compile_attempts == saved->compiler_attempts && instance_compile_live == saved->compiler_live &&
        instance_compile_bytes == saved->compiler_bytes && runtime_attempts == saved->runtime_attempts);
    OBSERVE(runtime_live == saved->runtime_live && runtime_bytes == saved->runtime_bytes &&
        runtime_owned == saved->table && runtime_owned_capacity == saved->capacity);
    OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    XrXirWaitRequest again = {0};
    OBSERVE(xr_xir_instance_wait_request(instance, epoch, wake, &again) == XR_XIR_CALL_READY);
    OBSERVE(again.kind == wait->kind && again.reserved == wait->reserved && again.after_ms == wait->after_ms &&
        again.subject == wait->subject && again.generation == wait->generation && again.ticket == wait->ticket);
    ++run->wait_guard_count;
    return true;
}

static bool yield_guard(PanicRun *run, XrXirInstance *instance, const XrXirInstanceResult *result) {
    OBSERVE(result->epoch && result->epoch != UINT64_MAX && result->outcome.wake);
    OBSERVE(xr_xir_call_result_valid(&result->outcome) && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    WaitSnapshot saved = {0};
    if (!snapshot_wait(run, &saved)) return false;
    XrXirWaitRequest wait = {0}, untouched;
    XrXirValue value;
    unsigned char wait_before[sizeof(untouched)], value_before[sizeof(value)];
    OBSERVE(xr_xir_instance_wait_request(instance, result->epoch, result->outcome.wake, &wait) == XR_XIR_CALL_READY);
    OBSERVE(wait.kind == XR_XIR_WAIT_YIELD && !wait.reserved && !wait.after_ms &&
        !wait.subject && !wait.generation && !wait.ticket);
    OBSERVE(xr_xir_instance_resume(instance, result->epoch + 1u, result->outcome.wake) == XR_XIR_CALL_BAD_STATE);
    if (!verify_wait_guard(run, instance, &saved, result->epoch, result->outcome.wake, &wait)) return false;
    OBSERVE(xr_xir_instance_resume(instance, result->epoch, result->outcome.wake ^ UINT64_C(1)) == XR_XIR_CALL_BAD_STATE);
    if (!verify_wait_guard(run, instance, &saved, result->epoch, result->outcome.wake, &wait)) return false;
    memset(&untouched, 0xa5, sizeof(untouched)); memcpy(wait_before, &untouched, sizeof(untouched));
    OBSERVE(xr_xir_instance_wait_request(instance, result->epoch + 1u, result->outcome.wake, &untouched) == XR_XIR_CALL_BAD_STATE);
    OBSERVE(!memcmp(&untouched, wait_before, sizeof(untouched)));
    if (!verify_wait_guard(run, instance, &saved, result->epoch, result->outcome.wake, &wait)) return false;
    OBSERVE(xr_xir_instance_wait_request(instance, result->epoch, result->outcome.wake ^ UINT64_C(1), &untouched) == XR_XIR_CALL_BAD_STATE);
    OBSERVE(!memcmp(&untouched, wait_before, sizeof(untouched)));
    if (!verify_wait_guard(run, instance, &saved, result->epoch, result->outcome.wake, &wait)) return false;
    memset(&value, 0xa5, sizeof(value)); memcpy(value_before, &value, sizeof(value));
    OBSERVE(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_BAD_STATE);
    OBSERVE(!memcmp(&value, value_before, sizeof(value)));
    if (!verify_wait_guard(run, instance, &saved, result->epoch, result->outcome.wake, &wait)) return false;
    ++run->yield_count;
    printf("coroutine-panic-native fixture=%s instance=%u repeat=%u operation=%s yield=%u "
        "kind=YIELD reserved=0 after-ms=0 subject=NULL generation=0 ticket=0 epoch=%" PRIu64
        " wake=%" PRIu64 " invalid-guards=5 outputs=full-A5-preserved ledger=unchanged physical=unchanged\n",
        run->fixture->id, run->current_instance, run->current_repeat, run->operation,
        run->yield_count, result->epoch, result->outcome.wake);
    return true;
}

static bool poll_terminal(PanicRun *run, XrXirInstance *instance, XrXirInstanceResult *result) {
    size_t polls = 0;
    run->call_yields = 0;
    for (;;) {
        OBSERVE(++polls <= 4096u);
        *result = xr_xir_instance_poll_bounded(instance, UINT64_C(1000000));
        if (result->outcome.status == XR_XIR_CALL_READY) continue;
        if (result->outcome.status != XR_XIR_CALL_SUSPENDED) break;
        OBSERVE(run->call_yields < run->expected_yields);
        if (!yield_guard(run, instance, result)) return false;
        ++run->call_yields;
        OBSERVE(xr_xir_instance_resume(instance, result->epoch, result->outcome.wake) == XR_XIR_CALL_READY);
        ++run->resume_count;
        printf("coroutine-panic-native fixture=%s instance=%u repeat=%u operation=%s resume=%u "
            "epoch=%" PRIu64 " wake=%" PRIu64 " status=READY\n", run->fixture->id,
            run->current_instance, run->current_repeat, run->operation, run->resume_count,
            result->epoch, result->outcome.wake);
    }
    OBSERVE(run->call_yields == run->expected_yields);
    run->call_status = result->outcome.status;
    ++run->driver_calls;
    printf("coroutine-panic-native fixture=%s instance=%u repeat=%u operation=%s driver-yields=%u expected-yields=%u\n",
        run->fixture->id, run->current_instance, run->current_repeat, run->operation,
        run->call_yields, run->expected_yields);
    return true;
}

static bool finish_return(PanicRun *run, XrXirInstance *instance, XrXirType type, int64_t payload) {
    XrXirInstanceResult result = {0};
    if (!poll_terminal(run, instance, &result)) return false;
    OBSERVE(run->call_status == XR_XIR_CALL_RETURNED);
    run->call_status = xr_xir_instance_take_result(instance, &run->value);
    OBSERVE(run->call_status == XR_XIR_CALL_RETURNED);
    bool same = run->value.type == (uint32_t)type && !run->value.reserved && run->value.payload == payload;
    xr_xir_value_drop(&run->value);
    OBSERVE(same && !run->value.type && !run->value.reserved && !run->value.payload);
    OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    if (type == XR_XIR_UNIT) ++run->Unit_count;
    else ++run->canonical_count;
    return true;
}

static bool exact_fault(const PanicFixture *chosen, const XrXirCallResult *result) {
    char scratch[XR_XIR_PANIC_MESSAGE_CAPACITY];
    XrErrorCoreMessageView view = {0};
    const XrXirFaultDetail *detail = &result->panic.detail;
    return xr_xir_call_result_valid(result) && result->status == XR_XIR_CALL_DIVIDE_BY_ZERO &&
        detail->code == chosen->code && !detail->reserved && !detail->index && !detail->length &&
        xr_xir_panic_valid(&result->panic) && result->panic.message.type == XR_XIR_UNIT &&
        !result->panic.message.reserved && !result->panic.message.payload &&
        xr_xir_panic_message_borrow(&result->panic, scratch, sizeof(scratch), &view) &&
        view.has_code && view.code == (int)chosen->code && view.message == scratch &&
        view.message_len == chosen->message_length && !memcmp(view.message, chosen->message, chosen->message_length);
}

static bool finish_failure(PanicRun *run, XrXirInstance *instance, unsigned ordinal, unsigned repeat) {
    XrXirInstanceResult result = {0};
    if (!poll_terminal(run, instance, &result)) return false;
    OBSERVE(run->call_status == XR_XIR_CALL_DIVIDE_BY_ZERO && exact_fault(run->fixture, &result.outcome));
    OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY && run->escaped_count < 4);
    XrXirCallResult *owned = &run->escaped[run->escaped_count];
    OBSERVE(xr_xir_call_result_copy(&result.outcome, owned) == XR_XIR_VALUE_OK);
    ++run->escaped_count;
    OBSERVE(exact_fault(run->fixture, owned));
    XrXirValue untouched;
    XrXirCallResult unavailable;
    memset(&untouched, 0, sizeof(untouched));
    memset(&unavailable, 0, sizeof(unavailable));
    unsigned char value_before[sizeof(untouched)], result_before[sizeof(unavailable)];
    memcpy(value_before, &untouched, sizeof(untouched));
    memcpy(result_before, &unavailable, sizeof(unavailable));
    OBSERVE(xr_xir_instance_take_result(instance, &untouched) == XR_XIR_CALL_BAD_STATE);
    OBSERVE(!memcmp(value_before, &untouched, sizeof(untouched)));
    OBSERVE(xr_xir_instance_copy_failure(instance, &unavailable) == XR_XIR_CALL_BAD_STATE);
    OBSERVE(!memcmp(result_before, &unavailable, sizeof(unavailable)));
    printf("coroutine-panic-native fixture=%s instance=%u repeat=%u failure=DIVIDE_BY_ZERO detail=%u "
        "message-kind=UNIT formatted-bytes=%zu state=READY output-slots=preserved\n",
        run->fixture->id, ordinal, repeat, run->fixture->code, run->fixture->message_length);
    return true;
}

static bool permissions(PanicRun *run, XrXirInstance *instance, unsigned ordinal) {
    run->operation = "private-entry-authority";
    size_t attempts = runtime_attempts, compiler_attempts = instance_compile_attempts;
    size_t blocks = runtime_live, bytes = runtime_bytes, capacity = runtime_owned_capacity;
    size_t compiler_live = instance_compile_live, compiler_bytes = instance_compile_bytes;
    const void *table = runtime_owned;
    XrCompileResourceStats before = {0}, after = {0};
    OBSERVE(xr_compile_resources_stats(run->context.resources, &before) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(xr_xir_instance_start(instance, run->roles.initializer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    unsigned rejected = 1;
    for (unsigned r = 0; r < ROLE_COUNT; ++r) {
        XrXirValue argument = {0};
        argument.type = (uint32_t)(r == ROLE_CHECKED ? XR_XIR_BOOL : XR_XIR_I64);
        uint32_t count = role_parameter_count(r);
        OBSERVE(xr_xir_instance_start(instance, run->roles.functions[r],
            count ? &argument : NULL, count) == XR_XIR_CALL_BAD_ARGUMENT);
        ++rejected;
    }
    OBSERVE(xr_xir_instance_start_test(instance, run->roles.initializer) == XR_XIR_CALL_BAD_ARGUMENT);
    ++rejected;
    for (unsigned r = ROLE_ANSWER; r < ROLE_FRESH; ++r) {
        OBSERVE(xr_xir_instance_start_test(instance, run->roles.functions[r]) == XR_XIR_CALL_BAD_ARGUMENT);
        ++rejected;
    }
    OBSERVE(xr_compile_resources_stats(run->context.resources, &after) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes);
    OBSERVE(before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes && before.work == after.work);
    OBSERVE(instance_compile_attempts == compiler_attempts && runtime_attempts == attempts);
    OBSERVE(instance_compile_live == compiler_live && instance_compile_bytes == compiler_bytes);
    OBSERVE(runtime_live == blocks && runtime_bytes == bytes && runtime_owned == table &&
        runtime_owned_capacity == capacity && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_NEW);
    OBSERVE(rejected == 14u);
    run->permission_count += rejected;
    printf("coroutine-panic-native fixture=%s instance=%u permissions=%u state=NEW ledger=unchanged physical=unchanged\n",
        run->fixture->id, ordinal, rejected);
    return true;
}

static bool unit_test(PanicRun *run, XrXirInstance *instance, unsigned role) {
    run->operation = role_names[role];
    run->expected_yields = role == ROLE_SUCCESS ? 1u : 0u;
    run->call_status = xr_xir_instance_start_test(instance, run->roles.functions[role]);
    OBSERVE(run->call_status == XR_XIR_CALL_READY);
    return finish_return(run, instance, XR_XIR_UNIT, 0);
}

static bool execute_instances(PanicRun *run) {
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = run->instances[i];
        run->current_instance = i; run->current_repeat = 0; run->expected_yields = 0;
        if (!permissions(run, instance, i)) return false;
        run->operation = "canonical-entry";
        run->call_status = xr_xir_instance_start(instance, run->roles.entry, NULL, 0);
        OBSERVE(run->call_status == XR_XIR_CALL_READY);
        if (!finish_return(run, instance, XR_XIR_I64, 0)) return false;
        printf("coroutine-panic-native fixture=%s instance=%u canonical=I64 payload=0\n", run->fixture->id, i);
        if (!unit_test(run, instance, ROLE_FRESH)) return false;
        printf("coroutine-panic-native fixture=%s instance=%u fresh=Unit expected-cleanup=0\n", run->fixture->id, i);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            run->current_repeat = repeat; run->expected_yields = 1u;
            run->operation = role_names[ROLE_FAILURE];
            run->call_status = xr_xir_instance_start_test(instance, run->roles.functions[ROLE_FAILURE]);
            OBSERVE(run->call_status == XR_XIR_CALL_READY);
            if (!finish_failure(run, instance, i, repeat)) return false;
            if (!unit_test(run, instance, ROLE_FAILURE_CLEANUP)) return false;
            printf("coroutine-panic-native fixture=%s instance=%u repeat=%u failure-cleanup=Unit expected-cleanup=22\n",
                run->fixture->id, i, repeat);
            if (!unit_test(run, instance, ROLE_SUCCESS)) return false;
            printf("coroutine-panic-native fixture=%s instance=%u repeat=%u success=Unit expected-value=42 expected-cleanup=11\n",
                run->fixture->id, i, repeat);
        }
        OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    }
    OBSERVE(run->escaped_count == 4 && run->canonical_count == 2);
    OBSERVE(run->Unit_count == 10u && run->permission_count == 28u);
    OBSERVE(run->yield_count == 8u && run->resume_count == 8u && run->wait_guard_count == 40u && run->driver_calls == 16u);
    return phase(run, "original-panic-success-and-cleanup-complete");
}

static bool release(PanicRun *run) {
    bool complete = true;
    xr_xir_compile_effects_free(run->effects); run->effects = NULL;
    xr_xir_value_drop(&run->value);
    xr_xir_compile_c_source_free(&run->emitted);
    xr_xir_compile_c_source_free(&run->repeated);
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->instances[i]) continue;
        XrXirCallStatus status = xr_xir_instance_free(run->instances[i]);
        if (status == XR_XIR_CALL_BUSY) complete = false;
        else { run->instances[i] = NULL; if (status != XR_XIR_CALL_READY) complete = false; }
    }
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    xr_xir_compile_artifact_free(run->closed_checked); run->closed_checked = NULL;
    xr_xir_compile_checked_packet_free(&run->retained);
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_compile_session_free(run->session); run->session = NULL;
    xr_compile_resources_free(run->input); run->input = NULL;
    if (run->context.resources) {
        if (xr_compile_resources_stats(run->context.resources, &run->final) != XR_COMPILE_RESOURCE_OK)
            complete = false;
        if (run->final.live_bytes != run->baseline.live_bytes || run->final.live_bytes != instance_compile_bytes)
            complete = false;
        xr_compile_resources_release(run->context.resources); run->context.resources = NULL;
    }
    if (instance_compile_live || instance_compile_bytes) complete = false;
    printf("coroutine-panic-native fixture=%s compiler-owner-released compiler=%zu/%zu escaped-copies=%u\n",
        run->fixture->id, instance_compile_live, instance_compile_bytes, run->escaped_count);
    for (unsigned i = 0; i < run->escaped_count; ++i) {
        if (!exact_fault(run->fixture, &run->escaped[i])) complete = false;
        else printf("coroutine-panic-native fixture=%s escaped=%u after-all-execution-owners=panic%u-Unit-facts-formatted-%zuB-exact\n",
            run->fixture->id, i, run->fixture->code, run->fixture->message_length);
        xr_xir_call_result_drop(&run->escaped[i]);
        if (!xr_xir_call_result_empty(&run->escaped[i])) complete = false;
    }
    if (runtime_live || runtime_bytes || runtime_owned || runtime_owned_capacity) complete = false;
    printf("coroutine-panic-native fixture=%s release compiler=%zu/%zu runtime=%zu/%zu table=%zu result=%s\n",
        run->fixture->id, instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes,
        runtime_owned_capacity, complete ? "PASS" : "FAIL");
    return complete;
}

_Static_assert(sizeof(source_sealed_coroutine_panic) - 1u == 824u, "Complete sealed call source and private Tests");
_Static_assert(sizeof(source_indirect_coroutine_panic) - 1u == 848u, "Complete indirect call source and private Tests");

int main(int argc, char **argv) {
    if (argc != 4 && argc != 6) return 2;
    const PanicFixture *chosen = select_fixture(argv[1]);
    if (!chosen) return 2;
    const char *output = NULL;
    if (argc == 6 && !strcmp(argv[4], "--emit")) output = argv[5];
    else if (argc != 4 || !native_spec(chosen)) return 2;
    instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    PanicRun run = {0};
    run.fixture = chosen;
    run.context.limits = xr_xir_compile_default_limits();
    run.operation = "finite-owner";
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrCompileResourceStatus owner = xr_compile_resources_new(&limits, &run.context.resources);
    bool passed = owner == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = xr_compile_resources_stats(run.context.resources, &run.baseline) == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = phase(&run, "owner-created") && produce(&run, argv[2], argv[3]);
    if (passed) passed = read_packets(&run, argv[3]) && detach_lower(&run) && emit_owned(&run);
    if (passed) passed = output ? write_translation_unit(&run, output) :
        (seal_native_instances(&run) && execute_instances(&run));
    if (!passed) fprintf(stderr, "coroutine-panic-native fixture=%s failure operation=%s owner-status=%u status=%u "
        "source-stage=%u source-status=%u module=%u line=%d column=%d xir-status=%u "
        "function=%u block=%u instruction=%u reason=%u call-status=%u message=%s\n", chosen->id, run.operation,
        (unsigned)owner, (unsigned)run.status, (unsigned)run.diagnostic.stage,
        (unsigned)run.diagnostic.source.status, run.diagnostic.source.module, run.diagnostic.source.line,
        run.diagnostic.source.column, (unsigned)run.xir.status, run.xir.function, run.xir.block,
        run.xir.instruction, (unsigned)run.xir.reason, (unsigned)run.call_status, run.diagnostic.source.message);
    bool released = release(&run);
    printf("coroutine-panic-native fixture=%s mode=%s result=%s compiler-sites=%zu runtime-sites=%zu allocated=%" PRIu64
        " peak=%" PRIu64 " work=%" PRIu64 " canonical=%u Unit=%u copied-panic-facts=%u permissions=%u "
        "message-kind=UNIT message-storage=CALLER_SCRATCH pointer-uniqueness=NOT_REQUIRED heap-allocation=NOT_REQUIRED "
        "yield-count=%u resume-count=%u wait-invalid-guards=%u driver-calls=%u "
        "business-catch7=NOT_APPLICABLE cancel=NOT_RUN structured-Task=OPEN "
        "callbacks=%s full-FI=NOT_RUN axes=NOT_RUN class-identities=OPEN original-stderr=OPEN\n",
        chosen->id, output ? "emit" : "native", passed && released ? "PASS" : "FAIL", instance_compile_attempts, runtime_attempts,
        run.final.allocated_bytes, run.final.peak_bytes, run.final.work,
        run.canonical_count, run.Unit_count, run.escaped_count, run.permission_count,
        run.yield_count, run.resume_count, run.wait_guard_count, run.driver_calls, output ? "NOT_EXECUTED" : "NATIVE");
    return passed && released ? 0 : 1;
}
