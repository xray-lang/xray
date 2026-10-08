/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_allocation_original.c - Original allocation family admission
 *
 * KEY CONCEPT:
 *   Unmodified positive inputs retain their original test and private function
 *   identities through detached Checked owners and independent VM instances.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include "xir/xxir_vm.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "allocation_original_sources.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Execution identity");

typedef struct OriginalRun {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
    XrCompilerSession *session;
    XrXirSourceProduct *product;
    XrXirSourceProductDiagnostic diagnostic;
    XrXirDiagnostic xir;
    XrXirArtifact *source, *closed, *reread, *lowered;
    XrXirCheckedPacket packet;
    XrXirProgram *program;
    XrXirInstance *instances[2];
    XrXirValue value;
    uint8_t *input;
    uint32_t entry, initializer, answer, exported, test;
    unsigned scenario;
    const char *stage;
    XrXirStatus status;
} OriginalRun;
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "allocation-original check line=%d stage=%s condition=%s\n", \
    __LINE__, run->stage, #c); return false; } } while (0)

static bool original_named(const XrXirFunction *fn, const char *name) {
    return fn->name_length == strlen(name) && !memcmp(fn->name, name, fn->name_length);
}
static bool original_shape(OriginalRun *run, const XrXirArtifact *artifact, XrXirStage stage, bool bind) {
    const XrXirModule *m = xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *d = m ? m->declarations : NULL;
    REQUIRE(m && m->stage == stage && d && d->root_module < d->module_count);
    uint32_t answer = UINT32_MAX, exported = UINT32_MAX, test = UINT32_MAX;
    for (uint32_t i = 0; i < m->function_count; ++i) {
        const XrXirFunctionIdentity *id = &d->functions[i];
        const XrXirFunction *fn = &m->functions[i];
        if (id->module != d->root_module) continue;
        if (original_named(fn, "answer") || original_named(fn, "exported")) {
            uint32_t *slot = original_named(fn, "answer") ? &answer : &exported;
            REQUIRE(*slot == UINT32_MAX && !id->exported && !id->test_role);
            REQUIRE(!fn->parameter_count && fn->result == XR_XIR_I64); *slot = i;
        } else if (original_named(fn, "checkAnswer")) {
            REQUIRE(test == UINT32_MAX && !id->exported && id->test_role == XR_XIR_TEST_ROLE_TEST);
            REQUIRE(!id->test_timeout_seconds && !fn->parameter_count && fn->result == XR_XIR_UNIT); test = i;
        }
    }
    REQUIRE(answer != UINT32_MAX && exported != UINT32_MAX && test != UINT32_MAX);
    uint32_t entry = d->entry_function, initializer = d->modules[d->root_module].initializer;
    REQUIRE(entry < m->function_count && initializer < m->function_count);
    REQUIRE(entry != initializer && entry != answer && entry != exported && entry != test);
    REQUIRE(m->functions[initializer].result == XR_XIR_UNIT && !d->functions[initializer].exported);
    if (bind) {
        run->entry = entry; run->initializer = initializer; run->answer = answer; run->exported = exported; run->test = test;
    } else REQUIRE(run->entry == entry && run->initializer == initializer && run->answer == answer &&
                   run->exported == exported && run->test == test);
    printf("allocation-original shape scenario=%u stage=%u functions=%u original-test=%u\n",
        run->scenario, (unsigned)stage, m->function_count, test);
    return true;
}
static bool original_produce(OriginalRun *run, const char *root, const char *file) {
    run->stage = "original-input";
    const char *original = allocation_original_sources[run->scenario];
    size_t length = strlen(original), actual = 0;
    XrOsIoPolicy io = xr_compile_io_policy(run->context.resources);
    REQUIRE(xr_os_io_read_regular_file(&io, file, length, &run->input, &actual) == XR_OS_IO_OK);
    REQUIRE(actual == length && !memcmp(run->input, original, length));
    xr_compile_resources_free(run->input); run->input = NULL;
    printf("allocation-original input scenario=%u bytes=%zu exact=1\n", run->scenario, length);
    run->stage = "session";
    REQUIRE(xr_compile_session_new(run->context.resources, &run->session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{run->session, file, &authority, &run->context,
        XR_ALLOCATION_STDLIB, NULL, XR_XIR_PROGRAM, NULL}, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    run->stage = "SourceProduct";
    run->status = xr_xir_compile_source_product_build(&request, &run->product, &run->diagnostic);
    REQUIRE(run->status == XR_XIR_OK && run->product);
    XrXirSourceProductPacketView source = {0}, closed = {0};
    REQUIRE(xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_SOURCE, &source) == XR_XIR_OK);
    REQUIRE(xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_CLOSED, &closed) == XR_XIR_OK);
    run->stage = "source-Checked";
    run->status = xr_xir_compile_checked_read(&run->context, source.bytes, source.length, &run->source, &run->xir);
    REQUIRE(run->status == XR_XIR_OK && original_shape(run, run->source, XR_XIR_CHECKED, true));
    run->stage = "closed-Checked";
    run->status = xr_xir_compile_checked_read(&run->context, closed.bytes, closed.length, &run->closed, &run->xir);
    REQUIRE(run->status == XR_XIR_OK && original_shape(run, run->closed, XR_XIR_CHECKED, true));
    const XrXirSourceTests *tests = xr_xir_compile_source_product_tests(run->product);
    REQUIRE(tests && tests->count == 1 && tests->entries && tests->entries[0].function == run->test);
    REQUIRE(tests->entries[0].name_length == 11 && !memcmp(tests->entries[0].name, "checkAnswer", 11));
    REQUIRE(tests->entries[0].role == XR_XIR_TEST_ROLE_TEST && !tests->entries[0].timeout_seconds);
    run->stage = "retained-Checked";
    run->status = xr_xir_compile_checked_write(run->closed, &run->packet, &run->xir);
    REQUIRE(run->status == XR_XIR_OK && run->packet.length == closed.length && run->packet.bytes != closed.bytes);
    REQUIRE(!memcmp(run->packet.bytes, closed.bytes, closed.length));
    xr_compile_session_free(run->session); run->session = NULL;
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    run->stage = "detached-Checked";
    REQUIRE(xr_xir_compile_artifact_verify(run->source, &run->xir) == XR_XIR_OK);
    REQUIRE(xr_xir_compile_artifact_verify(run->closed, &run->xir) == XR_XIR_OK);
    run->status = xr_xir_compile_checked_read(&run->context, run->packet.bytes, run->packet.length, &run->reread, &run->xir);
    REQUIRE(run->status == XR_XIR_OK && original_shape(run, run->reread, XR_XIR_CHECKED, false));
    REQUIRE(xr_xir_compile_artifact_verify(run->reread, &run->xir) == XR_XIR_OK);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run->stage = "Lowered";
    run->status = xr_xir_compile_lower(run->closed, &target, &run->lowered, &run->xir);
    REQUIRE(run->status == XR_XIR_OK && original_shape(run, run->lowered, XR_XIR_LOWERED, false));
    REQUIRE(xr_xir_compile_artifact_verify(run->lowered, &run->xir) == XR_XIR_OK);
    xr_xir_compile_artifact_free(run->source); run->source = NULL;
    xr_xir_compile_artifact_free(run->closed); run->closed = NULL;
    xr_xir_compile_artifact_free(run->reread); run->reread = NULL;
    xr_xir_compile_checked_packet_free(&run->packet);
    run->stage = "VM-Program";
    run->status = xr_xir_compile_vm_program_take(&run->lowered, &run->program);
    REQUIRE(run->status == XR_XIR_OK && !run->lowered && run->program);
    return true;
}
static bool original_finish(OriginalRun *run, XrXirInstance *instance, XrXirType type) {
    XrXirInstanceResult result = {0};
    size_t polls = 0;
    do {
        REQUIRE(++polls <= 4096);
        result = xr_xir_instance_poll_bounded(instance, UINT64_C(1000000));
        REQUIRE(xr_xir_call_result_valid(&result.outcome));
    } while (result.outcome.status == XR_XIR_CALL_READY);
    REQUIRE(result.outcome.status == XR_XIR_CALL_RETURNED);
    REQUIRE(xr_xir_instance_take_result(instance, &run->value) == XR_XIR_CALL_RETURNED);
    REQUIRE(run->value.type == (uint32_t)type && !run->value.reserved && !run->value.payload);
    xr_xir_value_drop(&run->value);
    REQUIRE(!run->value.type && !run->value.reserved && !run->value.payload);
    return true;
}
static bool original_execute(OriginalRun *run) {
    run->stage = "instances";
    XrXirInstanceConfig config = {0};
    REQUIRE(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i)
        REQUIRE(xr_xir_instance_new(run->program, &config, &run->instances[i]) == XR_XIR_CALL_READY);
    REQUIRE(run->instances[0] && run->instances[1] && run->instances[0] != run->instances[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = run->instances[i];
        run->stage = "private-authority";
        size_t compiler = instance_compile_attempts, runtime = runtime_attempts;
        REQUIRE(xr_xir_instance_start(instance, run->answer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        REQUIRE(xr_xir_instance_start(instance, run->exported, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        REQUIRE(xr_xir_instance_start(instance, run->initializer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        REQUIRE(xr_xir_instance_start(instance, run->test, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        REQUIRE(xr_xir_instance_start_test(instance, run->answer) == XR_XIR_CALL_BAD_ARGUMENT);
        REQUIRE(xr_xir_instance_start_test(instance, run->exported) == XR_XIR_CALL_BAD_ARGUMENT);
        REQUIRE(xr_xir_instance_start_test(instance, run->initializer) == XR_XIR_CALL_BAD_ARGUMENT);
        REQUIRE(instance_compile_attempts == compiler && runtime_attempts == runtime);
        REQUIRE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_NEW);
        run->stage = "canonical-entry";
        REQUIRE(xr_xir_instance_start(instance, run->entry, NULL, 0) == XR_XIR_CALL_READY);
        if (!original_finish(run, instance, XR_XIR_I64)) return false;
        run->stage = "original-checkAnswer";
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            REQUIRE(xr_xir_instance_start_test(instance, run->test) == XR_XIR_CALL_READY);
            if (!original_finish(run, instance, XR_XIR_UNIT)) return false;
            printf("allocation-original result scenario=%u instance=%u repeat=%u checkAnswer=Unit exported=42\n",
                run->scenario, i, repeat);
        }
        REQUIRE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    }
    return true;
}
static bool original_release(OriginalRun *run) {
    bool ok = true;
    xr_xir_value_drop(&run->value);
    for (unsigned i = 0; i < 2; ++i)
        if (run->instances[i] && xr_xir_instance_free(run->instances[i]) != XR_XIR_CALL_READY) ok = false;
    xr_xir_compile_program_drop(run->program);
    xr_xir_compile_artifact_free(run->lowered); xr_xir_compile_artifact_free(run->reread);
    xr_xir_compile_artifact_free(run->source); xr_xir_compile_artifact_free(run->closed);
    xr_xir_compile_checked_packet_free(&run->packet);
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    xr_xir_compile_source_product_free(run->product); xr_compile_session_free(run->session);
    xr_compile_resources_free(run->input);
    if (run->context.resources) {
        XrCompileResourceStats final = {0};
        if (xr_compile_resources_stats(run->context.resources, &final) != XR_COMPILE_RESOURCE_OK ||
            final.live_bytes != run->baseline.live_bytes || final.live_bytes != instance_compile_bytes) ok = false;
        printf("allocation-original ledger allocated=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 "\n",
            final.allocated_bytes, final.peak_bytes, final.work);
        xr_compile_resources_release(run->context.resources);
    }
    if (instance_compile_live || instance_compile_bytes || runtime_live || runtime_bytes || runtime_owned || runtime_owned_capacity) ok = false;
    printf("allocation-original release compiler=%zu/%zu runtime=%zu/%zu table=%zu result=%s\n",
        instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes, runtime_owned_capacity, ok ? "PASS" : "FAIL");
    return ok;
}
int main(int argc, char **argv) {
    if (argc != 4 || strlen(argv[3]) != 1 || argv[3][0] < '0' || argv[3][0] > '7') return 2;
    OriginalRun run = {0}; run.scenario = (unsigned)(argv[3][0] - '0'); run.stage = "finite-owner";
    run.context.limits = xr_xir_compile_default_limits();
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    bool ok = xr_compile_resources_new(&limits, &run.context.resources) == XR_COMPILE_RESOURCE_OK;
    if (ok) ok = xr_compile_resources_stats(run.context.resources, &run.baseline) == XR_COMPILE_RESOURCE_OK;
    if (ok) ok = original_produce(&run, argv[1], argv[2]) && original_execute(&run);
    if (!ok) fprintf(stderr, "allocation-original failure scenario=%u stage=%s status=%u source-stage=%u "
        "source-status=%u line=%d column=%d xir-status=%u reason=%u source-path=%s product-xir=%u "
        "function=%u block=%u instruction=%u product-reason=%u message=%s\n", run.scenario, run.stage,
        (unsigned)run.status, (unsigned)run.diagnostic.stage, (unsigned)run.diagnostic.source.status,
        run.diagnostic.source.line, run.diagnostic.source.column, (unsigned)run.xir.status,
        (unsigned)run.xir.reason, run.diagnostic.source_path ? run.diagnostic.source_path : "NONE",
        (unsigned)run.diagnostic.xir.status, run.diagnostic.xir.function, run.diagnostic.xir.block,
        run.diagnostic.xir.instruction, (unsigned)run.diagnostic.xir.reason, run.diagnostic.source.message);
    bool released = original_release(&run);
    printf("allocation-original normal=%s scenario=%u compiler-sites=%zu runtime-sites=%zu "
        "external-manifest=NOT_APPLIED answer-execution=NOT_RUN FI=NOT_RUN native=NOT_RUN mixed=NOT_RUN\n",
        ok && released ? "PASS" : "FAIL", run.scenario, instance_compile_attempts, runtime_attempts);
    return ok && released ? 0 : 1;
}
