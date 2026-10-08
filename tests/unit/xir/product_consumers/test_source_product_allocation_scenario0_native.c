/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_allocation_scenario0_native.c - Legal native Source execution
 *
 * KEY CONCEPT:
 *   Unmodified private source functions retain their permissions in emitted C.
 *   Root-module tests run through the common native Program entry contract.
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
    XrXirCSource emitted, repeated;
    char c_digest[65];
    XrXirProgram *program;
    XrXirInstance *instances[2];
    XrXirValue value;
    AllocationRoles roles;
    const char *operation;
    XrXirStatus status;
    XrXirCallStatus call_status;
    uint8_t *input;
    size_t input_length;
    unsigned mixed_mode;
    bool mixed_sealed;
} AllocationRun;

typedef struct AllocationMixedOwner {
    XrXirArtifact *lowered;
    XrXirCallEntry *actual, *entries;
    XrXirVmBinding *bindings;
    uint32_t count, test, exported;
    unsigned mode;
} AllocationMixedOwner;

/* The test drives one Program's two instances serially; observation never allocates. */
static AllocationMixedOwner *allocation_mixed_owner;
static size_t allocation_mixed_native, allocation_mixed_vm, allocation_mixed_crossings, allocation_mixed_releases;

static bool allocation_mixed_is_native(const AllocationMixedOwner *owner, uint32_t function) {
    return (function % 2 == 0) == (owner->mode == 2);
}
static XrXirAction allocation_mixed_resume(XrXirCallView *view) {
    AllocationMixedOwner *owner = allocation_mixed_owner;
    uint32_t function = xr_xir_call_current_entry(view->activation);
    CHECK(owner && function < owner->count);
    const XrXirCallEntry *actual = &owner->actual[function];
    CHECK(view->environment == actual->environment);
    if (allocation_mixed_is_native(owner, function)) ++allocation_mixed_native;
    else ++allocation_mixed_vm;
    /* Preserve the authenticated view and return the backend's exact action. */
    XrXirAction action = actual->resume(view);
    if (function == owner->test && action.kind == XR_XIR_ACTION_CALL && action.callee == owner->exported) {
        CHECK(allocation_mixed_is_native(owner, function) != allocation_mixed_is_native(owner, action.callee));
        ++allocation_mixed_crossings;
    }
    return action;
}
static void allocation_mixed_free(void *pointer) {
    AllocationMixedOwner *owner = pointer;
    if (allocation_mixed_owner == owner) allocation_mixed_owner = NULL;
    xr_xir_compile_artifact_free(owner->lowered);
    xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner->actual);
    xr_compile_resources_free(owner);
    ++allocation_mixed_releases;
}

#define OBSERVE(condition) do { if (!(condition)) { \
    fprintf(stderr, "source-allocation0-native check line=%d operation=%s condition=%s\n", \
        __LINE__, run->operation, #condition); return false; } } while (0)

static bool phase(AllocationRun *run, const char *name) {
    XrCompileResourceStats stats = {0};
    run->operation = name;
    OBSERVE(xr_compile_resources_stats(run->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(stats.live_bytes == instance_compile_bytes);
    printf("source-allocation0-native phase=%s sites=%zu allocations=%" PRIu64 " allocated=%" PRIu64
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
    printf("source-allocation0-native metadata stage=%u root=%u entry=%u initializer=%u answer=%u "
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
    if (io != XR_OS_IO_OK) fprintf(stderr, "source-allocation0-native input io-status=%u\n", (unsigned)io);
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
        size_t native_before = allocation_mixed_native, vm_before = allocation_mixed_vm;
        size_t crossings_before = allocation_mixed_crossings;
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
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            run->operation = "canonical-entry";
            run->call_status = xr_xir_instance_start(instance, run->roles.entry, NULL, 0);
            OBSERVE(run->call_status == XR_XIR_CALL_READY);
            if (!finish(run, instance, XR_XIR_I64, 0)) return false;
            printf("source-allocation0-native instance=%u repeat=%u canonical-entry=I64(0)\n", i, repeat);
            run->operation = "original-checkAnswer";
            run->call_status = xr_xir_instance_start_test(instance, run->roles.test);
            OBSERVE(run->call_status == XR_XIR_CALL_READY);
            if (!finish(run, instance, XR_XIR_UNIT, 0)) return false;
            printf("source-allocation0-native instance=%u repeat=%u original-checkAnswer=Unit exported-fixed-oracle=42\n",
                i, repeat);
        }
        OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
        if (run->mixed_mode) {
            OBSERVE(allocation_mixed_owner && !allocation_mixed_releases);
            OBSERVE(allocation_mixed_native > native_before && allocation_mixed_vm > vm_before);
            OBSERVE(allocation_mixed_crossings - crossings_before == 2);
            printf("source-allocation0-native mixed-executed mode=%u instance=%u native-resumes=%zu vm-resumes=%zu test-crossings=%zu\n",
                run->mixed_mode, i, allocation_mixed_native - native_before, allocation_mixed_vm - vm_before,
                allocation_mixed_crossings - crossings_before);
        }
    }
    return true;
}

static bool release(AllocationRun *run) {
    bool complete = true;
    xr_xir_value_drop(&run->value);
    xr_xir_compile_c_source_free(&run->repeated);
    xr_xir_compile_c_source_free(&run->emitted);
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->instances[i]) continue;
        XrXirCallStatus status = xr_xir_instance_free(run->instances[i]);
        if (status == XR_XIR_CALL_BUSY) complete = false;
        else { run->instances[i] = NULL; if (status != XR_XIR_CALL_READY) complete = false; }
        if (run->mixed_sealed && i == 0 && run->instances[1] && allocation_mixed_releases) complete = false;
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
    if (run->mixed_sealed && (allocation_mixed_owner || allocation_mixed_releases != 1)) complete = false;
    if (run->mixed_mode) printf("source-allocation0-native mixed-code-release count=%zu owner-live=%u\n",
        allocation_mixed_releases, (unsigned)(allocation_mixed_owner != NULL));
    printf("source-allocation0-native release compiler=%zu/%zu runtime=%zu/%zu table=%zu result=%s\n",
        instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes,
        runtime_owned_capacity, complete ? "PASS" : "FAIL");
    return complete;
}


static const char original_native_manifest[] =
    "[[export.c]]\nxray = \"exported\"\nsymbol = \"external_answer\"\n"
    "abi = \"native\"\nvisibility = \"hidden\"\nheader = true\n"
    "[[export.c]]\nxray = \"answer\"\nsymbol = \"external_entry\"\n";
_Static_assert(sizeof(original_source) - 1u == 119u, "Original Source0 bytes");
_Static_assert(sizeof(original_native_manifest) - 1u == 164u, "Original export responsibility bytes");

#if defined(XR_SOURCE_ALLOCATION0_NATIVE)
extern const XrXirProgramSpec source_allocation0_program;
extern const char source_allocation0_c_sha[65];
static const XrXirProgramSpec *native_spec(void) { return &source_allocation0_program; }
static const char *native_sha(void) { return source_allocation0_c_sha; }
#else
static const XrXirProgramSpec *native_spec(void) { return NULL; }
static const char *native_sha(void) { return NULL; }
#endif

static void hexadecimal(const uint8_t bytes[32], char output[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        output[i * 2] = digits[bytes[i] >> 4];
        output[i * 2 + 1] = digits[bytes[i] & 15u];
    }
    output[64] = '\0';
}

static bool original_literals(AllocationRun *run) {
    uint8_t digest[32]; char source[65], manifest[65];
    xr_sha256((const uint8_t *)original_source, sizeof(original_source) - 1u, digest);
    hexadecimal(digest, source);
    xr_sha256((const uint8_t *)original_native_manifest, sizeof(original_native_manifest) - 1u, digest);
    hexadecimal(digest, manifest);
    OBSERVE(!strcmp(source, "6c906c2d61178e524ddaa699383c3884bf0df6b2f98d2a3b9afbdda0c9d81774"));
    OBSERVE(!strcmp(manifest, "cf1a1e14d20fafca09a649f47ddcba036ea57a27e5febfeb5184518c05c45ac7"));
    printf("source-allocation0-native literals source=119:%s original-manifest=164:%s extern-exports=OPEN\n",
        source, manifest);
    return true;
}

static bool emit_owned(AllocationRun *run) {
    run->operation = "emit-first";
    run->status = xr_xir_compile_emit_c(run->lowered, "source_allocation0", 16777216u, &run->emitted);
    OBSERVE(run->status == XR_XIR_OK && run->emitted.text && run->emitted.length);
    if (!phase(run, "emission-first")) return false;
    run->operation = "emit-repeat";
    run->status = xr_xir_compile_emit_c(run->lowered, "source_allocation0", 16777216u, &run->repeated);
    OBSERVE(run->status == XR_XIR_OK && run->repeated.text && run->repeated.length);
    OBSERVE(run->emitted.text != run->repeated.text && run->emitted.length == run->repeated.length);
    OBSERVE(!memcmp(run->emitted.text, run->repeated.text, run->emitted.length));
    uint8_t digest[32];
    xr_sha256((const uint8_t *)run->emitted.text, run->emitted.length, digest);
    hexadecimal(digest, run->c_digest);
    printf("source-allocation0-native owned-C count=2 bytes=%zu identical=1 sha256=%s\n",
        run->emitted.length, run->c_digest);
    xr_xir_compile_c_source_free(&run->repeated);
    return phase(run, "emission-repeat-freed");
}

static bool write_translation_unit(AllocationRun *run, const char *output) {
    run->operation = "write-complete-native-TU";
    FILE *file = fopen(output, "wb");
    OBSERVE(file != NULL);
    bool written = fwrite(run->emitted.text, 1, run->emitted.length, file) == run->emitted.length;
    if (written) written = fprintf(file, "\nconst char source_allocation0_c_sha[65]=\"%s\";\n", run->c_digest) > 0;
    int closed = fclose(file);
    OBSERVE(written && closed == 0);
    printf("source-allocation0-native emitted-TU payload-sha=%s output=%s callback-ProgramSpec=1 extern-exports=OPEN\n",
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

static bool generated_correspondence(AllocationRun *run, const XrXirProgramSpec *spec) {
    const XrXirModule *module = xr_xir_compile_artifact_module(run->lowered);
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    OBSERVE(spec && module && module->declarations && proof.bytes && proof.identity && proof.layouts);
    OBSERVE(native_sha() && !strcmp(native_sha(), run->c_digest));
    OBSERVE(spec->abi_version == XR_XIR_PROGRAM_ABI_VERSION && spec->target.architecture == XR_XIR_ARCH_X86_64);
    OBSERVE(spec->target.abi_version == XR_XIR_VALUE_ABI_VERSION && spec->entry_count == module->function_count);
    OBSERVE(spec->entries && spec->declarations && !spec->code.owner && !spec->code.release);
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
    }
    printf("source-allocation0-native compiled-correspondence functions=%u all-native=1 proof-bytes=%zu layouts-exact=1 permissions-exact=1\n",
        spec->entry_count, proof.length);
    return true;
}

static XrXirStatus allocation_mixed_allocate(AllocationRun *run, size_t count, size_t size, void **output) {
    XrCompileResourceStatus status = xr_compile_resources_calloc(run->context.resources, count, size, output);
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
}
static bool seal_mixed_program(AllocationRun *run, const XrXirProgramSpec *native) {
    const XrXirModule *module = xr_xir_compile_artifact_module(run->lowered);
    OBSERVE(module && module->function_count == native->entry_count && !allocation_mixed_owner);
    OBSERVE(run->roles.test % 2 != run->roles.exported % 2);
    AllocationMixedOwner *owner = NULL;
    run->status = allocation_mixed_allocate(run, 1, sizeof(*owner), (void **)&owner);
    if (run->status != XR_XIR_OK) return false;
    owner->count = module->function_count; owner->mode = run->mixed_mode;
    owner->test = run->roles.test; owner->exported = run->roles.exported;
    owner->lowered = run->lowered; run->lowered = NULL;
    run->status = allocation_mixed_allocate(run, owner->count, sizeof(*owner->entries), (void **)&owner->entries);
    if (run->status != XR_XIR_OK) goto failed;
    run->status = allocation_mixed_allocate(run, owner->count, sizeof(*owner->actual), (void **)&owner->actual);
    if (run->status != XR_XIR_OK) goto failed;
    run->status = allocation_mixed_allocate(run, owner->count, sizeof(*owner->bindings), (void **)&owner->bindings);
    if (run->status != XR_XIR_OK) goto failed;
    unsigned native_count = 0, vm_count = 0;
    for (uint32_t f = 0; f < owner->count; ++f) {
        if (allocation_mixed_is_native(owner, f)) { owner->actual[f] = native->entries[f]; ++native_count; }
        else {
            run->status = xr_xir_compile_vm_bind(owner->lowered, f, &owner->bindings[f], &owner->actual[f]);
            if (run->status != XR_XIR_OK) goto failed;
            ++vm_count;
        }
        owner->entries[f] = owner->actual[f];
        owner->entries[f].resume = allocation_mixed_resume;
    }
    XrXirProgramSpec spec = *native;
    spec.entries = owner->entries;
    spec.declarations = module->declarations;
    spec.types = module->types;
    spec.proof = xr_xir_compile_program_proof(owner->lowered);
    spec.code = (XrXirCodeLease){owner, allocation_mixed_free};
    run->status = xr_xir_compile_program_seal(&run->context, &spec, &run->program);
    if (run->status != XR_XIR_OK) goto failed;
    allocation_mixed_owner = owner; run->mixed_sealed = true;
    printf("source-allocation0-native mixed-bound mode=%u native=%u vm=%u test=%u exported=%u direction=%s\n",
        run->mixed_mode, native_count, vm_count, owner->test, owner->exported,
        allocation_mixed_is_native(owner, owner->test) ? "native-to-VM" : "VM-to-native");
    return true;
failed:
    allocation_mixed_free(owner);
    return false;
}

static bool seal_native_instances(AllocationRun *run) {
    run->operation = "native-Program-seal";
    const XrXirProgramSpec *spec = native_spec();
    if (!generated_correspondence(run, spec)) return false;
    if (run->mixed_mode) {
        if (!seal_mixed_program(run, spec)) return false;
    } else run->status = xr_xir_compile_program_seal(&run->context, spec, &run->program);
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
    if (run->mixed_mode) OBSERVE(allocation_mixed_owner && !allocation_mixed_releases);
    return phase(run, "native-Program-caller-dropped-two-instances-retain");
}

int main(int argc, char **argv) {
    const char *output = NULL;
    unsigned mixed = 0;
    if (argc == 5 && !strcmp(argv[3], "--emit")) output = argv[4];
    else if (argc == 4 && native_spec() && !strcmp(argv[3], "--mixed-even")) mixed = 2;
    else if (argc == 4 && native_spec() && !strcmp(argv[3], "--mixed-odd")) mixed = 3;
    else if (argc != 3 || !native_spec()) return 2;
    instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    AllocationRun run = {0};
    run.mixed_mode = mixed;
    run.context.limits = xr_xir_compile_default_limits(); run.operation = "finite-owner";
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrCompileResourceStatus owner = xr_compile_resources_new(&limits, &run.context.resources);
    bool passed = owner == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = xr_compile_resources_stats(run.context.resources, &run.baseline) == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = original_literals(&run) && phase(&run, "owner-created") && produce(&run, argv[1], argv[2]);
    if (passed) passed = read_packets(&run, argv[2]) && detach_lower(&run) && emit_owned(&run);
    if (passed) passed = output ? write_translation_unit(&run, output) : seal_native_instances(&run) && execute_instances(&run);
    if (!passed) fprintf(stderr, "source-allocation0-native failure operation=%s owner-status=%u status=%u call-status=%u message=%s\n",
        run.operation, (unsigned)owner, (unsigned)run.status, (unsigned)run.call_status, run.diagnostic.source.message);
    bool released = release(&run);
    printf("source-allocation0-native mode=%s normal=%s compiler-sites=%zu runtime-sites=%zu allocated=%" PRIu64
        " peak=%" PRIu64 " work=%" PRIu64 " callback-native=%u extern-exports=OPEN FI=NOT_RUN axes=NOT_RUN source-physical-removal=OPEN image-unload=NOT_APPLICABLE\n",
        output ? "emit" : mixed == 2 ? "mixed-even" : mixed == 3 ? "mixed-odd" : "native",
        passed && released ? "PASS" : "FAIL", instance_compile_attempts,
        runtime_attempts, run.final.allocated_bytes, run.final.peak_bytes, run.final.work, output ? 0u : 1u);
    return passed && released ? 0 : 1;
}
