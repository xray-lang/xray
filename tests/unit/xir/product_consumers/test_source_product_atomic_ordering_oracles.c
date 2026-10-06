/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_atomic_ordering_oracles.c - Exact Atomic ordering and result oracles
 *
 * KEY CONCEPT:
 *   Same-module tests prove old and current values separately while preserving
 *   private entry permissions and the three typed ordering controls.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_types.h"
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

static const char source_0[] =
    "fn answer() -> i64 {\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.Relaxed)\n"
    " return old + counter.load(Ordering.SeqCst)\n"
    "}\n"
    "@test\n"
    "fn checkOrdering() {\n"
    " assert(answer() == 82)\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.Relaxed)\n"
    " const current = counter.load(Ordering.SeqCst)\n"
    " assert(old == 42)\n"
    " assert(current == 40)\n"
    "}\n";
static const char source_1[] =
    "fn answer() -> i64 {\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.Acquire)\n"
    " return old + counter.load(Ordering.SeqCst)\n"
    "}\n"
    "@test\n"
    "fn checkOrdering() {\n"
    " assert(answer() == 82)\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.Acquire)\n"
    " const current = counter.load(Ordering.SeqCst)\n"
    " assert(old == 42)\n"
    " assert(current == 40)\n"
    "}\n";
static const char source_2[] =
    "fn answer() -> i64 {\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.Release)\n"
    " return old + counter.load(Ordering.SeqCst)\n"
    "}\n"
    "@test\n"
    "fn checkOrdering() {\n"
    " assert(answer() == 82)\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.Release)\n"
    " const current = counter.load(Ordering.SeqCst)\n"
    " assert(old == 42)\n"
    " assert(current == 40)\n"
    "}\n";
static const char source_3[] =
    "fn answer() -> i64 {\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.AcquireRelease)\n"
    " return old + counter.load(Ordering.SeqCst)\n"
    "}\n"
    "@test\n"
    "fn checkOrdering() {\n"
    " assert(answer() == 82)\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.AcquireRelease)\n"
    " const current = counter.load(Ordering.SeqCst)\n"
    " assert(old == 42)\n"
    " assert(current == 40)\n"
    "}\n";
static const char source_4[] =
    "fn answer() -> i64 {\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.SeqCst)\n"
    " return old + counter.load(Ordering.SeqCst)\n"
    "}\n"
    "@test\n"
    "fn checkOrdering() {\n"
    " assert(answer() == 82)\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.SeqCst)\n"
    " const current = counter.load(Ordering.SeqCst)\n"
    " assert(old == 42)\n"
    " assert(current == 40)\n"
    "}\n";
static const char source_5[] =
    "fn answer() -> i64 {\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.Relaxed)\n"
    " const old = counter.fetchSub(2, Ordering.Relaxed)\n"
    " return old + counter.load(Ordering.Relaxed)\n"
    "}\n"
    "@test\n"
    "fn checkOrdering() {\n"
    " assert(answer() == 82)\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.Relaxed)\n"
    " const old = counter.fetchSub(2, Ordering.Relaxed)\n"
    " const current = counter.load(Ordering.Relaxed)\n"
    " assert(old == 42)\n"
    " assert(current == 40)\n"
    "}\n";
static const char source_6[] =
    "fn answer() -> i64 {\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.SeqCst)\n"
    " return old + counter.load(Ordering.SeqCst)\n"
    "}\n"
    "@test\n"
    "fn checkOrdering() {\n"
    " assert(answer() == 82)\n"
    " const counter = Atomic(40)\n"
    " counter.store(42, Ordering.SeqCst)\n"
    " const old = counter.fetchSub(2, Ordering.SeqCst)\n"
    " const current = counter.load(Ordering.SeqCst)\n"
    " assert(old == 42)\n"
    " assert(current == 40)\n"
    "}\n";

typedef struct AtomicFixture {
    const char *name, *source;
    size_t length;
    uint32_t store, ordering, load;
} AtomicFixture;

static const AtomicFixture fixtures[] = {
    {"rmw_Relaxed", source_0, sizeof(source_0) - 1, 4u, 0u, 4u},
    {"rmw_Acquire", source_1, sizeof(source_1) - 1, 4u, 1u, 4u},
    {"rmw_Release", source_2, sizeof(source_2) - 1, 4u, 2u, 4u},
    {"rmw_AcquireRelease", source_3, sizeof(source_3) - 1, 4u, 3u, 4u},
    {"rmw_SeqCst", source_4, sizeof(source_4) - 1, 4u, 4u, 4u},
    {"original_Relaxed", source_5, sizeof(source_5) - 1, 0u, 0u, 0u},
    {"original_SeqCst", source_6, sizeof(source_6) - 1, 4u, 4u, 4u}
};

static const AtomicFixture *select_fixture(const char *name) {
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i)
        if (!strcmp(name, fixtures[i].name)) return &fixtures[i];
    return NULL;
}

typedef struct AtomicRoles {
    uint32_t entry, initializer, answer, test;
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
    fprintf(stderr, "atomic-ordering check line=%d operation=%s condition=%s\n", \
        __LINE__, run->operation, #condition); return false; } } while (0)

static bool phase(AtomicRun *run, const char *name) {
    XrCompileResourceStats stats = {0};
    run->operation = name;
    OBSERVE(xr_compile_resources_stats(run->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    OBSERVE(stats.live_bytes == instance_compile_bytes);
    printf("atomic-ordering phase=%s sites=%zu allocations=%" PRIu64 " allocated=%" PRIu64
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

static bool ordering_value(AtomicRun *run, const XrXirModule *module,
                           const XrXirFunction *fn, uint32_t value, uint32_t expected) {
    OBSERVE(!fn->parameter_count && value < fn->instruction_count);
    const XrXirInstruction *some = &fn->instructions[value];
    OBSERVE(some->op == XR_XIR_NULLABLE_SOME);
    OBSERVE(xr_xir_type_is_nullable(module->types, some->type));
    XrXirType element = xr_xir_nullable_element(module->types, some->type);
    OBSERVE(xr_xir_nominal_native_ordering(module->types, element));
    OBSERVE(some->args[0] < fn->instruction_count);
    const XrXirInstruction *variant = &fn->instructions[some->args[0]];
    OBSERVE(variant->op == XR_XIR_ENUM_NEW && variant->type == element && !variant->args[1]);
    OBSERVE(variant->immediate == (int64_t)expected);
    return true;
}

static bool ordering_controls(AtomicRun *run, const XrXirModule *module,
                              const XrXirFunction *fn) {
    uint32_t counts[3] = {0};
    uint32_t expected[3] = {run->fixture->store, run->fixture->ordering, run->fixture->load};
    static const char *const names[] = {"STORE", "FETCH_SUB", "LOAD"};
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
        const XrXirInstruction *op = &fn->instructions[i];
        uint32_t slot;
        if (op->op == XR_XIR_ATOMIC_STORE) slot = 0;
        else if (op->op == XR_XIR_ATOMIC_FETCH_SUB) slot = 1;
        else if (op->op == XR_XIR_ATOMIC_LOAD) slot = 2;
        else continue;
        OBSERVE(op->args[1] == (slot == 2 ? 2u : 3u));
        OBSERVE(op->args[0] <= fn->operand_count && op->args[1] <= fn->operand_count - op->args[0]);
        uint32_t receiver = fn->operands[op->args[0]];
        XrXirType receiver_type = xr_xir_operand_type(fn, receiver);
        OBSERVE(xr_xir_type_is_atomic(module->types, receiver_type));
        OBSERVE(xr_xir_atomic_element(module->types, receiver_type) == XR_XIR_I64);
        OBSERVE(op->type == (slot == 0 ? XR_XIR_UNIT : XR_XIR_I64));
        uint32_t order = fn->operands[op->args[0] + op->args[1] - 1];
        if (!ordering_value(run, module, fn, order, expected[slot])) return false;
        OBSERVE(!counts[slot]++);
        printf("atomic-ordering control case=%s function=%.*s operation=%s ordinal=%u typed=Nullable<Ordering>\n",
            run->fixture->name, (int)fn->name_length, fn->name, names[slot], expected[slot]);
    }
    OBSERVE(counts[0] == 1 && counts[1] == 1 && counts[2] == 1);
    return true;
}

static bool inspect_module(AtomicRun *run, const XrXirArtifact *artifact,
                           XrXirStage stage, AtomicRoles *roles) {
    const XrXirModule *module = xr_xir_compile_artifact_module(artifact);
    const XrXirDeclarations *d = module ? module->declarations : NULL;
    OBSERVE(module && module->stage == stage && module->types && d && d->root_module < d->module_count);
    OBSERVE(d->functions);
    *roles = (AtomicRoles){d->entry_function, d->modules[d->root_module].initializer, UINT32_MAX, UINT32_MAX};
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *identity = &d->functions[f];
        if (identity->module != d->root_module) continue;
        if (named(fn, "answer")) {
            OBSERVE(roles->answer == UINT32_MAX && !identity->exported && !identity->test_role);
            OBSERVE(!fn->parameter_count && fn->result == XR_XIR_I64); roles->answer = f;
        } else if (named(fn, "checkOrdering")) {
            OBSERVE(roles->test == UINT32_MAX && !identity->exported);
            OBSERVE(identity->test_role == XR_XIR_TEST_ROLE_TEST && !identity->test_timeout_seconds);
            OBSERVE(!fn->parameter_count && fn->result == XR_XIR_UNIT); roles->test = f;
        }
    }
    OBSERVE(roles->answer != UINT32_MAX && roles->test != UINT32_MAX && roles->answer != roles->test);
    OBSERVE(roles->entry < module->function_count && roles->initializer < module->function_count);
    OBSERVE(roles->entry != roles->initializer && roles->entry != roles->answer && roles->entry != roles->test);
    OBSERVE(roles->initializer != roles->answer && roles->initializer != roles->test);
    OBSERVE(integer_return(&module->functions[roles->entry], 0));
    OBSERVE(module->functions[roles->initializer].result == XR_XIR_UNIT);
    OBSERVE(!d->functions[roles->initializer].exported && !d->functions[roles->initializer].test_role);
    if (stage == XR_XIR_CHECKED) {
        if (!ordering_controls(run, module, &module->functions[roles->answer])) return false;
        if (!ordering_controls(run, module, &module->functions[roles->test])) return false;
    }
    printf("atomic-ordering metadata stage=%u root=%u entry=%u initializer=%u answer=%u test=%u "
        "expected-entry=0 expected-answer=82 expected-old=42 expected-current=40 expected-test=Unit\n",
        (unsigned)stage, d->root_module, roles->entry, roles->initializer, roles->answer, roles->test);
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
    OBSERVE(tests->count == 1 && tests->entries);
    const XrXirSourceTestEntry *test = &tests->entries[0];
    OBSERVE(test->function == run->roles.test && test->role == XR_XIR_TEST_ROLE_TEST);
    OBSERVE(!test->timeout_seconds && test->name_length == strlen("checkOrdering"));
    OBSERVE(!memcmp(test->name, "checkOrdering", test->name_length));
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
    if (io != XR_OS_IO_OK) fprintf(stderr, "atomic-ordering input io-status=%u\n", (unsigned)io);
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
    AtomicRoles source_roles;
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
    bool reread_valid = run->status == XR_XIR_OK &&
        inspect_module(run, reread, XR_XIR_CHECKED, &reread_roles);
    if (reread_valid) reread_valid = run->roles.entry == reread_roles.entry &&
        run->roles.initializer == reread_roles.initializer && run->roles.answer == reread_roles.answer &&
        run->roles.test == reread_roles.test;
    xr_xir_compile_artifact_free(reread);
    OBSERVE(run->status == XR_XIR_OK && reread_valid);
    run->operation = "Lowered";
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    run->status = xr_xir_compile_lower(run->closed_checked, &target, &run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    AtomicRoles lowered_roles;
    if (!inspect_module(run, run->lowered, XR_XIR_LOWERED, &lowered_roles)) return false;
    OBSERVE(run->roles.entry == lowered_roles.entry && run->roles.initializer == lowered_roles.initializer);
    OBSERVE(run->roles.answer == lowered_roles.answer && run->roles.test == lowered_roles.test);
    run->status = xr_xir_compile_artifact_verify(run->lowered, &run->xir);
    OBSERVE(run->status == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&run->retained);
    xr_xir_compile_artifact_free(run->source_checked); run->source_checked = NULL;
    xr_xir_compile_artifact_free(run->closed_checked); run->closed_checked = NULL;
    return phase(run, "detached-Lowered-verified");
}

static bool seal_instances(AtomicRun *run) {
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
        OBSERVE(xr_xir_instance_start(instance, run->roles.test, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        OBSERVE(xr_xir_instance_start_test(instance, run->roles.answer) == XR_XIR_CALL_BAD_ARGUMENT);
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
        printf("atomic-ordering instance=%u canonical=I64(0) permission-denied=5 unchanged=1\n", i);
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            run->operation = "same-module-checkOrdering";
            run->call_status = xr_xir_instance_start_test(instance, run->roles.test);
            OBSERVE(run->call_status == XR_XIR_CALL_READY);
            if (!finish(run, instance, XR_XIR_UNIT, 0)) return false;
            printf("atomic-ordering case=%s instance=%u repeat=%u test=Unit answer=82 old=42 current=40\n",
                run->fixture->name, i, repeat);
        }
        OBSERVE(xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    }
    return true;
}

static bool release(AtomicRun *run) {
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
    printf("atomic-ordering release compiler=%zu/%zu runtime=%zu/%zu table=%zu result=%s\n",
        instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes,
        runtime_owned_capacity, complete ? "PASS" : "FAIL");
    return complete;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    const AtomicFixture *fixture = select_fixture(argv[1]);
    if (!fixture) return 2;
    instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    AtomicRun run = {0};
    run.fixture = fixture;
    run.context.limits = xr_xir_compile_default_limits();
    run.operation = "finite-owner";
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrCompileResourceStatus owner = xr_compile_resources_new(&limits, &run.context.resources);
    bool passed = owner == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = xr_compile_resources_stats(run.context.resources, &run.baseline) == XR_COMPILE_RESOURCE_OK;
    if (passed) passed = phase(&run, "owner-created") && produce(&run, argv[2], argv[3]);
    if (passed) passed = read_packets(&run, argv[3]) && detach_lower(&run) && seal_instances(&run) && execute_instances(&run);
    if (!passed) fprintf(stderr, "atomic-ordering failure operation=%s owner-status=%u status=%u "
        "source-stage=%u source-status=%u module=%u line=%d column=%d xir-status=%u "
        "function=%u block=%u instruction=%u reason=%u call-status=%u message=%s\n", run.operation,
        (unsigned)owner, (unsigned)run.status, (unsigned)run.diagnostic.stage,
        (unsigned)run.diagnostic.source.status, run.diagnostic.source.module, run.diagnostic.source.line,
        run.diagnostic.source.column, (unsigned)run.xir.status, run.xir.function, run.xir.block,
        run.xir.instruction, (unsigned)run.xir.reason, (unsigned)run.call_status, run.diagnostic.source.message);
    bool released = release(&run);
    printf("atomic-ordering case=%s normal=%s compiler-sites=%zu runtime-sites=%zu allocated=%" PRIu64
        " peak=%" PRIu64 " work=%" PRIu64 " whole-atomic-FI=NOT_RUN native=NOT_RUN\n",
        fixture->name, passed && released ? "PASS" : "FAIL", instance_compile_attempts, runtime_attempts,
        run.final.allocated_bytes, run.final.peak_bytes, run.final.work);
    return passed && released ? 0 : 1;
}
