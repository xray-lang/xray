/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_atomic_source_families_native.c - Full Atomic native Programs
 *
 * KEY CONCEPT:
 *   Complete generated callbacks execute the original Atomic families through
 *   private same-module tests; owned C and Lowered data die before execution.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include "base/xsha256.h"
#include "xir/xxir_vm.h"
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

#define ATOMIC_I64_SOURCE \
    "fn answer() -> i64 {\n" \
    " const counter = Atomic(40)\n" \
    " const alias = counter\n" \
    " var (old, matched) = alias.compareExchange(40, 42)\n" \
    " counter.store(42, Ordering.Release)\n" \
    " counter.add(1)\n" \
    " counter.sub(1)\n" \
    " return counter.load()\n" \
    "}\n" \
    "fn exported() -> i64 { return 42 }\n" \
    "@test\n" \
    "fn checkAnswer() { assert(exported() == 42) }\n"
#define ATOMIC_BOOL_SOURCE \
    "fn answer() -> i64 {\n" \
    " const flag = Atomic(false)\n" \
    " const alias = flag\n" \
    " var (old, matched) = alias.compareExchange(false, true)\n" \
    " flag.store(true, Ordering.Release)\n" \
    " const before = flag.toggle(Ordering.Relaxed)\n" \
    " if (matched && before) { return 42 }\n" \
    " return 0\n" \
    "}\n" \
    "fn exported() -> i64 { return 42 }\n" \
    "@test\n" \
    "fn checkAnswer() { assert(exported() == 42) }\n"
#define ATOMIC_F64_SOURCE \
    "fn identity(value: f64) -> f64 { return value }\n" \
    "fn answer() -> i64 {\n" \
    " const value = identity(1.5)\n" \
    " const other = identity(2.25)\n" \
    " const counter = Atomic(value)\n" \
    " const old = counter.fetchAdd(other)\n" \
    " var (before, matched) = counter.compareExchange(3.75, 1.5)\n" \
    " counter.store(1.5, Ordering.Release)\n" \
    " const text = counter.load().toString()\n" \
    " if (old < other && len(text) == 3) { return 42 }\n" \
    " return 0\n" \
    "}\n" \
    "fn exported() -> i64 { return 42 }\n" \
    "@test\n" \
    "fn checkAnswer() { assert(exported() == 42) }\n"
#define ATOMIC_ANSWER_TEST_SOURCE \
    "@test\n" \
    "fn checkAtomicAnswer() { assert(answer() == 42) }\n"
static const char test_i64[] = ATOMIC_I64_SOURCE ATOMIC_ANSWER_TEST_SOURCE;
static const char test_bool[] = ATOMIC_BOOL_SOURCE ATOMIC_ANSWER_TEST_SOURCE;
static const char test_f64[] = ATOMIC_F64_SOURCE ATOMIC_ANSWER_TEST_SOURCE;

typedef struct AtomicFixture {
    const char *scenario, *mode, *source;
    size_t length;
    bool atomic_test;
    const char *symbol;
} AtomicFixture;

static const AtomicFixture fixtures[] = {
    {"1", "atomic", test_i64, sizeof(test_i64) - 1, true, "source_atomic_i64_family"},
    {"2", "atomic", test_bool, sizeof(test_bool) - 1, true, "source_atomic_bool_family"},
    {"3", "atomic", test_f64, sizeof(test_f64) - 1, true, "source_atomic_f64_family"}
};

static const AtomicFixture *select_fixture(const char *scenario, const char *mode) {
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i)
        if (!strcmp(scenario, fixtures[i].scenario) && !strcmp(mode, fixtures[i].mode)) return &fixtures[i];
    return NULL;
}

typedef struct AtomicRoles {
    uint32_t entry, initializer, answer, exported, test, atomic_test;
} AtomicRoles;

typedef struct AtomicRun {
    XrXirCompileContext context;
    XrCompileResourceStats baseline, final;
    XrCompilerSession *session;
    XrXirSourceProduct *product;
    XrXirSourceProductDiagnostic diagnostic;
    XrXirDiagnostic xir;
    XrXirArtifact *source_checked, *closed_checked, *lowered;
    XrXirCheckedPacket retained;
    XrXirCSource emitted, repeated;
    char c_digest[65];
    XrXirProgram *program;
    XrXirInstance *instances[2];
    XrXirValue value;
    AtomicRoles roles;
    const AtomicFixture *fixture;
    const char *operation;
    XrXirStatus status;
    XrXirCallStatus call_status;
    uint8_t *input;
    size_t input_length;
} AtomicRun;

#define OBSERVE(condition) do { if (!(condition)) { \
    fprintf(stderr, "atomic-families-native check line=%d operation=%s condition=%s\n", \
        __LINE__, run->operation, #condition); return false; } } while (0)

static bool phase(AtomicRun *run, const char *name) {
    XrCompileResourceStats stats = {0};
    run->operation = name;
    OBSERVE(xr_compile_resources_stats(run->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(stats.live_bytes == instance_compile_bytes);
    printf("atomic-families-native phase=%s sites=%zu allocations=%" PRIu64 " allocated=%" PRIu64
        " live=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 " physical=%zu/%zu\n",
        name, instance_compile_attempts, stats.allocation_count, stats.allocated_bytes,
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

static bool inspect_module(AtomicRun *run, const XrXirArtifact *artifact,
                           XrXirStage stage, AtomicRoles *roles) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *d = module ? module->declarations : NULL;
    OBSERVE(module && module->stage == stage && d && d->root_module < d->module_count);
    *roles = (AtomicRoles){d->entry_function, d->modules[d->root_module].initializer,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *identity = &d->functions[f];
        if (identity->module != d->root_module) continue;
        if (named(fn, "answer")) {
            OBSERVE(roles->answer == UINT32_MAX && !identity->exported && !identity->test_role);
            OBSERVE(!fn->parameter_count && fn->result == XR_XIR_I64); roles->answer = f;
        } else if (named(fn, "exported")) {
            OBSERVE(roles->exported == UINT32_MAX && !identity->exported && !identity->test_role);
            OBSERVE(integer_return(fn, 42)); roles->exported = f;
        } else if (named(fn, "checkAnswer") || named(fn, "checkAtomicAnswer")) {
            uint32_t *slot = named(fn, "checkAnswer") ? &roles->test : &roles->atomic_test;
            OBSERVE(*slot == UINT32_MAX && !identity->exported);
            OBSERVE(identity->test_role == XR_XIR_TEST_ROLE_TEST && !identity->test_timeout_seconds);
            OBSERVE(!fn->parameter_count && fn->result == XR_XIR_UNIT); *slot = f;
        }
    }
    OBSERVE(roles->answer != UINT32_MAX && roles->exported != UINT32_MAX && roles->test != UINT32_MAX);
    OBSERVE((roles->atomic_test != UINT32_MAX) == run->fixture->atomic_test);
    OBSERVE(roles->entry < module->function_count && roles->initializer < module->function_count);
    uint32_t named_roles[] = {roles->answer, roles->exported, roles->test, roles->atomic_test};
    OBSERVE(roles->entry != roles->initializer);
    for (size_t i = 0; i < sizeof(named_roles) / sizeof(named_roles[0]); ++i) {
        if (named_roles[i] == UINT32_MAX) continue;
        OBSERVE(roles->entry != named_roles[i] && roles->initializer != named_roles[i]);
        for (size_t j = i + 1; j < sizeof(named_roles) / sizeof(named_roles[0]); ++j)
            OBSERVE(named_roles[j] == UINT32_MAX || named_roles[i] != named_roles[j]);
    }
    OBSERVE(integer_return(&module->functions[roles->entry], 0));
    OBSERVE(module->functions[roles->initializer].result == XR_XIR_UNIT);
    OBSERVE(!d->functions[roles->initializer].exported && !d->functions[roles->initializer].test_role);
    printf("atomic-families-native metadata stage=%u root=%u entry=%u initializer=%u answer=%u "
        "exported=%u original-test=%u atomic-test=%u expected-entry=0 expected-answer=42 "
        "expected-exported=42 expected-test=Unit\n", (unsigned)stage, d->root_module,
        roles->entry, roles->initializer, roles->answer, roles->exported, roles->test, roles->atomic_test);
    return true;
}

static bool inspect_product(AtomicRun *run, const char *file) {
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
    OBSERVE(tests->count == (run->fixture->atomic_test ? 2u : 1u) && tests->entries);
    for (uint32_t t = 0; t < tests->count; ++t) {
        const char *name = t ? "checkAtomicAnswer" : "checkAnswer";
        uint32_t expected = t ? run->roles.atomic_test : run->roles.test;
        OBSERVE(tests->entries[t].function == expected && tests->entries[t].role == XR_XIR_TEST_ROLE_TEST);
        OBSERVE(!tests->entries[t].timeout_seconds && tests->entries[t].name_length == strlen(name));
        OBSERVE(!memcmp(tests->entries[t].name, name, strlen(name)));
    }
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

static bool produce(AtomicRun *run, const char *root, const char *file) {
    XrOsIoPolicy policy = xr_compile_io_policy(run->context.resources);
    run->operation = "original-input";
    XrOsIoStatus io = xr_os_io_read_regular_file(&policy, file, run->fixture->length,
        &run->input, &run->input_length);
    if (io != XR_OS_IO_OK) fprintf(stderr, "atomic-families-native input io-status=%u\n", (unsigned)io);
    OBSERVE(io == XR_OS_IO_OK && run->input_length == run->fixture->length);
    OBSERVE(!memcmp(run->input, run->fixture->source, run->input_length));
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

static bool read_packets(AtomicRun *run, const char *file) {
    XrXirSourceProductPacketView source = {0}, closed = {0};
    AtomicRoles source_roles = {0};
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

static bool detach_lower(AtomicRun *run) {
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
    AtomicRoles reread_roles = {0};
    bool valid = run->status == XR_XIR_OK &&
        inspect_module(run, reread, XR_XIR_CHECKED, &reread_roles);
    if (valid) valid = run->roles.entry == reread_roles.entry &&
        run->roles.initializer == reread_roles.initializer && run->roles.answer == reread_roles.answer &&
        run->roles.exported == reread_roles.exported && run->roles.test == reread_roles.test &&
        run->roles.atomic_test == reread_roles.atomic_test;
    xr_xir_compile_artifact_free(reread);
    OBSERVE(run->status == XR_XIR_OK && valid);
    run->operation = "Lowered";
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run->status = xr_xir_compile_lower(run->closed_checked, &target, &run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    AtomicRoles lowered_roles = {0};
    if (!inspect_module(run, run->lowered, XR_XIR_LOWERED, &lowered_roles)) return false;
    OBSERVE(run->roles.entry == lowered_roles.entry && run->roles.initializer == lowered_roles.initializer);
    OBSERVE(run->roles.answer == lowered_roles.answer && run->roles.exported == lowered_roles.exported &&
        run->roles.test == lowered_roles.test && run->roles.atomic_test == lowered_roles.atomic_test);
    run->status = xr_xir_compile_artifact_verify(run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&run->retained);
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    xr_xir_compile_artifact_free(run->closed_checked); run->closed_checked = NULL;
    return phase(run, "detached-Lowered-verified");
}

static bool finish(AtomicRun *run, XrXirInstance *instance, XrXirType type, int64_t payload) {
    XrXirInstanceResult result = {0};
    size_t polls = 0;
    do {
        OBSERVE(++polls <= 4096);
        result = xr_xir_instance_poll_bounded(instance, UINT64_C(1000000));
    } while (result.outcome.status == XR_XIR_CALL_READY);
    run->call_status = result.outcome.status;
    OBSERVE(run->call_status == XR_XIR_CALL_RETURNED);
    run->call_status = xr_xir_instance_take_result(instance, &run->value);
    OBSERVE(run->call_status == XR_XIR_CALL_RETURNED);
    bool same = run->value.type == (uint32_t)type && !run->value.reserved && (int64_t)run->value.payload == payload;
    xr_xir_value_drop(&run->value);
    OBSERVE(same && !run->value.type && !run->value.reserved && !run->value.payload);
    return true;
}

static bool execute_instances(AtomicRun *run) {
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = run->instances[i];
        run->operation = "private-entry-authority";
        size_t attempts = runtime_attempts, compiler_attempts = instance_compile_attempts;
        size_t blocks = runtime_live, bytes = runtime_bytes;
        XrCompileResourceStats before = {0}, after = {0};
        OBSERVE(xr_compile_resources_stats(run->context.resources, &before) == XR_COMPILE_RESOURCE_OK);
        OBSERVE(xr_xir_instance_start(instance, run->roles.initializer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_xir_instance_start(instance, run->roles.answer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_xir_instance_start(instance, run->roles.exported, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_xir_instance_start(instance, run->roles.test, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        if (run->fixture->atomic_test)
            OBSERVE(xr_xir_instance_start(instance, run->roles.atomic_test, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_xir_instance_start_test(instance, run->roles.answer) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_xir_instance_start_test(instance, run->roles.exported) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_xir_instance_start_test(instance, run->roles.initializer) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_compile_resources_stats(run->context.resources, &after) == XR_COMPILE_RESOURCE_OK);
        OBSERVE(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes);
        OBSERVE(before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes && before.work == after.work);
        OBSERVE(instance_compile_attempts == compiler_attempts && runtime_attempts == attempts);
        OBSERVE(runtime_live == blocks && runtime_bytes == bytes && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_NEW);
        printf("atomic-families-native instance=%u permission-denied=8 unchanged=1\n", i);
        run->operation = "canonical-entry";
        run->call_status = xr_xir_instance_start(instance, run->roles.entry, NULL, 0);
        OBSERVE(run->call_status == XR_XIR_CALL_READY);
        if (!finish(run, instance, XR_XIR_I64, 0)) return false;
        printf("atomic-families-native instance=%u canonical=I64(0)\n", i);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            run->operation = "original-checkAnswer";
            run->call_status = xr_xir_instance_start_test(instance, run->roles.test);
            OBSERVE(run->call_status == XR_XIR_CALL_READY);
            if (!finish(run, instance, XR_XIR_UNIT, 0)) return false;
            printf("atomic-families-native instance=%u repeat=%u original-checkAnswer=Unit exported-fixed-oracle=42\n",
                i, repeat);
            if (run->fixture->atomic_test) {
                run->operation = "same-module-checkAtomicAnswer";
                run->call_status = xr_xir_instance_start_test(instance, run->roles.atomic_test);
                OBSERVE(run->call_status == XR_XIR_CALL_READY);
                if (!finish(run, instance, XR_XIR_UNIT, 0)) return false;
                printf("atomic-families-native instance=%u repeat=%u same-module-checkAtomicAnswer=Unit answer-fixed-oracle=42\n",
                    i, repeat);
            }
        }
        OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    }
    return true;
}

static bool release(AtomicRun *run) {
    bool complete = true;
    xr_xir_value_drop(&run->value);
    xr_xir_compile_c_source_free(&run->repeated);
    xr_xir_compile_c_source_free(&run->emitted);
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
    if (instance_compile_live || instance_compile_bytes || runtime_live || runtime_bytes ||
        runtime_owned || runtime_owned_capacity) complete = false;
    printf("atomic-families-native release compiler=%zu/%zu runtime=%zu/%zu table=%zu result=%s\n",
        instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes,
        runtime_owned_capacity, complete ? "PASS" : "FAIL");
    return complete;
}

#if defined(XR_SOURCE_ATOMIC_FAMILIES_NATIVE)
extern const XrXirProgramSpec source_atomic_i64_family_program;
extern const XrXirProgramSpec source_atomic_bool_family_program;
extern const XrXirProgramSpec source_atomic_f64_family_program;
extern const char source_atomic_i64_family_c_sha[65];
extern const char source_atomic_bool_family_c_sha[65];
extern const char source_atomic_f64_family_c_sha[65];
static const XrXirProgramSpec *native_spec(const AtomicFixture *fixture) {
    if (!strcmp(fixture->scenario, "1")) return &source_atomic_i64_family_program;
    if (!strcmp(fixture->scenario, "2")) return &source_atomic_bool_family_program;
    if (!strcmp(fixture->scenario, "3")) return &source_atomic_f64_family_program;
    return NULL;
}
static const char *native_sha(const AtomicFixture *fixture) {
    if (!strcmp(fixture->scenario, "1")) return source_atomic_i64_family_c_sha;
    if (!strcmp(fixture->scenario, "2")) return source_atomic_bool_family_c_sha;
    if (!strcmp(fixture->scenario, "3")) return source_atomic_f64_family_c_sha;
    return NULL;
}
#else
static const XrXirProgramSpec *native_spec(const AtomicFixture *fixture) { (void)fixture; return NULL; }
static const char *native_sha(const AtomicFixture *fixture) { (void)fixture; return NULL; }
#endif

static void hexadecimal(const uint8_t bytes[32], char output[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        output[i * 2] = digits[bytes[i] >> 4];
        output[i * 2 + 1] = digits[bytes[i] & 15u];
    }
    output[64] = '\0';
}

static bool emit_owned(AtomicRun *run) {
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
    printf("atomic-families-native owned-C count=2 bytes=%zu identical=1 sha256=%s\n",
        run->emitted.length, run->c_digest);
    xr_xir_compile_c_source_free(&run->repeated);
    return phase(run, "emission-repeat-freed");
}

static bool write_translation_unit(AtomicRun *run, const char *output) {
    run->operation = "write-complete-native-TU";
    FILE *file = fopen(output, "wb");
    OBSERVE(file != NULL);
    bool written = fwrite(run->emitted.text, 1, run->emitted.length, file) == run->emitted.length;
    if (written) written = fprintf(file, "\nconst char %s_c_sha[65]=\"%s\";\n", run->fixture->symbol, run->c_digest) > 0;
    int closed = fclose(file);
    OBSERVE(written && closed == 0);
    printf("atomic-families-native emitted-TU payload-sha=%s output=%s callback-ProgramSpec=1 extern-exports=OPEN\n",
        run->c_digest, output);
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

static bool generated_correspondence(AtomicRun *run, const XrXirProgramSpec *spec) {
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
        uint32_t parameters = module->functions[f].parameter_count;
        OBSERVE(!parameters || (spec->entries[f].parameters && module->functions[f].parameters &&
            !memcmp(spec->entries[f].parameters, module->functions[f].parameters,
                (size_t)parameters * sizeof(XrXirType))));
    }
    printf("atomic-families-native compiled-correspondence functions=%u all-native=1 proof-bytes=%zu layouts-exact=1 permissions-exact=1\n",
        spec->entry_count, proof.length);
    return true;
}

static bool seal_native_instances(AtomicRun *run) {
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

_Static_assert(sizeof(test_i64) - 1u == 361u, "Exact complete i64 Atomic source");
_Static_assert(sizeof(test_bool) - 1u == 401u, "Exact complete legal bool Atomic source");
_Static_assert(sizeof(test_f64) - 1u == 539u, "Exact complete f64 Atomic source");

int main(int argc, char **argv) {
    const char *output = NULL;
    if (argc == 7 && !strcmp(argv[5], "--emit")) output = argv[6];
    else if (argc != 5) return 2;
    const AtomicFixture *fixture = select_fixture(argv[1], argv[2]);
    if (!fixture || (!output && !native_spec(fixture))) return 2;
    instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    AtomicRun run = {0}; run.fixture = fixture;
    run.context.limits = xr_xir_compile_default_limits();
    run.operation = "finite-owner";
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrCompileResourceStatus owner = xr_compile_resources_new(&limits, &run.context.resources);
    bool passed = owner == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = xr_compile_resources_stats(run.context.resources, &run.baseline) == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = phase(&run, "owner-created") && produce(&run, argv[3], argv[4]);
    if (passed) passed = read_packets(&run, argv[4]) && detach_lower(&run) && emit_owned(&run);
    if (passed) passed = output ? write_translation_unit(&run, output) : seal_native_instances(&run) && execute_instances(&run);
    if (!passed) fprintf(stderr, "atomic-families-native failure operation=%s owner-status=%u status=%u "
        "source-stage=%u source-status=%u module=%u line=%d column=%d xir-status=%u "
        "function=%u block=%u instruction=%u reason=%u call-status=%u message=%s\n", run.operation,
        (unsigned)owner, (unsigned)run.status, (unsigned)run.diagnostic.stage,
        (unsigned)run.diagnostic.source.status, run.diagnostic.source.module, run.diagnostic.source.line,
        run.diagnostic.source.column, (unsigned)run.xir.status, run.xir.function, run.xir.block,
        run.xir.instruction, (unsigned)run.xir.reason, (unsigned)run.call_status, run.diagnostic.source.message);
    bool released = release(&run);
    printf("atomic-families-native scenario=%s mode=%s normal=%s compiler-sites=%zu runtime-sites=%zu allocated=%" PRIu64
        " peak=%" PRIu64 " work=%" PRIu64 " callback-native=%u foreign-exports=OPEN FI=NOT_RUN axes=NOT_RUN "
        "source-physical-removal=OPEN image-unload=NOT_APPLICABLE\n",
        fixture->scenario, output ? "emit" : "native", passed && released ? "PASS" : "FAIL", instance_compile_attempts,
        runtime_attempts, run.final.allocated_bytes, run.final.peak_bytes, run.final.work, output ? 0u : 1u);
    return passed && released ? 0 : 1;
}
