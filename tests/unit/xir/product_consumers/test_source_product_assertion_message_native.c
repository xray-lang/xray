/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_assertion_message_native.c - Native escaped assertion message ownership
 *
 * KEY CONCEPT:
 *   Complete source compiles to native callbacks through one checked pipeline.
 *   Independent panic message copies survive every execution and compiler owner.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include "base/xsha256.h"
#include "xir/xxir_types.h"
#include "xir/xxir_emit_c.h"
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

static const char assertion_source[] =
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
    "var message = \"assert \" + \"message\"\n"
    "assert(ok, message)\n"
    "return first.value + second.value\n"
    "}\n"
    "fn answer() -> i64 { return checked(false) + readCleanupCount() }\n"
    "@test\n"
    "fn checkFreshCleanup() { assert(readCleanupCount() == 0) }\n"
    "@test\n"
    "fn checkAssertionFailure() { answer() }\n"
    "@test\n"
    "fn checkFailureCleanup() { assert(readCleanupCount() == 22) }\n"
    "@test\n"
    "fn checkSuccessCleanup() { assert(checked(true) == 42); assert(readCleanupCount() == 11) }\n";

enum AssertionRole {
    ROLE_ANSWER, ROLE_READ, ROLE_CHECKED, ROLE_FRESH, ROLE_FAILURE,
    ROLE_FAILURE_CLEANUP, ROLE_SUCCESS, ROLE_COUNT
};
static const char *const role_names[ROLE_COUNT] = {
    "answer", "readCleanupCount", "checked", "checkFreshCleanup",
    "checkAssertionFailure", "checkFailureCleanup", "checkSuccessCleanup"
};
typedef struct AssertionRoles {
    uint32_t entry, initializer, functions[ROLE_COUNT];
} AssertionRoles;

typedef struct AssertionRun {
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
    AssertionRoles roles;
    XrXirCallResult escaped[4];
    unsigned escaped_count, canonical_count, Unit_count, permission_count;
    XrXirCSource emitted, repeated;
    char c_digest[65];
    const char *operation;
    XrXirStatus status;
    XrXirCallStatus call_status;
    uint8_t *input;
    size_t input_length;
} AssertionRun;

#define OBSERVE(condition) do { if (!(condition)) { \
    fprintf(stderr, "assertion-message-native check line=%d operation=%s condition=%s\n", \
        __LINE__, run->operation, #condition); return false; } } while (0)

static bool phase(AssertionRun *run, const char *name) {
    XrCompileResourceStats stats = {0};
    run->operation = name;
    OBSERVE(xr_compile_resources_stats(run->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(stats.live_bytes == instance_compile_bytes);
    printf("assertion-message-native phase=%s sites=%zu allocations=%" PRIu64 " allocated=%" PRIu64
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

static bool inspect_module(AssertionRun *run, const XrXirArtifact *artifact,
                           XrXirStage stage, AssertionRoles *roles) {
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
            OBSERVE(fn->parameter_count == (r == ROLE_CHECKED ? 1u : 0u));
            if (r == ROLE_CHECKED) OBSERVE(fn->parameters && fn->parameters[0] == XR_XIR_BOOL);
            uint32_t expected_role = r < ROLE_FRESH ? XR_XIR_TEST_ROLE_NONE : XR_XIR_TEST_ROLE_TEST;
            OBSERVE(identity->test_role == expected_role);
            OBSERVE(!identity->test_timeout_seconds);
            roles->functions[r] = f;
        }
    }
    OBSERVE(roles->entry < module->function_count && roles->initializer < module->function_count);
    OBSERVE(roles->entry != roles->initializer);
    for (unsigned r = 0; r < ROLE_COUNT; ++r) {
        OBSERVE(roles->functions[r] != UINT32_MAX);
        OBSERVE(roles->functions[r] != roles->entry && roles->functions[r] != roles->initializer);
        for (unsigned other = r + 1; other < ROLE_COUNT; ++other)
            OBSERVE(roles->functions[r] != roles->functions[other]);
    }
    uint32_t root_slots = 0;
    for (uint32_t s = 0; s < d->slot_count; ++s) {
        if (d->slots[s].module != d->root_module) continue;
        OBSERVE(d->slots[s].type == XR_XIR_I64 && d->slots[s].mutable == 1u);
        ++root_slots;
    }
    OBSERVE(root_slots == 1);
    OBSERVE(integer_return(&module->functions[roles->entry], 0));
    OBSERVE(module->functions[roles->initializer].result == XR_XIR_UNIT);
    OBSERVE(!d->functions[roles->initializer].exported && !d->functions[roles->initializer].test_role);
    printf("assertion-message-native metadata stage=%u root=%u entry=%u initializer=%u "
        "private-functions=3 tests=4 root-mutable-I64-slots=1 expected-entry=0\n",
        (unsigned)stage, d->root_module, roles->entry, roles->initializer);
    return true;
}

static bool inspect_product(AssertionRun *run, const char *file) {
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
    for (uint32_t t = 0; t < tests->count; ++t) {
        const char *name = role_names[ROLE_FRESH + t];
        uint32_t expected = run->roles.functions[ROLE_FRESH + t];
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

static bool produce(AssertionRun *run, const char *root, const char *file) {
    XrOsIoPolicy policy = xr_compile_io_policy(run->context.resources);
    run->operation = "original-input";
    XrOsIoStatus io = xr_os_io_read_regular_file(&policy, file, (sizeof(assertion_source) - 1),
        &run->input, &run->input_length);
    if (io != XR_OS_IO_OK) fprintf(stderr, "assertion-message-native input io-status=%u\n", (unsigned)io);
    OBSERVE(io == XR_OS_IO_OK && run->input_length == (sizeof(assertion_source) - 1));
    OBSERVE(!memcmp(run->input, assertion_source, run->input_length));
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

static bool read_packets(AssertionRun *run, const char *file) {
    XrXirSourceProductPacketView source = {0}, closed = {0};
    AssertionRoles source_roles = {0};
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

static bool detach_lower(AssertionRun *run) {
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
    AssertionRoles reread_roles = {0};
    bool same_roles = run->status == XR_XIR_OK && inspect_module(run, reread, XR_XIR_CHECKED, &reread_roles);
    if (same_roles) same_roles = !memcmp(&run->roles, &reread_roles, sizeof(reread_roles));
    xr_xir_compile_artifact_free(reread);
    OBSERVE(same_roles);
    OBSERVE(run->status == XR_XIR_OK);
    run->operation = "Lowered";
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run->status = xr_xir_compile_lower(run->closed_checked, &target, &run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    AssertionRoles lowered_roles = {0};
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

#if defined(XR_SOURCE_ASSERTION_MESSAGE_NATIVE)
extern const XrXirProgramSpec source_assertion_message_native_program;
extern const char source_assertion_message_native_c_sha[65];
static const XrXirProgramSpec *native_spec(void) { return &source_assertion_message_native_program; }
static const char *native_sha(void) { return source_assertion_message_native_c_sha; }
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

static bool emit_owned(AssertionRun *run) {
    run->operation = "emit-first";
    run->status = xr_xir_compile_emit_c(run->lowered, "source_assertion_message_native", 16777216u, &run->emitted);
    OBSERVE(run->status == XR_XIR_OK && run->emitted.text && run->emitted.length);
    if (!phase(run, "emission-first")) return false;
    run->operation = "emit-repeat";
    run->status = xr_xir_compile_emit_c(run->lowered, "source_assertion_message_native", 16777216u, &run->repeated);
    OBSERVE(run->status == XR_XIR_OK && run->repeated.text && run->repeated.length);
    OBSERVE(run->emitted.text != run->repeated.text && run->emitted.length == run->repeated.length);
    OBSERVE(!memcmp(run->emitted.text, run->repeated.text, run->emitted.length));
    uint8_t digest[32];
    xr_sha256((const uint8_t *)run->emitted.text, run->emitted.length, digest);
    hexadecimal(digest, run->c_digest);
    printf("assertion-message-native owned-C count=2 bytes=%zu identical=1 sha256=%s\n",
        run->emitted.length, run->c_digest);
    xr_xir_compile_c_source_free(&run->repeated);
    return phase(run, "emission-repeat-freed");
}

static bool write_translation_unit(AssertionRun *run, const char *output) {
    run->operation = "write-complete-native-TU";
    FILE *file = fopen(output, "wb");
    OBSERVE(file != NULL);
    bool written = fwrite(run->emitted.text, 1, run->emitted.length, file) == run->emitted.length;
    if (written) written = fprintf(file, "\nconst char source_assertion_message_native_c_sha[65]=\"%s\";\n", run->c_digest) > 0;
    int closed = fclose(file);
    OBSERVE(written && closed == 0);
    printf("assertion-message-native emitted-TU payload-sha=%s output=%s callback-ProgramSpec=1 extern-exports=OPEN\n",
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

static bool generated_correspondence(AssertionRun *run, const XrXirProgramSpec *spec) {
    const XrXirModule *module = xr_xir_compile_artifact_module(run->lowered);
    XrXirProgramProof proof = xr_xir_compile_program_proof(run->lowered);
    OBSERVE(spec && module && module->declarations && proof.bytes && proof.identity && proof.layouts);
    OBSERVE(native_sha() && !strcmp(native_sha(), run->c_digest));
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
    printf("assertion-message-native compiled-correspondence functions=%u all-native=1 proof-bytes=%zu layouts-exact=1 permissions-exact=1\n",
        spec->entry_count, proof.length);
    return true;
}

static bool seal_native_instances(AssertionRun *run) {
    run->operation = "native-Program-seal";
    const XrXirProgramSpec *spec = native_spec();
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

static bool poll_terminal(AssertionRun *run, XrXirInstance *instance, XrXirInstanceResult *result) {
    size_t polls = 0;
    do {
        OBSERVE(++polls <= 4096);
        *result = xr_xir_instance_poll_bounded(instance, UINT64_C(1000000));
    } while (result->outcome.status == XR_XIR_CALL_READY);
    run->call_status = result->outcome.status;
    return true;
}

static bool finish_return(AssertionRun *run, XrXirInstance *instance, XrXirType type, int64_t payload) {
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

static bool exact_message(const XrXirCallResult *result) {
    const char *bytes = NULL;
    size_t length = 0;
    return xr_xir_call_result_valid(result) && result->status == XR_XIR_CALL_ASSERTION &&
        result->panic.detail.code == XR_XIR_PANIC_ASSERTION && xr_xir_panic_valid(&result->panic) &&
        xr_xir_string_view(&result->panic.message, &bytes, &length) && length == 14u &&
        !memcmp(bytes, "assert message", 14);
}

static bool finish_failure(AssertionRun *run, XrXirInstance *instance, unsigned ordinal, unsigned repeat) {
    XrXirInstanceResult result = {0};
    if (!poll_terminal(run, instance, &result)) return false;
    OBSERVE(run->call_status == XR_XIR_CALL_ASSERTION && exact_message(&result.outcome));
    OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY && run->escaped_count < 4);
    XrXirCallResult *owned = &run->escaped[run->escaped_count];
    OBSERVE(xr_xir_call_result_copy(&result.outcome, owned) == XR_XIR_VALUE_OK);
    ++run->escaped_count;
    OBSERVE(exact_message(owned));
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
    printf("assertion-message-native instance=%u repeat=%u failure=ASSERTION detail=445 "
        "owned-message=assert-message bytes=14 state=READY output-slots=preserved\n", ordinal, repeat);
    return true;
}

static bool permissions(AssertionRun *run, XrXirInstance *instance, unsigned ordinal) {
    run->operation = "private-entry-authority";
    size_t attempts = runtime_attempts, compiler_attempts = instance_compile_attempts;
    unsigned permissions_before = run->permission_count;
    size_t blocks = runtime_live, bytes = runtime_bytes;
    XrCompileResourceStats before = {0}, after = {0};
    OBSERVE(xr_compile_resources_stats(run->context.resources, &before) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(xr_xir_instance_start(instance, run->roles.initializer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    ++run->permission_count;
    for (unsigned r = 0; r < ROLE_COUNT; ++r) {
        XrXirValue flag = {XR_XIR_BOOL, 0, 0};
        const XrXirValue *arguments = r == ROLE_CHECKED ? &flag : NULL;
        uint32_t count = r == ROLE_CHECKED ? 1u : 0u;
        OBSERVE(xr_xir_instance_start(instance, run->roles.functions[r], arguments, count) == XR_XIR_CALL_BAD_ARGUMENT);
        ++run->permission_count;
    }
    OBSERVE(xr_xir_instance_start_test(instance, run->roles.initializer) == XR_XIR_CALL_BAD_ARGUMENT);
    ++run->permission_count;
    for (unsigned r = ROLE_ANSWER; r <= ROLE_CHECKED; ++r) {
        OBSERVE(xr_xir_instance_start_test(instance, run->roles.functions[r]) == XR_XIR_CALL_BAD_ARGUMENT);
        ++run->permission_count;
    }
    OBSERVE(xr_compile_resources_stats(run->context.resources, &after) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(before.allocation_count == after.allocation_count && before.allocated_bytes == after.allocated_bytes);
    OBSERVE(before.live_bytes == after.live_bytes && before.peak_bytes == after.peak_bytes && before.work == after.work);
    OBSERVE(instance_compile_attempts == compiler_attempts && runtime_attempts == attempts);
    OBSERVE(runtime_live == blocks && runtime_bytes == bytes && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_NEW);
    OBSERVE(run->permission_count - permissions_before == 12u);
    printf("assertion-message-native instance=%u permissions=%u state=NEW ledger=unchanged physical=unchanged\n",
        ordinal, run->permission_count - permissions_before);
    return true;
}

static bool unit_test(AssertionRun *run, XrXirInstance *instance, unsigned role) {
    run->operation = role_names[role];
    run->call_status = xr_xir_instance_start_test(instance, run->roles.functions[role]);
    OBSERVE(run->call_status == XR_XIR_CALL_READY);
    return finish_return(run, instance, XR_XIR_UNIT, 0);
}

static bool execute_instances(AssertionRun *run) {
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = run->instances[i];
        if (!permissions(run, instance, i)) return false;
        run->operation = "canonical-entry";
        run->call_status = xr_xir_instance_start(instance, run->roles.entry, NULL, 0);
        OBSERVE(run->call_status == XR_XIR_CALL_READY);
        if (!finish_return(run, instance, XR_XIR_I64, 0)) return false;
        printf("assertion-message-native instance=%u canonical=I64 payload=0\n", i);
        if (!unit_test(run, instance, ROLE_FRESH)) return false;
        printf("assertion-message-native instance=%u fresh=Unit expected-cleanup=0\n", i);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            run->operation = "checkAssertionFailure";
            run->call_status = xr_xir_instance_start_test(instance, run->roles.functions[ROLE_FAILURE]);
            OBSERVE(run->call_status == XR_XIR_CALL_READY);
            if (!finish_failure(run, instance, i, repeat)) return false;
            if (!unit_test(run, instance, ROLE_FAILURE_CLEANUP)) return false;
            printf("assertion-message-native instance=%u repeat=%u failure-cleanup=Unit expected-cleanup=22\n", i, repeat);
            if (!unit_test(run, instance, ROLE_SUCCESS)) return false;
            printf("assertion-message-native instance=%u repeat=%u success=Unit expected-value=42 expected-cleanup=11\n", i, repeat);
        }
        OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    }
    OBSERVE(run->escaped_count == 4 && run->canonical_count == 2 && run->Unit_count == 10 &&
        run->permission_count == 24);
    return phase(run, "original-panic-success-and-cleanup-complete");
}

static bool release(AssertionRun *run) {
    bool complete = true;
    xr_xir_value_drop(&run->value);
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->instances[i]) continue;
        XrXirCallStatus status = xr_xir_instance_free(run->instances[i]);
        if (status == XR_XIR_CALL_BUSY) complete = false;
        else { run->instances[i] = NULL; if (status != XR_XIR_CALL_READY) complete = false; }
    }
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    xr_xir_compile_c_source_free(&run->emitted);
    xr_xir_compile_c_source_free(&run->repeated);
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
    printf("assertion-message-native compiler-owner-released compiler=%zu/%zu escaped-copies=%u\n",
        instance_compile_live, instance_compile_bytes, run->escaped_count);
    for (unsigned i = 0; i < run->escaped_count; ++i) {
        if (!exact_message(&run->escaped[i])) complete = false;
        else printf("assertion-message-native escaped=%u after-all-execution-owners=14B-exact\n", i);
        xr_xir_call_result_drop(&run->escaped[i]);
        if (!xr_xir_call_result_empty(&run->escaped[i])) complete = false;
    }
    if (runtime_live || runtime_bytes || runtime_owned || runtime_owned_capacity) complete = false;
    printf("assertion-message-native release compiler=%zu/%zu runtime=%zu/%zu table=%zu result=%s\n",
        instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes,
        runtime_owned_capacity, complete ? "PASS" : "FAIL");
    return complete;
}

_Static_assert(sizeof(assertion_source) - 1u == 726u, "Complete original source and private Tests");

int main(int argc, char **argv) {
    const char *output = NULL;
    if (argc == 5 && !strcmp(argv[3], "--emit")) output = argv[4];
    else if (argc != 3 || !native_spec()) return 2;
    instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    AssertionRun run = {0};
    run.context.limits = xr_xir_compile_default_limits();
    run.operation = "finite-owner";
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrCompileResourceStatus owner = xr_compile_resources_new(&limits, &run.context.resources);
    bool passed = owner == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = xr_compile_resources_stats(run.context.resources, &run.baseline) == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = phase(&run, "owner-created") && produce(&run, argv[1], argv[2]);
    if (passed) passed = read_packets(&run, argv[2]) && detach_lower(&run) && emit_owned(&run);
    if (passed) passed = output ? write_translation_unit(&run, output) :
        (seal_native_instances(&run) && execute_instances(&run));
    if (!passed) fprintf(stderr, "assertion-message-native failure operation=%s owner-status=%u status=%u "
        "source-stage=%u source-status=%u module=%u line=%d column=%d xir-status=%u "
        "function=%u block=%u instruction=%u reason=%u call-status=%u message=%s\n", run.operation,
        (unsigned)owner, (unsigned)run.status, (unsigned)run.diagnostic.stage,
        (unsigned)run.diagnostic.source.status, run.diagnostic.source.module, run.diagnostic.source.line,
        run.diagnostic.source.column, (unsigned)run.xir.status, run.xir.function, run.xir.block,
        run.xir.instruction, (unsigned)run.xir.reason, (unsigned)run.call_status, run.diagnostic.source.message);
    bool released = release(&run);
    printf("assertion-message-native mode=%s result=%s compiler-sites=%zu runtime-sites=%zu allocated=%" PRIu64
        " peak=%" PRIu64 " work=%" PRIu64 " canonical=%u Unit=%u owned-panic=%u permissions=%u "
        "callbacks=%s full-FI=NOT_RUN axes=NOT_RUN class-identities=OPEN original-stderr=OPEN\n",
        output ? "emit" : "native", passed && released ? "PASS" : "FAIL",
        instance_compile_attempts, runtime_attempts, run.final.allocated_bytes, run.final.peak_bytes, run.final.work,
        run.canonical_count, run.Unit_count, run.escaped_count, run.permission_count, output ? "NOT_EXECUTED" : "NATIVE");
    return passed && released ? 0 : 1;
}
