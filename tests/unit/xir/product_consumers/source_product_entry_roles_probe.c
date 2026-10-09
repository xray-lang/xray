/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * probe.c - Reviewed source admission and detached public entry roles
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked inputs");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Current public runtime inputs");

typedef struct EntryRoles {
    uint32_t functions[7], imported, initializer, plain, entry, answer;
    XrXirType entry_result;
    bool tests;
} EntryRoles;

static void diagnostic_report(const char *name, XrXirStatus status,
                              const XrXirSourceProductDiagnostic *diagnostic) {
    printf("source-admission case=%s status=%u stage=%u source-status=%u module=%u line=%d column=%d "
        "xir-status=%u function=%u block=%u instruction=%u reason=%u message=%s\n", name, status,
        diagnostic->stage, diagnostic->source.status, diagnostic->source.module,
        diagnostic->source.line, diagnostic->source.column, diagnostic->xir.status,
        diagnostic->xir.function, diagnostic->xir.block, diagnostic->xir.instruction,
        diagnostic->xir.reason, diagnostic->source.message);
    if (diagnostic->source_path)
        printf("source-path %s\n", diagnostic->source_path);
}

static bool named(const XrXirFunction *function, const char *name) {
    return function->name_length == strlen(name) && !memcmp(function->name, name, function->name_length);
}

static void atomic_controls(const char *name, const XrXirModule *module) {
    if (!strstr(name, "atomic_ordering_") && !strstr(name, "legal_rmw_") && !strstr(name, "legal_original_")) return;
    static const char *const orders[] = {"Relaxed", "Acquire", "Release", "AcquireRelease", "SeqCst"};
    const char *suffix = strrchr(name, '_');
    CHECK(suffix);
    uint32_t expected = UINT32_MAX;
    for (uint32_t i = 0; i < 5; ++i) if (!strcmp(suffix + 1, orders[i])) expected = i;
    CHECK(expected < 5);
    uint32_t counts[3] = {0};
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        if (!named(fn, "answer")) continue;
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            uint32_t slot;
            if (op->op == XR_XIR_ATOMIC_STORE) slot = 0;
            else if (op->op == XR_XIR_ATOMIC_FETCH_SUB) slot = 1;
            else if (op->op == XR_XIR_ATOMIC_LOAD) slot = 2;
            else continue;
            CHECK(op->args[1] == (slot == 2 ? 2u : 3u));
            CHECK(op->args[0] <= fn->operand_count && op->args[1] <= fn->operand_count - op->args[0]);
            uint32_t order = fn->operands[op->args[0] + op->args[1] - 1];
            CHECK(order < fn->instruction_count && fn->instructions[order].op == XR_XIR_NULLABLE_SOME);
            order = fn->instructions[order].args[0];
            CHECK(order < fn->instruction_count && fn->instructions[order].op == XR_XIR_ENUM_NEW);
            CHECK(fn->instructions[order].immediate >= 0 && fn->instructions[order].immediate < 5);
            uint32_t tag = (uint32_t)fn->instructions[order].immediate;
            uint32_t wanted = !strncmp(name, "legal_rmw_", 10) && slot != 1 ? 4u : expected;
            CHECK(tag == wanted && !counts[slot]++);
            printf("atomic-control case=%s operation=%s ordering=%s tag=%u typed=Nullable<Ordering>\n",
                name, xr_xir_op_name(op->op), orders[tag], tag);
        }
    }
    CHECK(counts[0] == 1 && counts[1] == 1 && counts[2] == 1);
}

static EntryRoles inspect_roles(const char *name, const XrXirModule *module, const XrXirSourceTests *tests) {
    EntryRoles result = {{0}, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0, false};
    const XrXirDeclarations *d = module->declarations;
    CHECK(d && tests);
    result.entry = d->entry_function;
    CHECK(result.entry < module->function_count);
    result.entry_result = module->functions[result.entry].result;
    result.initializer = d->modules[d->root_module].initializer;
    result.tests = !strcmp(name, "legal_test_discovery") || !strcmp(name, "original_test_discovery");
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *identity = &d->functions[f];
        if (named(fn, "consumerAnswer")) result.answer = f;
        if (named(fn, "answer")) result.plain = f;
        if (identity->test_role && identity->module != d->root_module) result.imported = f;
        printf("checked-function index=%u module=%u role=%u timeout=%u result=%u name=%.*s\n",
            f, identity->module, identity->test_role, identity->test_timeout_seconds,
            fn->result, (int)fn->name_length, fn->name);
        for (uint32_t i = 0; i < fn->instruction_count; ++i) {
            const XrXirInstruction *op = &fn->instructions[i];
            printf("checked-operation function=%u index=%u operation=%u name=%s type=%u args=%u,%u immediate=%lld\n",
                f, i, op->op, xr_xir_op_name(op->op), op->type, op->args[0], op->args[1], (long long)op->immediate);
        }
    }
    if (!result.tests) return result;
    static const char *const names[] = {"first", "second", "skipped", "beforeEach", "afterEach", "beforeAll", "afterAll"};
    const uint32_t roles[] = {XR_XIR_TEST_ROLE_TEST, XR_XIR_TEST_ROLE_TEST, XR_XIR_TEST_ROLE_SKIP,
        XR_XIR_TEST_ROLE_BEFORE_EACH, XR_XIR_TEST_ROLE_AFTER_EACH, XR_XIR_TEST_ROLE_BEFORE_ALL, XR_XIR_TEST_ROLE_AFTER_ALL};
    CHECK(tests->count == 7 && result.imported != UINT32_MAX && result.plain != UINT32_MAX);
    for (uint32_t i = 0; i < 7; ++i) {
        const XrXirSourceTestEntry *test = &tests->entries[i];
        CHECK(test->role == roles[i] && !strcmp(test->name, names[i]));
        CHECK(test->name_length == strlen(names[i]) && test->timeout_seconds == (i ? 0u : 2u));
        CHECK(test->function < module->function_count && test->function != result.imported);
        CHECK(d->functions[test->function].module == d->root_module && module->functions[test->function].result == XR_XIR_UNIT);
        CHECK(d->functions[test->function].test_role == roles[i]);
        for (uint32_t prior = 0; prior < i; ++prior) CHECK(test->function != result.functions[prior]);
        result.functions[i] = test->function;
        printf("source-test index=%u function=%u role=%u timeout=%u name=%s\n",
            i, test->function, test->role, test->timeout_seconds, test->name);
    }
    return result;
}

static XrXirValue finish(XrXirInstance *instance) {
    XrXirInstanceResult result = {0};
    size_t polls = 0;
    do {
        CHECK(++polls < 4096);
        result = xr_xir_instance_poll_bounded(instance, UINT64_C(1000000));
    } while (result.outcome.status == XR_XIR_CALL_READY);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}

static void test_entries(XrXirInstance *instance, const EntryRoles *roles, unsigned index) {
    size_t attempts = runtime_attempts;
    CHECK(xr_xir_instance_start_test(instance, roles->imported) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_test(instance, roles->initializer) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_test(instance, roles->plain) == XR_XIR_CALL_BAD_ARGUMENT);
    for (uint32_t i = 0; i < 7; ++i)
        CHECK(xr_xir_instance_start(instance, roles->functions[i], NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start_test(instance, roles->functions[2]) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(runtime_attempts == attempts && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_NEW);
    const uint32_t sequence[] = {5, 3, 0, 4, 3, 1, 4, 6};
    for (uint32_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        uint32_t role = sequence[i];
        CHECK(xr_xir_instance_start_test(instance, roles->functions[role]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start_test(instance, roles->functions[role]) == XR_XIR_CALL_BUSY);
        XrXirValue value = finish(instance);
        CHECK(value.type == XR_XIR_UNIT && !value.reserved && !value.payload);
        xr_xir_value_drop(&value);
        printf("test-execution instance=%u role-index=%u result=Unit\n", index, role);
    }
    printf("test-isolation instance=%u first-bump=41 second-read=41 dependency-denied=1 skip-denied=1\n", index);
}

static void runtime_probe(const char *name, XrXirProgram **program, const EntryRoles *roles) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instances[2] = {0};
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_instance_new(*program, &config, &instances[i]) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(*program); *program = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        if (roles->tests) {
            test_entries(instances[i], roles, i);
        } else {
            CHECK(roles->answer != UINT32_MAX && roles->plain != UINT32_MAX);
            size_t attempts = runtime_attempts;
            CHECK(xr_xir_instance_start(instances[i], roles->plain, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(runtime_attempts == attempts);
            CHECK(xr_xir_instance_start(instances[i], roles->entry, NULL, 0) == XR_XIR_CALL_READY);
            XrXirValue initialized = finish(instances[i]);
            CHECK((XrXirType)initialized.type == roles->entry_result && !initialized.payload);
            xr_xir_value_drop(&initialized);
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                CHECK(xr_xir_instance_start(instances[i], roles->answer, NULL, 0) == XR_XIR_CALL_READY);
                XrXirValue value = finish(instances[i]);
                CHECK(value.type == XR_XIR_I64 && (int64_t)value.payload == 82);
                xr_xir_value_drop(&value);
                printf("atomic-execution case=%s instance=%u repeat=%u value=82\n", name, i, repeat);
            }
        }
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(!runtime_live && !runtime_bytes);
    printf("runtime physical blocks/bytes=0/0; attempts=%zu\n", runtime_attempts);
}

int main(int argc, char **argv) {
    CHECK(argc == 5);
    instance_compile_zero();
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(16777216), UINT64_C(128000000)};
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    XrCompileResourceStats baseline = {0}, stats = {0};
    XrCompilerSession *session = NULL;
    XrXirSourceProduct *product = NULL;
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirProgram *program = NULL;
    XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirCSource emitted = {0}, regenerated = {0};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(context.resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, argv[2]};
    XrXirSourceProductRequest request = {{session, argv[3], &authority, &context, NULL, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    int passed = 0;
    if (status != XR_XIR_OK) {
        CHECK(!product); diagnostic_report(argv[1], status, &diagnostic); goto release;
    }
    XrXirSourceProductFacts facts = *xr_xir_compile_source_product_facts(product);
    printf("source-admission case=%s status=0 functions=%u modules=%u entry=%u\n", argv[1],
        facts.function_count, facts.module_count, facts.entry);
    XrXirSourceProductPacketView packet = {0};
    CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    xr_compile_session_free(session); session = NULL;
    EntryRoles roles = inspect_roles(argv[1], xr_xir_compile_artifact_module(checked), xr_xir_compile_source_product_tests(product));
    atomic_controls(argv[1], xr_xir_compile_artifact_module(checked));
    status = xr_xir_compile_source_product_emit(product, "entry_roles", UINT64_C(16777216), &emitted);
    if (status != XR_XIR_OK) { printf("emission case=%s status=%u\n", argv[1], status); goto release; }
    CHECK(xr_xir_compile_source_product_vm_take(product, &program) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product); product = NULL;
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(checked, &target, &lowered, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_emit_c(lowered, "entry_roles", UINT64_C(16777216), &regenerated) == XR_XIR_OK);
    CHECK(emitted.length == regenerated.length && !memcmp(emitted.text, regenerated.text, emitted.length));
    FILE *output = fopen(argv[4], "wb");
    CHECK(output && fwrite(regenerated.text, 1, regenerated.length, output) == regenerated.length);
    CHECK(!fclose(output));
    if (roles.tests || !strncmp(argv[1], "legal_", 6)) runtime_probe(argv[1], &program, &roles);
    passed = 1;
    printf("projection case=%s detached-Checked-Lowered=PASS C-identical=PASS\n", argv[1]);
release:
    xr_xir_compile_program_drop(program);
    xr_xir_compile_c_source_free(&emitted); xr_xir_compile_c_source_free(&regenerated);
    xr_xir_compile_artifact_free(lowered); xr_xir_compile_artifact_free(checked);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_xir_compile_source_product_free(product); xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    instance_compile_zero(); instance_compile_report();
    CHECK(!runtime_live && !runtime_bytes);
    printf("final runtime physical blocks/bytes=0/0; attempts=%zu\n", runtime_attempts);
    return passed ? 0 : 1;
}
