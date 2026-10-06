/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_allocation_scenario0.c - Original source allocation ownership
 *
 * KEY CONCEPT:
 *   Unmodified source declarations survive producer destruction as Checked
 *   and Lowered owners; isolated instances retain their sealed code lease.
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

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked input");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Current public execution contracts");

static const char original_source[] =
    "fn answer() -> i64 { return 0 }\n"
    "fn exported() -> i64 { return 42 }\n"
    "@test\nfn checkAnswer() { assert(exported() == 42) }\n";

typedef struct AllocationRoles {
    uint32_t entry, initializer, answer, exported, test;
} AllocationRoles;

typedef struct AllocationRun {
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
    AllocationRoles roles;
    const char *operation;
    XrXirStatus status;
    XrXirCallStatus call_status;
    uint8_t *input;
    size_t input_length;
} AllocationRun;

#define OBSERVE(condition) do { if (!(condition)) { \
    fprintf(stderr, "source-allocation0 check line=%d operation=%s condition=%s\n", \
        __LINE__, run->operation, #condition); return false; } } while (0)

static bool phase(AllocationRun *run, const char *name) {
    XrCompileResourceStats stats = {0};
    run->operation = name;
    OBSERVE(xr_compile_resources_stats(run->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(stats.live_bytes == instance_compile_bytes);
    printf("source-allocation0 phase=%s sites=%zu allocations=%" PRIu64 " allocated=%" PRIu64
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

static bool inspect_module(AllocationRun *run, const XrXirArtifact *artifact,
                           XrXirStage stage, AllocationRoles *roles) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *d = module ? module->declarations : NULL;
    OBSERVE(module && module->stage == stage && d && d->root_module < d->module_count);
    *roles = (AllocationRoles){d->entry_function, d->modules[d->root_module].initializer,
        UINT32_MAX, UINT32_MAX, UINT32_MAX};
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *identity = &d->functions[f];
        if (identity->module != d->root_module) continue;
        if (named(fn, "answer")) {
            OBSERVE(roles->answer == UINT32_MAX && !identity->exported && !identity->test_role);
            OBSERVE(integer_return(fn, 0)); roles->answer = f;
        } else if (named(fn, "exported")) {
            OBSERVE(roles->exported == UINT32_MAX && !identity->exported && !identity->test_role);
            OBSERVE(integer_return(fn, 42)); roles->exported = f;
        } else if (named(fn, "checkAnswer")) {
            OBSERVE(roles->test == UINT32_MAX && !identity->exported);
            OBSERVE(identity->test_role == XR_XIR_TEST_ROLE_TEST && !identity->test_timeout_seconds);
            OBSERVE(!fn->parameter_count && fn->result == XR_XIR_UNIT); roles->test = f;
        }
    }
    OBSERVE(roles->answer != UINT32_MAX && roles->exported != UINT32_MAX && roles->test != UINT32_MAX);
    OBSERVE(roles->entry < module->function_count && roles->initializer < module->function_count);
    OBSERVE(roles->entry != roles->initializer && roles->entry != roles->answer && roles->entry != roles->test);
    OBSERVE(integer_return(&module->functions[roles->entry], 0));
    OBSERVE(module->functions[roles->initializer].result == XR_XIR_UNIT);
    OBSERVE(!d->functions[roles->initializer].exported && !d->functions[roles->initializer].test_role);
    printf("source-allocation0 metadata stage=%u root=%u entry=%u initializer=%u answer=%u "
        "exported=%u test=%u expected-answer=0 expected-exported=42 expected-test=Unit\n",
        (unsigned)stage, d->root_module, roles->entry, roles->initializer,
        roles->answer, roles->exported, roles->test);
    return true;
}

static bool inspect_product(AllocationRun *run, const char *file) {
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
    OBSERVE(tests->count == 1 && tests->entries && tests->entries[0].function == run->roles.test);
    OBSERVE(tests->entries[0].role == XR_XIR_TEST_ROLE_TEST && !tests->entries[0].timeout_seconds);
    OBSERVE(tests->entries[0].name_length == 11 && !memcmp(tests->entries[0].name, "checkAnswer", 11));
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

static bool produce(AllocationRun *run, const char *root, const char *file) {
    XrOsIoPolicy policy = xr_compile_io_policy(run->context.resources);
    run->operation = "original-input";
    XrOsIoStatus io = xr_os_io_read_regular_file(&policy, file, sizeof(original_source) - 1,
        &run->input, &run->input_length);
    if (io != XR_OS_IO_OK) fprintf(stderr, "source-allocation0 input io-status=%u\n", (unsigned)io);
    OBSERVE(io == XR_OS_IO_OK && run->input_length == sizeof(original_source) - 1);
    OBSERVE(!memcmp(run->input, original_source, run->input_length));
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

static bool read_packets(AllocationRun *run, const char *file) {
    XrXirSourceProductPacketView source = {0}, closed = {0};
    AllocationRoles source_roles;
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

static bool detach_lower(AllocationRun *run) {
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
    xr_xir_compile_artifact_free(reread);
    OBSERVE(run->status == XR_XIR_OK);
    run->operation = "Lowered";
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run->status = xr_xir_compile_lower(run->closed_checked, &target, &run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    AllocationRoles lowered_roles;
    if (!inspect_module(run, run->lowered, XR_XIR_LOWERED, &lowered_roles)) return false;
    OBSERVE(run->roles.entry == lowered_roles.entry && run->roles.initializer == lowered_roles.initializer);
    OBSERVE(run->roles.answer == lowered_roles.answer && run->roles.exported == lowered_roles.exported &&
        run->roles.test == lowered_roles.test);
    run->status = xr_xir_compile_artifact_verify(run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&run->retained);
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    xr_xir_compile_artifact_free(run->closed_checked); run->closed_checked = NULL;
    return phase(run, "detached-Lowered-verified");
}

static bool seal_instances(AllocationRun *run) {
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

static bool finish(AllocationRun *run, XrXirInstance *instance, XrXirType type, int64_t payload) {
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

static bool execute_instances(AllocationRun *run) {
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
        OBSERVE(xr_xir_instance_start_test(instance, run->roles.answer) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_xir_instance_start_test(instance, run->roles.exported) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_xir_instance_start_test(instance, run->roles.initializer) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_compile_resources_stats(run->context.resources, &after) == XR_COMPILE_RESOURCE_OK);
        OBSERVE(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes);
        OBSERVE(before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes && before.work == after.work);
        OBSERVE(instance_compile_attempts == compiler_attempts && runtime_attempts == attempts);
        OBSERVE(runtime_live == blocks && runtime_bytes == bytes && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_NEW);
        run->operation = "canonical-entry";
        run->call_status = xr_xir_instance_start(instance, run->roles.entry, NULL, 0);
        OBSERVE(run->call_status == XR_XIR_CALL_READY);
        if (!finish(run, instance, XR_XIR_I64, 0)) return false;
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            run->operation = "original-checkAnswer";
            run->call_status = xr_xir_instance_start_test(instance, run->roles.test);
            OBSERVE(run->call_status == XR_XIR_CALL_READY);
            if (!finish(run, instance, XR_XIR_UNIT, 0)) return false;
            printf("source-allocation0 instance=%u repeat=%u original-checkAnswer=Unit exported-fixed-oracle=42\n",
                i, repeat);
        }
        OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    }
    return true;
}

static bool release(AllocationRun *run) {
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
    if (instance_compile_live || instance_compile_bytes || runtime_live || runtime_bytes ||
        runtime_owned || runtime_owned_capacity) complete = false;
    printf("source-allocation0 release compiler=%zu/%zu runtime=%zu/%zu table=%zu result=%s\n",
        instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes,
        runtime_owned_capacity, complete ? "PASS" : "FAIL");
    return complete;
}

typedef enum AllocationFaultStage {
    ALLOCATION_FAULT_OWNER, ALLOCATION_FAULT_INPUT, ALLOCATION_FAULT_SESSION,
    ALLOCATION_FAULT_PRODUCT, ALLOCATION_FAULT_SOURCE_READ, ALLOCATION_FAULT_CLOSED_READ,
    ALLOCATION_FAULT_RETAIN, ALLOCATION_FAULT_SOURCE_VERIFY, ALLOCATION_FAULT_CLOSED_VERIFY,
    ALLOCATION_FAULT_REREAD, ALLOCATION_FAULT_REREAD_VERIFY, ALLOCATION_FAULT_LOWER,
    ALLOCATION_FAULT_LOWER_VERIFY, ALLOCATION_FAULT_PROGRAM, ALLOCATION_FAULT_COUNT
} AllocationFaultStage;
typedef struct AllocationFaultSample {
    XrCompileResourceStats ledger;
    size_t sites, blocks, bytes;
    bool valid, injected;
} AllocationFaultSample;
typedef struct AllocationFaultCall {
    AllocationFaultSample before, after;
    const void *outputs[2];
    size_t widths[2];
    unsigned char original[2][sizeof(XrXirCheckedPacket)];
    int status;
    bool called, output_preserved, live_preserved;
} AllocationFaultCall;
typedef struct AllocationFault {
    AllocationRun run;
    XrXirArtifact *reread;
    AllocationFaultCall calls[ALLOCATION_FAULT_COUNT];
    AllocationFaultStage current;
    size_t normal_sites, site, hits, entered;
    bool accounting_ok, stopped, diagnostic_ok;
} AllocationFault;

static const char *allocation_fault_name(AllocationFaultStage stage) {
    static const char *const names[] = {"owner-new", "original-input", "session-new", "source-product",
        "source-Checked-reader", "closed-Checked-reader", "closed-Checked-retained", "detached-source-verify",
        "detached-closed-verify", "retained-Checked-reread", "retained-Checked-verify", "Lowered",
        "Lowered-verify", "VM-Program-take"};
    return names[(unsigned)stage];
}
static int allocation_fault_oom(AllocationFaultStage stage) {
    if (stage == ALLOCATION_FAULT_OWNER) return (int)XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    if (stage == ALLOCATION_FAULT_INPUT) return (int)XR_OS_IO_OUT_OF_MEMORY;
    if (stage == ALLOCATION_FAULT_SESSION) return (int)XR_COMPILER_SESSION_OUT_OF_MEMORY;
    return (int)XR_XIR_OUT_OF_MEMORY;
}
static AllocationFaultSample allocation_fault_sample(AllocationFault *fault) {
    AllocationFaultSample sample = {0};
    sample.sites = instance_compile_attempts;
    sample.blocks = instance_compile_live; sample.bytes = instance_compile_bytes;
    sample.injected = instance_compile_injected;
    if (fault->run.context.resources) {
        sample.valid = xr_compile_resources_stats(fault->run.context.resources, &sample.ledger) == XR_COMPILE_RESOURCE_OK;
        if (!sample.valid || sample.ledger.live_bytes != sample.bytes ||
            sample.ledger.allocated_bytes > UINT64_C(67108864) || sample.ledger.peak_bytes > UINT64_C(8388608) ||
            sample.ledger.work > UINT64_C(128000000)) fault->accounting_ok = false;
    } else if (sample.blocks || sample.bytes) fault->accounting_ok = false;
    return sample;
}
static void allocation_fault_begin(AllocationFault *fault, AllocationFaultStage stage,
    const void *first, size_t first_width, const void *second, size_t second_width) {
    AllocationFaultCall *call = &fault->calls[stage];
    if (fault->stopped || fault->entered != (size_t)stage || call->called || instance_compile_injected)
        fault->accounting_ok = false;
    fault->current = stage; ++fault->entered;
    fault->run.operation = allocation_fault_name(stage);
    call->called = true; call->before = allocation_fault_sample(fault);
    call->outputs[0] = first; call->widths[0] = first_width;
    call->outputs[1] = second; call->widths[1] = second_width;
    for (unsigned i = 0; i < 2; ++i) {
        if (call->widths[i] > sizeof(call->original[i]) || (call->widths[i] && !call->outputs[i]))
            fault->accounting_ok = false;
        else if (call->widths[i]) memcpy(call->original[i], call->outputs[i], call->widths[i]);
    }
    if (stage >= ALLOCATION_FAULT_SOURCE_READ && stage <= ALLOCATION_FAULT_LOWER_VERIFY)
        fault->run.xir = (XrXirDiagnostic){XR_XIR_BAD_STRUCTURE, 17, 18, 19, XR_XIR_DIAGNOSTIC_NONE};
}
static bool allocation_fault_end(AllocationFault *fault, int status) {
    AllocationFaultCall *call = &fault->calls[fault->current];
    call->status = status; call->after = allocation_fault_sample(fault);
    if (!call->before.injected && call->after.injected) ++fault->hits;
    call->output_preserved = true;
    for (unsigned i = 0; i < 2; ++i)
        if (call->widths[i] && memcmp(call->original[i], call->outputs[i], call->widths[i]))
            call->output_preserved = false;
    call->live_preserved = call->before.blocks == call->after.blocks && call->before.bytes == call->after.bytes &&
        call->before.valid == call->after.valid &&
        (!call->before.valid || call->before.ledger.live_bytes == call->after.ledger.live_bytes);
    printf("source-allocation0 compiler-call stage=%s status=%d expected-oom=%d site-begin=%zu site-end=%zu "
        "hit-transitions=%zu output-preserved=%u live-preserved=%u valid=%u count=%" PRIu64
        " allocated=%" PRIu64 " live=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64 " physical=%zu/%zu\n",
        allocation_fault_name(fault->current), status, allocation_fault_oom(fault->current),
        call->before.sites, call->after.sites, fault->hits, (unsigned)call->output_preserved,
        (unsigned)call->live_preserved, (unsigned)call->after.valid, call->after.ledger.allocation_count,
        call->after.ledger.allocated_bytes, call->after.ledger.live_bytes, call->after.ledger.peak_bytes,
        call->after.ledger.work, call->after.blocks, call->after.bytes);
    if (!status && !call->after.injected) return true;
    fault->stopped = true;
    if (fault->current == ALLOCATION_FAULT_PRODUCT) {
        XrXirSourceProductDiagnostic *diagnostic = &fault->run.diagnostic;
        fault->diagnostic_ok = diagnostic->status == XR_XIR_OUT_OF_MEMORY &&
            diagnostic->stage >= XR_XIR_SOURCE_PRODUCT_CHECK && diagnostic->stage <= XR_XIR_SOURCE_PRODUCT_FACTS;
        if (diagnostic->stage == XR_XIR_SOURCE_PRODUCT_CHECK)
            fault->diagnostic_ok = fault->diagnostic_ok && diagnostic->source.status == XR_XIR_OUT_OF_MEMORY;
        else if (diagnostic->stage != XR_XIR_SOURCE_PRODUCT_FACTS)
            fault->diagnostic_ok = fault->diagnostic_ok && diagnostic->xir.status == XR_XIR_OUT_OF_MEMORY;
    } else if (fault->current >= ALLOCATION_FAULT_SOURCE_READ && fault->current <= ALLOCATION_FAULT_LOWER_VERIFY)
        fault->diagnostic_ok = fault->run.xir.status == XR_XIR_OUT_OF_MEMORY;
    else fault->diagnostic_ok = true;
    return false;
}

static bool allocation_fault_produce(AllocationFault *fault, const char *root, const char *file) {
    AllocationRun *run = &fault->run;
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    allocation_fault_begin(fault, ALLOCATION_FAULT_OWNER, &run->context.resources, sizeof(run->context.resources), NULL, 0);
    XrCompileResourceStatus owner = xr_compile_resources_new(&limits, &run->context.resources);
    if (!allocation_fault_end(fault, (int)owner)) return false;
    OBSERVE(run->context.resources && xr_compile_resources_stats(run->context.resources, &run->baseline) == XR_COMPILE_RESOURCE_OK);
    if (!phase(run, "owner-created")) return false;
    XrOsIoPolicy policy = xr_compile_io_policy(run->context.resources);
    allocation_fault_begin(fault, ALLOCATION_FAULT_INPUT, &run->input, sizeof(run->input), &run->input_length, sizeof(run->input_length));
    XrOsIoStatus io = xr_os_io_read_regular_file(&policy, file, sizeof(original_source) - 1, &run->input, &run->input_length);
    if (!allocation_fault_end(fault, (int)io)) return false;
    OBSERVE(run->input_length == sizeof(original_source) - 1 && !memcmp(run->input, original_source, run->input_length));
    xr_compile_resources_free(run->input); run->input = NULL;
    if (!phase(run, "original-input-verified")) return false;
    allocation_fault_begin(fault, ALLOCATION_FAULT_SESSION, &run->session, sizeof(run->session), NULL, 0);
    XrCompilerSessionStatus session = xr_compile_session_new(run->context.resources, &run->session);
    if (!allocation_fault_end(fault, (int)session)) return false;
    OBSERVE(run->session);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{run->session, file, &authority, &run->context,
        NULL, NULL, XR_XIR_PROGRAM, NULL}, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    allocation_fault_begin(fault, ALLOCATION_FAULT_PRODUCT, &run->product, sizeof(run->product), NULL, 0);
    run->status = xr_xir_compile_source_product_build(&request, &run->product, &run->diagnostic);
    if (!allocation_fault_end(fault, (int)run->status)) return false;
    OBSERVE(run->product);
    return phase(run, "source-product-complete");
}
static bool allocation_fault_packets(AllocationFault *fault, const char *file) {
    AllocationRun *run = &fault->run;
    XrXirSourceProductPacketView source = {0}, closed = {0};
    AllocationRoles source_roles;
    run->status = xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_SOURCE, &source);
    OBSERVE(run->status == XR_XIR_OK && source.bytes && source.length);
    allocation_fault_begin(fault, ALLOCATION_FAULT_SOURCE_READ, &run->source_checked, sizeof(run->source_checked), NULL, 0);
    run->status = xr_xir_compile_checked_read(&run->context, source.bytes, source.length, &run->source_checked, &run->xir);
    if (!allocation_fault_end(fault, (int)run->status)) return false;
    if (!inspect_module(run, run->source_checked, XR_XIR_CHECKED, &source_roles)) return false;
    if (!phase(run, "source-Checked-reader")) return false;
    run->status = xr_xir_compile_source_product_packet(run->product, XR_XIR_SOURCE_PRODUCT_CLOSED, &closed);
    OBSERVE(run->status == XR_XIR_OK && closed.bytes && closed.length);
    allocation_fault_begin(fault, ALLOCATION_FAULT_CLOSED_READ, &run->closed_checked, sizeof(run->closed_checked), NULL, 0);
    run->status = xr_xir_compile_checked_read(&run->context, closed.bytes, closed.length, &run->closed_checked, &run->xir);
    if (!allocation_fault_end(fault, (int)run->status)) return false;
    if (!inspect_module(run, run->closed_checked, XR_XIR_CHECKED, &run->roles) || !inspect_product(run, file)) return false;
    allocation_fault_begin(fault, ALLOCATION_FAULT_RETAIN, &run->retained, sizeof(run->retained), NULL, 0);
    run->status = xr_xir_compile_checked_write(run->closed_checked, &run->retained, &run->xir);
    if (!allocation_fault_end(fault, (int)run->status)) return false;
    OBSERVE(run->retained.length == closed.length && run->retained.bytes != closed.bytes);
    OBSERVE(!memcmp(run->retained.bytes, closed.bytes, closed.length));
    return phase(run, "closed-Checked-retained");
}
static bool allocation_fault_verify(AllocationFault *fault, AllocationFaultStage stage, const XrXirArtifact *artifact) {
    allocation_fault_begin(fault, stage, NULL, 0, NULL, 0);
    fault->run.status = xr_xir_compile_artifact_verify(artifact, &fault->run.xir);
    return allocation_fault_end(fault, (int)fault->run.status);
}
static bool allocation_fault_detach(AllocationFault *fault) {
    AllocationRun *run = &fault->run;
    xr_compile_session_free(run->session); run->session = NULL;
    xr_xir_compile_source_product_free(run->product); run->product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&run->diagnostic);
    if (!phase(run, "producers-destroyed")) return false;
    if (!allocation_fault_verify(fault, ALLOCATION_FAULT_SOURCE_VERIFY, run->source_checked) ||
        !allocation_fault_verify(fault, ALLOCATION_FAULT_CLOSED_VERIFY, run->closed_checked)) return false;
    allocation_fault_begin(fault, ALLOCATION_FAULT_REREAD, &fault->reread, sizeof(fault->reread), NULL, 0);
    run->status = xr_xir_compile_checked_read(&run->context, run->retained.bytes, run->retained.length, &fault->reread, &run->xir);
    if (!allocation_fault_end(fault, (int)run->status)) return false;
    if (!allocation_fault_verify(fault, ALLOCATION_FAULT_REREAD_VERIFY, fault->reread)) return false;
    xr_xir_compile_artifact_free(fault->reread); fault->reread = NULL;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    allocation_fault_begin(fault, ALLOCATION_FAULT_LOWER, &run->lowered, sizeof(run->lowered), NULL, 0);
    run->status = xr_xir_compile_lower(run->closed_checked, &target, &run->lowered, &run->xir);
    if (!allocation_fault_end(fault, (int)run->status)) return false;
    AllocationRoles lowered_roles;
    if (!inspect_module(run, run->lowered, XR_XIR_LOWERED, &lowered_roles)) return false;
    OBSERVE(run->roles.entry == lowered_roles.entry && run->roles.initializer == lowered_roles.initializer);
    OBSERVE(run->roles.answer == lowered_roles.answer && run->roles.exported == lowered_roles.exported && run->roles.test == lowered_roles.test);
    if (!allocation_fault_verify(fault, ALLOCATION_FAULT_LOWER_VERIFY, run->lowered)) return false;
    xr_xir_compile_checked_packet_free(&run->retained);
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    xr_xir_compile_artifact_free(run->closed_checked); run->closed_checked = NULL;
    return phase(run, "detached-Lowered-verified");
}
static bool allocation_fault_program(AllocationFault *fault) {
    AllocationRun *run = &fault->run;
    allocation_fault_begin(fault, ALLOCATION_FAULT_PROGRAM, &run->program, sizeof(run->program), &run->lowered, sizeof(run->lowered));
    run->status = xr_xir_compile_vm_program_take(&run->lowered, &run->program);
    if (!allocation_fault_end(fault, (int)run->status)) return false;
    OBSERVE(run->program && !run->lowered);
    return phase(run, "Program-sealed");
}
static bool allocation_fault_expected(const AllocationFault *fault) {
    const AllocationFaultCall *call = &fault->calls[fault->current];
    if (!fault->stopped || fault->hits != 1 || !instance_compile_injected || !call->called ||
        call->before.injected || !call->after.injected || call->status != allocation_fault_oom(fault->current) ||
        fault->site < call->before.sites || fault->site >= call->after.sites ||
        !call->output_preserved || !fault->diagnostic_ok || !fault->accounting_ok) return false;
    if (fault->current != ALLOCATION_FAULT_PRODUCT && !call->live_preserved) return false;
    for (unsigned i = 0; i < (unsigned)ALLOCATION_FAULT_COUNT; ++i) {
        if (i <= (unsigned)fault->current) {
            if (!fault->calls[i].called || (i < (unsigned)fault->current && fault->calls[i].status)) return false;
        } else if (fault->calls[i].called) return false;
    }
    return fault->entered == (size_t)fault->current + 1;
}
static void allocation_fault_report(const AllocationFault *fault) {
    for (unsigned i = 0; i < (unsigned)ALLOCATION_FAULT_COUNT; ++i)
        if (!fault->calls[i].called) printf("source-allocation0 compiler-call stage=%s status=NOT_ENTERED\n",
            allocation_fault_name((AllocationFaultStage)i));
    const AllocationRun *run = &fault->run;
    printf("source-allocation0 compiler-diagnostic stage=%s source-stage=%u product-status=%u source-status=%u "
        "source-module=%u line=%d column=%d source-xir-status=%u xir-status=%u function=%u block=%u instruction=%u reason=%u "
        "snapshot-owned=%u path-owned=%u message=%s\n", allocation_fault_name(fault->current),
        (unsigned)run->diagnostic.stage, (unsigned)run->diagnostic.status, (unsigned)run->diagnostic.source.status,
        run->diagnostic.source.module, run->diagnostic.source.line, run->diagnostic.source.column,
        (unsigned)run->diagnostic.xir.status, (unsigned)run->xir.status, run->xir.function, run->xir.block,
        run->xir.instruction, (unsigned)run->xir.reason, (unsigned)(run->diagnostic.snapshot != NULL),
        (unsigned)(run->diagnostic.source_path != NULL), run->diagnostic.source.message);
}
static bool allocation_fault_cleanup_cost(const AllocationFaultSample *before,
    const XrCompileResourceStats *after, bool had_owner) {
    if (!had_owner) return !before->valid && !before->blocks && !before->bytes;
    return before->valid && before->ledger.allocation_count == after->allocation_count &&
        before->ledger.allocated_bytes == after->allocated_bytes && before->ledger.peak_bytes == after->peak_bytes &&
        before->ledger.work == after->work && after->live_bytes <= before->ledger.live_bytes;
}
static int allocation_fault_run(const char *root, const char *file, size_t normal_sites, size_t site) {
    AllocationFault fault = {0};
    fault.normal_sites = normal_sites; fault.site = site;
    fault.run.context.limits = xr_xir_compile_default_limits();
    fault.accounting_ok = !instance_compile_attempts && !instance_compile_live && !instance_compile_bytes &&
        !instance_compile_injected && instance_compile_fail_at == SIZE_MAX && !runtime_attempts &&
        !runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity && runtime_fail_at == SIZE_MAX;
    bool reached_end = false;
    if (fault.accounting_ok) {
        instance_compile_fail_at = site;
        reached_end = allocation_fault_produce(&fault, root, file) && allocation_fault_packets(&fault, file) &&
            allocation_fault_detach(&fault) && allocation_fault_program(&fault);
    }
    allocation_fault_report(&fault);
    bool expected = !reached_end && allocation_fault_expected(&fault);
    AllocationFaultSample cleanup_before = allocation_fault_sample(&fault);
    bool had_owner = fault.run.context.resources != NULL;
    size_t cleanup_runtime_begin = runtime_attempts;
    instance_compile_fail_at = SIZE_MAX;
    xr_xir_compile_artifact_free(fault.reread); fault.reread = NULL;
    bool released = release(&fault.run);
    bool cleanup_uncharged = cleanup_before.sites == instance_compile_attempts &&
        cleanup_runtime_begin == runtime_attempts && allocation_fault_cleanup_cost(&cleanup_before, &fault.run.final, had_owner);
    printf("source-allocation0 compiler-cleanup had-owner=%u valid-before=%u sites-before=%zu sites-after=%zu "
        "runtime-sites-before=%zu runtime-sites-after=%zu count-before=%" PRIu64 " count-owner-before-release=%" PRIu64
        " allocated-before=%" PRIu64 " allocated-owner-before-release=%" PRIu64 " live-before=%" PRIu64
        " live-owner-before-release=%" PRIu64 " peak-before=%" PRIu64 " peak-owner-before-release=%" PRIu64
        " work-before=%" PRIu64 " work-owner-before-release=%" PRIu64 " cumulative-uncharged=%u\n",
        (unsigned)had_owner, (unsigned)cleanup_before.valid, cleanup_before.sites, instance_compile_attempts,
        cleanup_runtime_begin, runtime_attempts, cleanup_before.ledger.allocation_count, fault.run.final.allocation_count,
        cleanup_before.ledger.allocated_bytes, fault.run.final.allocated_bytes, cleanup_before.ledger.live_bytes,
        fault.run.final.live_bytes, cleanup_before.ledger.peak_bytes, fault.run.final.peak_bytes,
        cleanup_before.ledger.work, fault.run.final.work, (unsigned)cleanup_uncharged);
    bool passed = expected && released && cleanup_uncharged && fault.accounting_ok;
    printf("source-allocation0 compiler-fault=%s frozen-N=%zu fault-site=%zu hit-transitions=%zu failure-stage=%s "
        "actual-status=%d expected-oom=%d output-preserved=%u diagnostic-match=%u cleanup-uncharged=%u "
        "compiler-sites=%zu runtime-sites=%zu allocated=%" PRIu64 " peak=%" PRIu64 " work=%" PRIu64
        " compiler-physical=%zu/%zu runtime-physical=%zu/%zu table=%zu "
        "input-packet-bytes=UNSNAPSHOTTED_OPEN full-eight-source-FI=NOT_RUN runtime-FI=NOT_RUN axes=NOT_RUN native=NOT_RUN\n",
        passed ? "PASS" : "FAIL", normal_sites, site, fault.hits, allocation_fault_name(fault.current),
        fault.calls[fault.current].status, allocation_fault_oom(fault.current),
        (unsigned)fault.calls[fault.current].output_preserved, (unsigned)fault.diagnostic_ok,
        (unsigned)cleanup_uncharged, instance_compile_attempts, runtime_attempts, fault.run.final.allocated_bytes,
        fault.run.final.peak_bytes, fault.run.final.work, instance_compile_live, instance_compile_bytes,
        runtime_live, runtime_bytes, runtime_owned_capacity);
    return passed ? 0 : 1;
}
static bool allocation_fault_number(const char *text, size_t *output) {
    size_t value = 0;
    if (!text || !*text) return false;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        size_t digit = (size_t)(*p - '0');
        if (value > (SIZE_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (value == SIZE_MAX) return false;
    *output = value;
    return true;
}

int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[3], "--compiler-fault")) {
        size_t normal_sites = 0, site = 0;
        if (!allocation_fault_number(argv[4], &normal_sites) || !normal_sites ||
            !allocation_fault_number(argv[5], &site) || site >= normal_sites) return 2;
        return allocation_fault_run(argv[1], argv[2], normal_sites, site);
    }
    if (argc != 3) return 2;
    instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    AllocationRun run = {0};
    run.context.limits = xr_xir_compile_default_limits();
    run.operation = "finite-owner";
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrCompileResourceStatus owner = xr_compile_resources_new(&limits, &run.context.resources);
    bool passed = owner == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = xr_compile_resources_stats(run.context.resources, &run.baseline) == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = phase(&run, "owner-created") && produce(&run, argv[1], argv[2]);
    if (passed) passed = read_packets(&run, argv[2]) && detach_lower(&run) && seal_instances(&run) && execute_instances(&run);
    if (!passed) fprintf(stderr, "source-allocation0 failure operation=%s owner-status=%u status=%u "
        "source-stage=%u source-status=%u module=%u line=%d column=%d xir-status=%u "
        "function=%u block=%u instruction=%u reason=%u call-status=%u message=%s\n", run.operation,
        (unsigned)owner, (unsigned)run.status, (unsigned)run.diagnostic.stage,
        (unsigned)run.diagnostic.source.status, run.diagnostic.source.module, run.diagnostic.source.line,
        run.diagnostic.source.column, (unsigned)run.xir.status, run.xir.function, run.xir.block,
        run.xir.instruction, (unsigned)run.xir.reason, (unsigned)run.call_status, run.diagnostic.source.message);
    bool released = release(&run);
    printf("source-allocation0 normal=%s compiler-sites=%zu runtime-sites=%zu allocated=%" PRIu64
        " peak=%" PRIu64 " work=%" PRIu64 " full-eight-source-FI=NOT_RUN native=NOT_RUN\n",
        passed && released ? "PASS" : "FAIL", instance_compile_attempts, runtime_attempts,
        run.final.allocated_bytes, run.final.peak_bytes, run.final.work);
    return passed && released ? 0 : 1;
}
