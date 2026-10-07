/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_coroutine_panic_group4.c - Original coroutine panic and invoke defer families
 *
 * KEY CONCEPT:
 *   Each process preserves one original source and executes only same-module tests.
 *   Copied intrinsic facts outlive owners; canonical text uses caller scratch.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_panic.h"
#include "xir/xxir_effects.h"
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

static const char source_sealed_coroutine_invoke[] =
    "enum DivisionFailure { Negative }\n"
    "fn quotient(divisor: i64) -> i64 {\n"
    "Coro.yield()\n"
    "if (divisor < 0) { throw DivisionFailure.Negative }\n"
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
    "var result = 0\n"
    "try { result = quotient(divisor) }\n"
    "catch (error: DivisionFailure) { result = 7 }\n"
    "var caughtResult = 0\n"
    "try { caughtResult = quotient(-1) }\n"
    "catch (error: DivisionFailure) { caughtResult = 7 }\n"
    "assert(caughtResult == 7)\n"
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

static const char source_indirect_coroutine_invoke[] =
    "enum DivisionFailure { Negative }\n"
    "fn quotient(divisor: i64) -> i64 {\n"
    "Coro.yield()\n"
    "if (divisor < 0) { throw DivisionFailure.Negative }\n"
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
    "var result = 0\n"
    "try { result = selected(divisor) }\n"
    "catch (error: DivisionFailure) { result = 7 }\n"
    "var caughtResult = 0\n"
    "try { caughtResult = selected(-1) }\n"
    "catch (error: DivisionFailure) { caughtResult = 7 }\n"
    "assert(caughtResult == 7)\n"
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
    bool deferred;
    unsigned dispatch;
} PanicFixture;
static const PanicFixture panic_fixtures[] = {
    {"sealed_coroutine_invoke", source_sealed_coroutine_invoke, sizeof(source_sealed_coroutine_invoke) - 1, 420u, "division by zero", 16u, true, 5u},
    {"indirect_coroutine_invoke", source_indirect_coroutine_invoke, sizeof(source_indirect_coroutine_invoke) - 1, 420u, "division by zero", 16u, true, 6u},
    {"sealed_coroutine_panic", source_sealed_coroutine_panic, sizeof(source_sealed_coroutine_panic) - 1, 420u, "division by zero", 16u, true, 1u},
    {"indirect_coroutine_panic", source_indirect_coroutine_panic, sizeof(source_indirect_coroutine_panic) - 1, 420u, "division by zero", 16u, true, 2u}
};

enum PanicRole {
    ROLE_ANSWER, ROLE_CHECKED, ROLE_READ, ROLE_QUOTIENT, ROLE_RELAY,
    ROLE_FRESH, ROLE_FAILURE, ROLE_FAILURE_CLEANUP, ROLE_SUCCESS, ROLE_COUNT
};
static const char *const role_names[ROLE_COUNT] = {
    "answer", "checked", "readCleanupCount", "quotient", "relay",
    "checkFreshCleanup", "checkPanicFailure", "checkFailureCleanup", "checkPanicSuccess"
};
typedef struct PanicRoles {
    uint32_t entry, initializer, functions[ROLE_COUNT];
} PanicRoles;

static bool role_enabled(const PanicFixture *chosen, unsigned role) {
    if (role == ROLE_READ || role == ROLE_FRESH || role == ROLE_FAILURE_CLEANUP)
        return chosen->deferred;
    if (role == ROLE_QUOTIENT) return chosen->dispatch != 0;
    if (role == ROLE_RELAY) return chosen->dispatch == 4u;
    return role < ROLE_COUNT;
}

static uint32_t role_parameter_count(const PanicFixture *chosen, unsigned role) {
    if (role == ROLE_CHECKED) return 1u;
    if (role == ROLE_QUOTIENT) return chosen->dispatch == 4u ? 2u : 1u;
    return role == ROLE_RELAY ? 2u : 0u;
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
    XrXirInstance *instances[2];
    XrXirValue value;
    PanicRoles roles;
    XrXirCallResult escaped[4];
    unsigned escaped_count, canonical_count, Unit_count;
    unsigned expected_yields, call_yields, yield_count, resume_count, wait_guard_count;
    unsigned current_instance, current_repeat;
    const char *operation;
    XrXirStatus status;
    XrXirCallStatus call_status;
    uint8_t *input;
    size_t input_length;
} PanicRun;

#define OBSERVE(condition) do { if (!(condition)) { \
    fprintf(stderr, "coroutine-panic fixture=%s check line=%d operation=%s condition=%s\n", \
        run->fixture->id, __LINE__, run->operation, #condition); return false; } } while (0)

static bool phase(PanicRun *run, const char *name) {
    XrCompileResourceStats stats = {0};
    run->operation = name;
    OBSERVE(xr_compile_resources_stats(run->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(stats.live_bytes == instance_compile_bytes);
    printf("coroutine-panic fixture=%s phase=%s sites=%zu allocations=%" PRIu64 " allocated=%" PRIu64
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
            OBSERVE(role_enabled(chosen, r) && roles->functions[r] == UINT32_MAX && !identity->exported);
            OBSERVE(fn->result == (r < ROLE_FRESH ? XR_XIR_I64 : XR_XIR_UNIT));
            uint32_t parameter_count = role_parameter_count(chosen, r);
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
        if (!role_enabled(chosen, r)) { OBSERVE(roles->functions[r] == UINT32_MAX); continue; }
        OBSERVE(roles->functions[r] != UINT32_MAX);
        OBSERVE(roles->functions[r] != roles->entry && roles->functions[r] != roles->initializer);
        for (unsigned other = r + 1; other < ROLE_COUNT; ++other)
            if (role_enabled(chosen, other)) OBSERVE(roles->functions[r] != roles->functions[other]);
        if (r < ROLE_FRESH) ++private_count; else ++test_count;
    }
    uint32_t root_slots = 0;
    for (uint32_t s = 0; s < d->slot_count; ++s) {
        if (d->slots[s].module != d->root_module) continue;
        OBSERVE(d->slots[s].type == XR_XIR_I64 && d->slots[s].mutable == 1u);
        ++root_slots;
    }
    OBSERVE(root_slots == (chosen->deferred ? 1u : 0u));
    OBSERVE(integer_return(&module->functions[roles->entry], 0));
    OBSERVE(module->functions[roles->initializer].result == XR_XIR_UNIT);
    OBSERVE(!d->functions[roles->initializer].exported && !d->functions[roles->initializer].test_role);
    printf("coroutine-panic fixture=%s metadata stage=%u root=%u entry=%u initializer=%u "
        "private-functions=%u tests=%u root-mutable-I64-slots=%u expected-entry=0\n",
        chosen->id, (unsigned)stage, d->root_module, roles->entry, roles->initializer,
        private_count, test_count, root_slots);
    return true;
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
    OBSERVE(tests->count == (run->fixture->deferred ? 4u : 2u) && tests->entries);
    uint32_t t = 0;
    for (unsigned r = ROLE_FRESH; r < ROLE_COUNT; ++r) {
        if (!role_enabled(run->fixture, r)) continue;
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
    if (io != XR_OS_IO_OK) fprintf(stderr, "coroutine-panic fixture=%s input io-status=%u\n", chosen->id, (unsigned)io);
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

static bool suspend_facts(PanicRun *run) {
    XrXirEffects *summary = NULL;
    run->operation = "Lowered-suspend-effects";
    run->status = xr_xir_compile_effects_analyze(run->lowered, &summary);
    bool valid = run->status == XR_XIR_OK && summary;
    XrXirEffect propagated = run->fixture->dispatch % 2u ? XR_XIR_EFFECT_MAY : XR_XIR_EFFECT_UNKNOWN;
    unsigned non_suspending = 0, propagating = 0;
    for (unsigned r = 0; valid && r < ROLE_COUNT; ++r) {
        if (!role_enabled(run->fixture, r)) continue;
        XrXirEffect expected = XR_XIR_EFFECT_NONE;
        if (r == ROLE_QUOTIENT) expected = XR_XIR_EFFECT_MAY;
        else if (r == ROLE_CHECKED || r == ROLE_ANSWER || r == ROLE_FAILURE || r == ROLE_SUCCESS) {
            expected = propagated;
            ++propagating;
        } else ++non_suspending;
        const XrXirFunctionEffects *facts = xr_xir_effects_function(summary, run->roles.functions[r]);
        valid = facts && facts->suspend == expected;
    }
    const XrXirFunctionEffects *entry = xr_xir_effects_function(summary, run->roles.entry);
    const XrXirFunctionEffects *initializer = xr_xir_effects_function(summary, run->roles.initializer);
    valid = valid && entry && initializer && entry->suspend == XR_XIR_EFFECT_NONE &&
        initializer->suspend == XR_XIR_EFFECT_NONE && non_suspending == 3u && propagating == 4u;
    xr_xir_compile_effects_free(summary);
    OBSERVE(valid);
    printf("coroutine-panic fixture=%s effects stage=LOWERED quotient=MAY propagated=%s "
        "propagated-functions=4 non-suspending-functions=5 summary=freed\n",
        run->fixture->id, propagated == XR_XIR_EFFECT_MAY ? "MAY" : "UNKNOWN");
    return true;
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
    if (!suspend_facts(run)) return false;
    xr_xir_compile_checked_packet_free(&run->retained);
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    xr_xir_compile_artifact_free(run->closed_checked); run->closed_checked = NULL;
    return phase(run, "detached-Lowered-verified");
}

static bool seal_instances(PanicRun *run) {
    run->operation = "VM-Program-take";
    run->status = xr_xir_compile_vm_program_take(&run->lowered, &run->program);
    OBSERVE(run->status == XR_XIR_OK && !run->lowered && run->program);
    if (!phase(run, "Program-sealed")) return false;
    XrXirInstanceConfig config = {0};
    run->call_status = xr_xir_instance_config_init(&config, sizeof(config));
    OBSERVE(run->call_status == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i) {
        run->call_status = xr_xir_instance_new(run->program, &config, &run->instances[i]);
        OBSERVE(run->call_status == XR_XIR_CALL_READY && run->instances[i]);
    }
    OBSERVE(run->instances[0] != run->instances[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    return phase(run, "Program-caller-dropped-two-instances-retain");
}

static bool yield_guard(PanicRun *run, XrXirInstance *instance,
                        const XrXirInstanceResult *result) {
    OBSERVE(result->epoch && result->epoch != UINT64_MAX && result->outcome.wake);
    OBSERVE(xr_xir_call_result_valid(&result->outcome) &&
        xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    const size_t attempts = runtime_attempts, compiler_attempts = instance_compile_attempts;
    const size_t blocks = runtime_live, bytes = runtime_bytes, capacity = runtime_owned_capacity;
    const void *table = runtime_owned;
    XrCompileResourceStats before = {0}, after = {0};
    OBSERVE(xr_compile_resources_stats(run->context.resources, &before) == XR_COMPILE_RESOURCE_OK);
    XrXirWaitRequest wait = {0}, again = {0}, untouched;
    XrXirValue value;
    unsigned char wait_before[sizeof(untouched)], value_before[sizeof(value)];
    OBSERVE(xr_xir_instance_wait_request(instance, result->epoch, result->outcome.wake, &wait) == XR_XIR_CALL_READY);
    OBSERVE(wait.kind == XR_XIR_WAIT_YIELD && !wait.reserved && !wait.after_ms &&
        !wait.subject && !wait.generation && !wait.ticket);
    OBSERVE(xr_xir_instance_resume(instance, result->epoch + 1u, result->outcome.wake) == XR_XIR_CALL_BAD_STATE);
    OBSERVE(xr_xir_instance_resume(instance, result->epoch, result->outcome.wake ^ UINT64_C(1)) == XR_XIR_CALL_BAD_STATE);
    memset(&untouched, 0xa5, sizeof(untouched)); memcpy(wait_before, &untouched, sizeof(untouched));
    OBSERVE(xr_xir_instance_wait_request(instance, result->epoch + 1u, result->outcome.wake, &untouched) == XR_XIR_CALL_BAD_STATE);
    OBSERVE(!memcmp(&untouched, wait_before, sizeof(untouched)));
    OBSERVE(xr_xir_instance_wait_request(instance, result->epoch, result->outcome.wake ^ UINT64_C(1), &untouched) == XR_XIR_CALL_BAD_STATE);
    OBSERVE(!memcmp(&untouched, wait_before, sizeof(untouched)));
    memset(&value, 0xa5, sizeof(value)); memcpy(value_before, &value, sizeof(value));
    OBSERVE(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_BAD_STATE);
    OBSERVE(!memcmp(&value, value_before, sizeof(value)));
    OBSERVE(xr_xir_instance_wait_request(instance, result->epoch, result->outcome.wake, &again) == XR_XIR_CALL_READY);
    OBSERVE(again.kind == wait.kind && again.reserved == wait.reserved && again.after_ms == wait.after_ms &&
        again.subject == wait.subject && again.generation == wait.generation && again.ticket == wait.ticket);
    OBSERVE(xr_compile_resources_stats(run->context.resources, &after) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes);
    OBSERVE(before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes && before.work == after.work);
    OBSERVE(instance_compile_attempts == compiler_attempts && runtime_attempts == attempts);
    OBSERVE(runtime_live == blocks && runtime_bytes == bytes && runtime_owned == table &&
        runtime_owned_capacity == capacity && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    ++run->yield_count; run->wait_guard_count += 5u;
    printf("coroutine-panic fixture=%s instance=%u repeat=%u operation=%s yield=%u "
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
        printf("coroutine-panic fixture=%s instance=%u repeat=%u operation=%s resume=%u "
            "epoch=%" PRIu64 " wake=%" PRIu64 " status=READY\n",
            run->fixture->id, run->current_instance, run->current_repeat, run->operation,
            run->resume_count, result->epoch, result->outcome.wake);
    }
    OBSERVE(run->call_yields == run->expected_yields);
    run->call_status = result->outcome.status;
    printf("coroutine-panic fixture=%s instance=%u repeat=%u operation=%s "
        "driver-yields=%u expected-yields=%u\n", run->fixture->id, run->current_instance,
        run->current_repeat, run->operation, run->call_yields, run->expected_yields);
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
    printf("coroutine-panic fixture=%s instance=%u repeat=%u failure=DIVIDE_BY_ZERO detail=%u "
        "message-kind=UNIT bytes=%zu state=READY output-slots=preserved\n",
        run->fixture->id, ordinal, repeat, run->fixture->code, run->fixture->message_length);
    return true;
}

static bool permissions(PanicRun *run, XrXirInstance *instance, unsigned ordinal) {
    run->operation = "private-entry-authority";
    size_t attempts = runtime_attempts, compiler_attempts = instance_compile_attempts;
    size_t blocks = runtime_live, bytes = runtime_bytes;
    XrCompileResourceStats before = {0}, after = {0};
    OBSERVE(xr_compile_resources_stats(run->context.resources, &before) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(xr_xir_instance_start(instance, run->roles.initializer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    unsigned rejected = 1;
    for (unsigned r = 0; r < ROLE_COUNT; ++r) {
        if (!role_enabled(run->fixture, r)) continue;
        XrXirValue arguments[2] = {{0}, {0}};
        arguments[0].type = (uint32_t)(r == ROLE_CHECKED ? XR_XIR_BOOL : XR_XIR_I64);
        arguments[1].type = XR_XIR_I64;
        uint32_t count = role_parameter_count(run->fixture, r);
        if (count == 2u) arguments[0].payload = 3;
        OBSERVE(xr_xir_instance_start(instance, run->roles.functions[r],
            count ? arguments : NULL, count) == XR_XIR_CALL_BAD_ARGUMENT);
        ++rejected;
    }
    OBSERVE(xr_xir_instance_start_test(instance, run->roles.initializer) == XR_XIR_CALL_BAD_ARGUMENT);
    ++rejected;
    for (unsigned r = ROLE_ANSWER; r < ROLE_FRESH; ++r) {
        if (!role_enabled(run->fixture, r)) continue;
        OBSERVE(xr_xir_instance_start_test(instance, run->roles.functions[r]) == XR_XIR_CALL_BAD_ARGUMENT);
        ++rejected;
    }
    OBSERVE(xr_compile_resources_stats(run->context.resources, &after) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes);
    OBSERVE(before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes && before.work == after.work);
    OBSERVE(instance_compile_attempts == compiler_attempts && runtime_attempts == attempts);
    OBSERVE(runtime_live == blocks && runtime_bytes == bytes && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_NEW);
    unsigned expected = 10u + (run->fixture->deferred ? 4u : 0u) + (run->fixture->dispatch == 4u ? 2u : 0u);
    OBSERVE(rejected == expected);
    printf("coroutine-panic fixture=%s instance=%u permissions=%u state=NEW ledger=unchanged physical=unchanged\n",
        run->fixture->id, ordinal, rejected);
    return true;
}

static bool unit_test(PanicRun *run, XrXirInstance *instance, unsigned role) {
    run->operation = role_names[role];
    run->expected_yields = role == ROLE_SUCCESS ? (run->fixture->dispatch >= 5u ? 2u : 1u) : 0u;
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
        printf("coroutine-panic fixture=%s instance=%u canonical=I64 payload=0\n", run->fixture->id, i);
        if (run->fixture->deferred) {
            if (!unit_test(run, instance, ROLE_FRESH)) return false;
            printf("coroutine-panic fixture=%s instance=%u fresh=Unit expected-cleanup=0\n", run->fixture->id, i);
        }
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            run->current_repeat = repeat; run->expected_yields = 1u;
            run->operation = role_names[ROLE_FAILURE];
            run->call_status = xr_xir_instance_start_test(instance, run->roles.functions[ROLE_FAILURE]);
            OBSERVE(run->call_status == XR_XIR_CALL_READY);
            if (!finish_failure(run, instance, i, repeat)) return false;
            if (run->fixture->deferred) {
                if (!unit_test(run, instance, ROLE_FAILURE_CLEANUP)) return false;
                printf("coroutine-panic fixture=%s instance=%u repeat=%u failure-cleanup=Unit expected-cleanup=22\n",
                    run->fixture->id, i, repeat);
            }
            if (!unit_test(run, instance, ROLE_SUCCESS)) return false;
            printf("coroutine-panic fixture=%s instance=%u repeat=%u success=Unit expected-value=42 expected-cleanup=%s\n",
                run->fixture->id, i, repeat, run->fixture->deferred ? "11" : "NA");
        }
        OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    }
    OBSERVE(run->escaped_count == 4 && run->canonical_count == 2);
    OBSERVE(run->Unit_count == (run->fixture->deferred ? 10u : 4u));
    unsigned expected_yields = run->fixture->dispatch >= 5u ? 12u : 8u;
    OBSERVE(run->yield_count == expected_yields && run->resume_count == expected_yields);
    OBSERVE(run->wait_guard_count == expected_yields * 5u);
    return phase(run, "original-panic-success-and-cleanup-complete");
}

static bool release(PanicRun *run) {
    bool complete = true;
    xr_xir_value_drop(&run->value);
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
    printf("coroutine-panic fixture=%s compiler-owner-released compiler=%zu/%zu escaped-copies=%u\n",
        run->fixture->id, instance_compile_live, instance_compile_bytes, run->escaped_count);
    for (unsigned i = 0; i < run->escaped_count; ++i) {
        if (!exact_fault(run->fixture, &run->escaped[i])) complete = false;
        else printf("coroutine-panic fixture=%s escaped=%u after-all-execution-owners=panic%u-Unit-message-%zuB-exact\n",
            run->fixture->id, i, run->fixture->code, run->fixture->message_length);
        xr_xir_call_result_drop(&run->escaped[i]);
        if (!xr_xir_call_result_empty(&run->escaped[i])) complete = false;
    }
    if (runtime_live || runtime_bytes || runtime_owned || runtime_owned_capacity) complete = false;
    printf("coroutine-panic fixture=%s release compiler=%zu/%zu runtime=%zu/%zu table=%zu result=%s\n",
        run->fixture->id, instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes,
        runtime_owned_capacity, complete ? "PASS" : "FAIL");
    return complete;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    const PanicFixture *chosen = select_fixture(argv[1]);
    if (!chosen) return 2;
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
    if (passed) passed = read_packets(&run, argv[3]) && detach_lower(&run) && seal_instances(&run) && execute_instances(&run);
    if (!passed) fprintf(stderr, "coroutine-panic fixture=%s failure operation=%s owner-status=%u status=%u "
        "source-stage=%u source-status=%u module=%u line=%d column=%d xir-status=%u "
        "function=%u block=%u instruction=%u reason=%u call-status=%u message=%s\n", chosen->id, run.operation,
        (unsigned)owner, (unsigned)run.status, (unsigned)run.diagnostic.stage,
        (unsigned)run.diagnostic.source.status, run.diagnostic.source.module, run.diagnostic.source.line,
        run.diagnostic.source.column, (unsigned)run.xir.status, run.xir.function, run.xir.block,
        run.xir.instruction, (unsigned)run.xir.reason, (unsigned)run.call_status, run.diagnostic.source.message);
    bool released = release(&run);
    printf("coroutine-panic fixture=%s normal=%s compiler-sites=%zu runtime-sites=%zu allocated=%" PRIu64
        " peak=%" PRIu64 " work=%" PRIu64 " canonical=%u Unit=%u copied-panic-facts=%u "
        "intrinsic-message=Unit canonical-message=%zuB-scratch owned-message=NOT_CLAIMED "
        "original-caught-assert=%s yield-count=%u resume-count=%u wait-invalid-guards=%u "
        "cancel=NOT_RUN native=NOT_RUN full-FI=NOT_RUN class-identities=OPEN original-stderr=OPEN\n",
        chosen->id, passed && released ? "PASS" : "FAIL", instance_compile_attempts, runtime_attempts,
        run.final.allocated_bytes, run.final.peak_bytes, run.final.work,
        run.canonical_count, run.Unit_count, run.escaped_count, chosen->message_length,
        chosen->dispatch >= 5u ? "7" : "NA", run.yield_count, run.resume_count, run.wait_guard_count);
    return passed && released ? 0 : 1;
}
