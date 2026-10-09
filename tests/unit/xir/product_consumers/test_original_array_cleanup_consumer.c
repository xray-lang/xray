/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_original_array_cleanup_consumer.c - Original Array cleanup through detached owners
 *
 * KEY CONCEPT:
 *   A legacy panic mismatch remains a failure after independently observing
 *   cleanup, recovery and physical release. Source bytes live in external files.
 */
#include <stdio.h>
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include "base/xsha256.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_panic.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"

#include "original_array_cleanup_native.inc.c"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Current public execution contracts");

typedef struct ArrayFixture {
    const char *name, *prefix_sha, *full_sha;
    size_t prefix_bytes, full_bytes;
} ArrayFixture;
static const ArrayFixture fixtures[] = {
    {"array_default_defer", "23ce98a8b829911a5e9b22792cdaeb3024feb8f111b651754f9438ade67fca02",
        "c89ac6f77080ee003f5d2984da70a96c2938d6cb60766b63ef6d1bbb0393dba5", 506, 637},
    {"array_invoke_defer", "7c66fc13a6469337bb63c9a56a58bec6ba90ccc7b29b19cef7e396ca9d3f2714",
        "12d1ee050a55ed2542b5f2a9b8f248bf28d6cf25049921def058f6b4f9ccf6cb", 900, 1031}
};
typedef struct ArrayRoles { uint32_t entry, checked, cleanup; } ArrayRoles;
typedef struct FailureSnapshot {
    XrXirCallStatus status;
    XrXirFaultDetail detail;
    uint32_t message_type;
    char text[XR_XIR_PANIC_MESSAGE_CAPACITY];
    size_t length;
} FailureSnapshot;
typedef struct ArrayRun {
    const ArrayFixture *fixture;
    XrXirCompileContext context;
    XrCompileResourceStats baseline, final;
    XrCompilerSession *session;
    XrXirSourceProduct *product;
    XrXirSourceProductDiagnostic diagnostic;
    XrXirDiagnostic xir;
    XrXirArtifact *checked, *lowered;
    XrXirCheckedPacket retained;
    XrXirProgram *program;
    XrXirInstance *instances[2];
    XrXirCallResult escaped[4];
    FailureSnapshot snapshots[4];
    XrXirValue value;
    ArrayRoles roles;
    XrModuleIdentityAuthority authority;
    XrXirSourceProductRequest request;
    char root[2048], file[2048], stdlib[2048];
    uint8_t *input;
    size_t input_length;
    unsigned escaped_count, returns, legacy_comparisons;
    bool legacy_pass;
    const char *operation;
    XrXirStatus status;
    XrXirCallStatus call_status;
} ArrayRun;
#define OBSERVE(c) do { if (!(c)) { fprintf(stderr, \
    "array-cleanup fixture=%s operation=%s line=%d condition=%s\n", \
    run->fixture->name, run->operation, __LINE__, #c); return false; } } while (0)

static bool phase(ArrayRun *run, const char *name) {
    XrCompileResourceStats stats = {0}; run->operation = name;
    OBSERVE(xr_compile_resources_stats(run->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(stats.live_bytes == instance_compile_bytes);
    printf("array-cleanup fixture=%s phase=%s sites=%zu live=%" PRIu64 " physical=%zu/%zu\n",
        run->fixture->name, name, instance_compile_attempts, stats.live_bytes,
        instance_compile_live, instance_compile_bytes);
    return true;
}
static bool digest_is(const uint8_t *bytes, size_t length, const char *expected) {
    static const char digits[] = "0123456789abcdef";
    uint8_t digest[32]; char text[65]; xr_sha256(bytes, length, digest);
    for (size_t i = 0; i < 32; ++i) {
        text[i * 2] = digits[digest[i] >> 4]; text[i * 2 + 1] = digits[digest[i] & 15];
    }
    text[64] = 0; return !strcmp(text, expected);
}
static bool named(const XrXirFunction *function, const char *name) {
    size_t length = strlen(name);
    return function->name_length == length && !memcmp(function->name, name, length);
}
static bool inspect_roles(ArrayRun *run, const XrXirArtifact *artifact, XrXirStage stage,
    ArrayRoles *roles) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *d = module ? module->declarations : NULL;
    OBSERVE(module && module->stage == stage && d && d->root_module < d->module_count);
    roles->entry = d->entry_function; roles->checked = roles->cleanup = UINT32_MAX;
    OBSERVE(roles->entry < module->function_count && d->functions);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *id = &d->functions[f];
        if (id->module != d->root_module) continue;
        if (named(fn, "consumerChecked")) {
            OBSERVE(roles->checked == UINT32_MAX && id->exported && !id->test_role);
            OBSERVE(fn->parameter_count == 1 && fn->parameters && fn->parameters[0] == XR_XIR_BOOL);
            OBSERVE(fn->result == XR_XIR_I64); roles->checked = f;
        }
        if (named(fn, "consumerCleanup")) {
            OBSERVE(roles->cleanup == UINT32_MAX && id->exported && !id->test_role);
            OBSERVE(!fn->parameter_count && fn->result == XR_XIR_I64); roles->cleanup = f;
        }
    }
    OBSERVE(roles->checked != UINT32_MAX && roles->cleanup != UINT32_MAX);
    OBSERVE(roles->entry != roles->checked && roles->entry != roles->cleanup);
    const XrXirFunction *entry = &module->functions[roles->entry]; unsigned returns = 0;
    OBSERVE(!entry->parameter_count && entry->result == XR_XIR_I64);
    for (uint32_t i = 0; i < entry->instruction_count; ++i) {
        const XrXirInstruction *op = &entry->instructions[i];
        if (op->op != XR_XIR_RETURN) continue;
        OBSERVE(op->args[0] < entry->instruction_count);
        const XrXirInstruction *literal = &entry->instructions[op->args[0]];
        OBSERVE(literal->op == XR_XIR_CONST_INT && literal->type == XR_XIR_I64 && !literal->immediate);
        ++returns;
    }
    OBSERVE(returns == 1);
    unsigned slots = 0;
    for (uint32_t s = 0; s < d->slot_count; ++s) if (d->slots[s].module == d->root_module) {
        OBSERVE(d->slots[s].type == XR_XIR_I64 && d->slots[s].mutable == 1); ++slots;
    }
    OBSERVE(slots == 1);
    printf("array-cleanup fixture=%s metadata-stage=%u entry=%u checked=%u cleanup=%u\n",
        run->fixture->name, (unsigned)stage, roles->entry, roles->checked, roles->cleanup);
    return true;
}
static bool produce(ArrayRun *run) {
    run->operation = "external-original-prefix-and-adapters";
    XrOsIoPolicy policy = xr_compile_io_policy(run->context.resources);
    OBSERVE(xr_os_io_read_regular_file(&policy, run->file, run->fixture->full_bytes,
        &run->input, &run->input_length) == XR_OS_IO_OK);
    OBSERVE(run->input_length == run->fixture->full_bytes);
    OBSERVE(digest_is(run->input, run->fixture->prefix_bytes, run->fixture->prefix_sha));
    OBSERVE(digest_is(run->input, run->input_length, run->fixture->full_sha));
    xr_compile_resources_free(run->input); run->input = NULL;
    OBSERVE(xr_compile_session_new(run->context.resources, &run->session) == XR_COMPILER_SESSION_OK);
    run->authority = (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_SCRIPT, NULL, run->root};
    run->request = (XrXirSourceProductRequest){{run->session, run->file, &run->authority,
        &run->context, run->stdlib, NULL, XR_XIR_PROGRAM, NULL}, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    run->operation = "SourceProduct";
    run->status = xr_xir_compile_source_product_build(&run->request, &run->product, &run->diagnostic);
    OBSERVE(run->status == XR_XIR_OK && run->product);
    OBSERVE(xr_xir_compile_source_product_verify(run->product, 16777216, NULL) == XR_XIR_OK);
    XrXirSourceProductPacketView packet = {0};
    OBSERVE(xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
    OBSERVE(packet.bytes && packet.length);
    OBSERVE(xr_xir_compile_checked_read(&run->context, packet.bytes, packet.length,
        &run->checked, &run->xir) == XR_XIR_OK);
    OBSERVE(inspect_roles(run, run->checked, XR_XIR_CHECKED, &run->roles));
    OBSERVE(xr_xir_compile_checked_write(run->checked, &run->retained, &run->xir) == XR_XIR_OK);
    OBSERVE(run->retained.bytes != packet.bytes && run->retained.length == packet.length);
    OBSERVE(!memcmp(run->retained.bytes, packet.bytes, packet.length));
    xr_compile_session_free(run->session); run->session = NULL;
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    memset(run->root, 0xa5, sizeof(run->root)); memset(run->file, 0xa5, sizeof(run->file));
    memset(run->stdlib, 0xa5, sizeof(run->stdlib));
    memset(&run->request, 0xa5, sizeof(run->request)); memset(&run->authority, 0xa5, sizeof(run->authority));
    return phase(run, "source-owners-and-input-buffers-dead");
}
static bool lower_owned(ArrayRun *run) {
    run->operation = "detached-Checked";
    OBSERVE(xr_xir_compile_artifact_verify(run->checked, &run->xir) == XR_XIR_OK);
    XrXirCheckedPacket repeated = {0};
    run->status = xr_xir_compile_checked_write(run->checked, &repeated, &run->xir);
    bool identical = run->status == XR_XIR_OK && repeated.length == run->retained.length &&
        !memcmp(repeated.bytes, run->retained.bytes, repeated.length);
    xr_xir_compile_checked_packet_free(&repeated); OBSERVE(identical);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    OBSERVE(xr_xir_compile_lower(run->checked, &target, &run->lowered, &run->xir) == XR_XIR_OK);
    xr_xir_compile_artifact_free(run->checked); run->checked = NULL;
    xr_xir_compile_checked_packet_free(&run->retained);
    OBSERVE(xr_xir_compile_artifact_verify(run->lowered, &run->xir) == XR_XIR_OK);
    ArrayRoles roles = {0}; OBSERVE(inspect_roles(run, run->lowered, XR_XIR_LOWERED, &roles));
    OBSERVE(roles.entry == run->roles.entry && roles.checked == run->roles.checked && roles.cleanup == run->roles.cleanup);
    return phase(run, "detached-Lowered-Checked-dead");
}
static bool create_instances(ArrayRun *run) {
    OBSERVE(run->program);
    XrXirInstanceConfig config = {0};
    OBSERVE(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i)
        OBSERVE(xr_xir_instance_new(run->program, &config, &run->instances[i]) == XR_XIR_CALL_READY);
    OBSERVE(run->instances[0] && run->instances[1] && run->instances[0] != run->instances[1]);
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    return phase(run, "two-instances-caller-Program-dead");
}
static bool seal_vm(ArrayRun *run) {
    run->operation = "VM-Program-take";
    OBSERVE(xr_xir_compile_vm_program_take(&run->lowered, &run->program) == XR_XIR_OK);
    OBSERVE(!run->lowered && run->program);
    return create_instances(run);
}
static bool poll_terminal(ArrayRun *run, XrXirInstance *instance, XrXirInstanceResult *result) {
    size_t polls = 0;
    do {
        OBSERVE(++polls <= 4096);
        *result = xr_xir_instance_poll_bounded(instance, UINT64_C(1000000));
    } while (result->outcome.status == XR_XIR_CALL_READY);
    run->call_status = result->outcome.status; return true;
}
static bool scalar_call(ArrayRun *run, XrXirInstance *instance, uint32_t function,
    const XrXirValue *argument, int64_t expected) {
    OBSERVE(xr_xir_instance_start(instance, function, argument, argument ? 1u : 0u) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = {0}; OBSERVE(poll_terminal(run, instance, &result));
    OBSERVE(result.outcome.status == XR_XIR_CALL_RETURNED);
    OBSERVE(xr_xir_instance_take_result(instance, &run->value) == XR_XIR_CALL_RETURNED);
    int64_t actual = run->value.payload;
    bool equal = run->value.type == XR_XIR_I64 && !run->value.reserved && actual == expected;
    xr_xir_value_drop(&run->value); OBSERVE(equal);
    OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    ++run->returns;
    printf("array-cleanup fixture=%s returned-function=%u expected=%" PRId64 " actual=%" PRId64 "\n",
        run->fixture->name, function, expected, actual);
    return true;
}
static bool capture_failure(ArrayRun *run, XrXirInstance *instance, unsigned ordinal, unsigned repeat) {
    run->operation = "checked-false-required-panic452";
    XrXirValue flag = {XR_XIR_BOOL, 0, 0};
    OBSERVE(xr_xir_instance_start(instance, run->roles.checked, &flag, 1) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = {0}; OBSERVE(poll_terminal(run, instance, &result));
    printf("array-cleanup fixture=%s instance=%u repeat=%u negative-terminal=%u actual-code=%u required-code=452\n",
        run->fixture->name, ordinal, repeat, (unsigned)result.outcome.status, result.outcome.panic.detail.code);
    OBSERVE(xr_xir_call_panic_status(result.outcome.status) && xr_xir_call_result_valid(&result.outcome));
    OBSERVE(xr_xir_panic_valid(&result.outcome.panic) && !xr_xir_panic_empty(&result.outcome.panic));
    OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY && run->escaped_count < 4);
    unsigned index = run->escaped_count;
    OBSERVE(xr_xir_call_result_copy(&result.outcome, &run->escaped[index]) == XR_XIR_VALUE_OK);
    ++run->escaped_count;
    FailureSnapshot *snapshot = &run->snapshots[index];
    snapshot->status = result.outcome.status; snapshot->detail = result.outcome.panic.detail;
    snapshot->message_type = result.outcome.panic.message.type;
    ++run->legacy_comparisons;
    if (snapshot->detail.code != 452u) {
        run->legacy_pass = false;
        fprintf(stderr, "array-cleanup legacy-mismatch fixture=%s required452 actual%u; continuing diagnostics with final FAIL\n",
            run->fixture->name, snapshot->detail.code);
    }
    char scratch[XR_XIR_PANIC_MESSAGE_CAPACITY]; XrErrorCoreMessageView view = {0};
    OBSERVE(xr_xir_panic_message_borrow(&run->escaped[index].panic, scratch, sizeof(scratch), &view));
    OBSERVE(view.has_code && view.code == (int)snapshot->detail.code && view.message_len <= sizeof(snapshot->text));
    snapshot->length = view.message_len; memcpy(snapshot->text, view.message, snapshot->length);
    return true;
}
static bool execute_instances(ArrayRun *run) {
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = run->instances[i];
        run->operation = "canonical-entry-zero";
        OBSERVE(scalar_call(run, instance, run->roles.entry, NULL, 0));
        run->operation = "fresh-cleanup-zero";
        OBSERVE(scalar_call(run, instance, run->roles.cleanup, NULL, 0));
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            OBSERVE(capture_failure(run, instance, i, repeat));
            run->operation = "same-instance-failure-cleanup22";
            OBSERVE(scalar_call(run, instance, run->roles.cleanup, NULL, 22));
            run->operation = "same-instance-positive42";
            XrXirValue flag = {XR_XIR_BOOL, 0, 1};
            OBSERVE(scalar_call(run, instance, run->roles.checked, &flag, 42));
            run->operation = "same-instance-positive-cleanup11";
            OBSERVE(scalar_call(run, instance, run->roles.cleanup, NULL, 11));
        }
    }
    OBSERVE(run->returns == 16 && run->escaped_count == 4);
    return phase(run, "two-instances-panic-cleanup-and-positive-diagnostics-complete");
}
static bool retained_failure_same(const XrXirCallResult *result, const FailureSnapshot *snapshot) {
    if (!xr_xir_call_result_valid(result) || result->status != snapshot->status ||
        result->panic.detail.code != snapshot->detail.code || result->panic.detail.reserved != snapshot->detail.reserved ||
        result->panic.detail.index != snapshot->detail.index || result->panic.detail.length != snapshot->detail.length ||
        result->panic.message.type != snapshot->message_type) return false;
    char scratch[XR_XIR_PANIC_MESSAGE_CAPACITY]; XrErrorCoreMessageView view = {0};
    return xr_xir_panic_message_borrow(&result->panic, scratch, sizeof(scratch), &view) &&
        view.has_code && view.code == (int)snapshot->detail.code &&
        view.message_len == snapshot->length && !memcmp(view.message, snapshot->text, snapshot->length);
}
static bool release(ArrayRun *run) {
    bool complete = true; xr_xir_value_drop(&run->value);
    for (unsigned i = 0; i < 2; ++i) if (run->instances[i]) {
        XrXirCallStatus status = xr_xir_instance_free(run->instances[i]);
        if (status == XR_XIR_CALL_BUSY) complete = false;
        else { run->instances[i] = NULL; if (status != XR_XIR_CALL_READY) complete = false; }
    }
    if (run->instances[0] || run->instances[1]) {
        fprintf(stderr, "array-cleanup BUSY owner retained; release and physical-zero evidence unavailable\n");
        return false;
    }
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    xr_xir_compile_artifact_free(run->checked); run->checked = NULL;
    xr_xir_compile_checked_packet_free(&run->retained);
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    xr_compile_session_free(run->session); run->session = NULL;
    xr_compile_resources_free(run->input); run->input = NULL;
    if (run->context.resources) {
        if (xr_compile_resources_stats(run->context.resources, &run->final) != XR_COMPILE_RESOURCE_OK) complete = false;
        if (run->final.live_bytes != run->baseline.live_bytes || run->final.live_bytes != instance_compile_bytes) complete = false;
        xr_compile_resources_release(run->context.resources); run->context.resources = NULL;
    }
    printf("array-cleanup fixture=%s execution-and-compiler-owners-dead compiler=%zu/%zu retained=%u runtime=%zu/%zu\n",
        run->fixture->name, instance_compile_live, instance_compile_bytes, run->escaped_count, runtime_live, runtime_bytes);
    for (unsigned i = 0; i < run->escaped_count; ++i) {
        bool same = retained_failure_same(&run->escaped[i], &run->snapshots[i]);
        if (!same) complete = false;
        if (run->escaped[i].panic.detail.code != 452u) run->legacy_pass = false;
        printf("array-cleanup retained=%u owners-dead=1 actual-code=%u required-code=452 same-snapshot=%u\n",
            i, run->escaped[i].panic.detail.code, same ? 1u : 0u);
        xr_xir_call_result_drop(&run->escaped[i]);
        if (!xr_xir_call_result_empty(&run->escaped[i])) complete = false;
    }
    if (instance_compile_live || instance_compile_bytes || runtime_live || runtime_bytes || runtime_owned || runtime_owned_capacity)
        complete = false;
    printf("array-cleanup fixture=%s release compiler=%zu/%zu runtime=%zu/%zu table=%zu result=%s\n",
        run->fixture->name, instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes,
        runtime_owned_capacity, complete ? "PASS" : "FAIL");
    return complete;
}
static bool seal_selected(ArrayRun *run, unsigned mode) {
    if (!mode) return seal_vm(run);
    run->operation = "native-or-mixed-Program-seal";
    run->status = array_cleanup_native_seal(&run->context, &run->lowered,
        array_cleanup_compiled_image(), mode, &run->program);
    OBSERVE(run->status == XR_XIR_OK && run->program);
    if (mode == 1) { xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL; }
    OBSERVE(!run->lowered);
    return create_instances(run);
}
static int finish(ArrayRun *run, unsigned mode, bool emitting, bool observed) {
    if (!observed) fprintf(stderr, "array-cleanup diagnostic-failure fixture=%s operation=%s xir-status=%u source-status=%u line=%d message=%s\n",
        run->fixture->name, run->operation, (unsigned)run->status, (unsigned)run->diagnostic.source.status,
        run->diagnostic.source.line, run->diagnostic.source.message);
    bool released = release(run);
    bool passed = observed && released && (emitting || (run->legacy_pass && run->legacy_comparisons == 4));
    const char *legacy = !run->legacy_comparisons ? "NOT_RUN" :
        run->legacy_comparisons != 4 ? "PARTIAL_FAIL" : !run->legacy_pass ? "FAIL" :
        observed && released ? "PASS" : "PARTIAL_FAIL";
    printf("array-cleanup fixture=%s mode=%u emission=%u observed=%s legacy452=%s legacy-compared=%u compiler-sites=%zu runtime-sites=%zu "
        "allocated=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 " scalar-returns=%u copied-panic-facts=%u "
        "fullFI=NOT_RUN class-identities=OPEN original-stderr=OPEN result=%s\n",
        run->fixture->name, mode, emitting ? 1u : 0u, observed ? "PASS" : "FAIL", legacy, run->legacy_comparisons,
        instance_compile_attempts, runtime_attempts, run->final.allocated_bytes, run->final.peak_bytes, run->final.work,
        run->returns, run->escaped_count, passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
int main(int argc, char **argv) {
    if ((argc != 6 && argc != 8) || strlen(argv[2]) != 1 || argv[2][0] < '0' || argv[2][0] > '3') return 2;
    bool emitting = argc == 8;
    if (emitting && strcmp(argv[6], "--emit")) return 2;
    unsigned mode = (unsigned)(argv[2][0] - '0');
    if (emitting && mode) return 2;
    ArrayRun run = {0};
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i)
        if (!strcmp(argv[1], fixtures[i].name)) run.fixture = &fixtures[i];
    if (!run.fixture || strlen(argv[3]) >= sizeof(run.root) || strlen(argv[4]) >= sizeof(run.file) ||
        strlen(argv[5]) >= sizeof(run.stdlib)) return 2;
    strcpy(run.root, argv[3]); strcpy(run.file, argv[4]); strcpy(run.stdlib, argv[5]); run.legacy_pass = true;
    instance_compile_zero(); CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    run.context.limits = xr_xir_compile_default_limits(); run.operation = "finite-compiler-owner";
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrCompileResourceStatus owner = xr_compile_resources_new(&limits, &run.context.resources);
    bool observed = owner == XR_COMPILE_RESOURCE_OK;
    if (observed) observed = xr_compile_resources_stats(run.context.resources, &run.baseline) == XR_COMPILE_RESOURCE_OK;
    if (observed) observed = produce(&run) && lower_owned(&run);
    if (observed && emitting) {
        run.operation = "actual-Lowered-C-emission";
        run.status = array_cleanup_write_native(run.lowered, argv[7]); observed = run.status == XR_XIR_OK;
    } else if (observed) observed = seal_selected(&run, mode) && execute_instances(&run);
    return finish(&run, mode, emitting, observed);
}
