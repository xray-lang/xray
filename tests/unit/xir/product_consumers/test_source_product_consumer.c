/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_consumer.c - Detached public source consumers
 *
 * KEY CONCEPT:
 *   Fixed source results survive producer destruction in independent backends.
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_vm.h"
#include "source_product_consumer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../xir_instance_compile_observer.h"
#include "../xir_runtime_allocations.h"
#include "array_default_source_shape.h"
#include "array_default_escape_shape.h"
#include "array_managed_escape_shape.h"
#include "array_element_place_shape.h"

static const SourceProductConsumerFixture *consumer_fixture;
#define XR_CONSUMER_NAME (consumer_fixture->name)
#define XR_CONSUMER_ROOT (consumer_fixture->root)
#define XR_CONSUMER_EXPECTED (consumer_fixture->expected)
#define product_consumer_program (*consumer_fixture->native_program)
#define product_consumer_c_sha (consumer_fixture->native_c_sha)

typedef struct Consumer {
    XrXirCompileContext context;
    XrCompileResourceStats baseline, stats;
    XrXirProgram *program;
    XrXirStatus status;
    uint32_t entry, answer, private_answer, state_probe, parameterized_answer;
    uint32_t array_entries[6];
    size_t attempts;
} Consumer;

static void consumer_stateful_shape(const XrXirModule *module, const Consumer *run);

typedef struct MixedOwner {
    XrXirArtifact *lowered;
    XrXirCallEntry *entries;
    XrXirVmBinding *bindings;
} MixedOwner;

static void mixed_free(void *pointer) {
    MixedOwner *owner = pointer;
    xr_xir_compile_artifact_free(owner->lowered);
    xr_compile_resources_free(owner->bindings);
    xr_compile_resources_free(owner->entries);
    xr_compile_resources_free(owner);
}

static XrXirStatus allocate(const XrXirCompileContext *context, size_t count, size_t size, void **out) {
    XrCompileResourceStatus status = xr_compile_resources_calloc(context->resources, count, size, out);
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
}

static void c_digest(const XrXirCSource *source, char text[65]) {
    uint8_t bytes[32];
    xr_sha256((const uint8_t *)source->text, source->length, bytes);
    for (unsigned i = 0; i < 32; ++i)
        CHECK(snprintf(text + i * 2, 3, "%02x", bytes[i]) == 2);
}

static XrCompileResourceStats stats(const Consumer *run) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(run->context.resources, &result) == XR_COMPILE_RESOURCE_OK);
    CHECK(result.live_bytes == instance_compile_bytes);
    return result;
}

#include "source_product_consumer_nominal.inc.c"
#include "source_product_consumer_initializers.inc.c"
#include "source_product_consumer_timers.inc.c"

static void closed_shape(const XrXirModule *module, uint32_t answer) {
    consumer_timer_shape(module, answer);
    if (consumer_initializer_case())
        consumer_initializer_shape(module);
    if (!strcmp(XR_CONSUMER_NAME, "constructor_folding"))
        constructor_shape(module);
    if (strcmp(XR_CONSUMER_NAME, "generics"))
        return;
    const XrXirFunction *function = &module->functions[answer];
    uint32_t integer = UINT32_MAX, boolean = UINT32_MAX, integers = 0, booleans = 0;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        const XrXirInstruction *instruction = &function->instructions[i];
        if (instruction->op != XR_XIR_CALL)
            continue;
        CHECK(instruction->immediate >= 0 && (uint64_t)instruction->immediate < module->function_count);
        uint32_t target = (uint32_t)instruction->immediate;
        const XrXirFunction *callee = &module->functions[target];
        CHECK(target != answer && callee->parameter_count == 1);
        CHECK(callee->parameters[0] == callee->result && instruction->type == callee->result);
        if (callee->result == XR_XIR_I64) {
            CHECK(integer == UINT32_MAX || integer == target);
            integer = target;
            ++integers;
        } else {
            CHECK(callee->result == XR_XIR_BOOL && boolean == UINT32_MAX);
            boolean = target;
            ++booleans;
        }
    }
    CHECK(integers == 2 && booleans == 1 && integer != boolean);
}

static XrXirStatus seal(Consumer *run, XrXirSourceProduct *product, XrXirArtifact *closed, unsigned mode) {
    if (!mode)
        return xr_xir_compile_source_product_vm_take(product, &run->program);
    CHECK(consumer_fixture->native_program);
    if (mode == 1)
        return xr_xir_compile_program_seal(&run->context, &product_consumer_program, &run->program);
    MixedOwner *owner = NULL;
    XrXirStatus status = allocate(&run->context, 1, sizeof(*owner), (void **)&owner);
    if (status != XR_XIR_OK)
        return status;
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(closed, &target, &owner->lowered, NULL);
    if (status != XR_XIR_OK)
        goto failed;
    const XrXirModule *module = xr_xir_compile_artifact_module(owner->lowered);
    if (!strcmp(XR_CONSUMER_NAME, "narrow_array")) consumer_stateful_shape(module, run);
    XrXirProgramProof proof = xr_xir_compile_program_proof(owner->lowered);
    CHECK(module->function_count == product_consumer_program.entry_count);
    CHECK(proof.length == product_consumer_program.proof.length);
    CHECK(!memcmp(proof.bytes, product_consumer_program.proof.bytes, proof.length));
    CHECK(!memcmp(proof.identity, product_consumer_program.proof.identity, 32));
    status = allocate(&run->context, module->function_count, sizeof(*owner->entries), (void **)&owner->entries);
    if (status != XR_XIR_OK)
        goto failed;
    status = allocate(&run->context, module->function_count, sizeof(*owner->bindings), (void **)&owner->bindings);
    if (status != XR_XIR_OK)
        goto failed;
    unsigned native = 0, vm = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if ((f % 2 == 0) == (mode == 2)) {
            owner->entries[f] = product_consumer_program.entries[f];
            ++native;
        } else {
            status = xr_xir_compile_vm_bind(owner->lowered, f, &owner->bindings[f], &owner->entries[f]);
            if (status != XR_XIR_OK)
                goto failed;
            ++vm;
        }
        if (!strcmp(XR_CONSUMER_NAME, "narrow_array"))
            CHECK(!owner->entries[f].flags && !owner->entries[f].cleanup_owner);
    }
    CHECK(native && vm);
    XrXirProgramSpec spec = product_consumer_program;
    spec.entries = owner->entries;
    spec.declarations = module->declarations;
    spec.types = module->types;
    spec.proof = proof;
    spec.code = (XrXirCodeLease){owner, mixed_free};
    status = xr_xir_compile_program_seal(&run->context, &spec, &run->program);
    if (status == XR_XIR_OK)
        return status;
failed:
    CHECK(!run->program);
    mixed_free(owner);
    return status;
}

static XrCompileResourceLimits compiler_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864), UINT64_C(16777216), UINT64_C(128000000)};
}

static Consumer build(unsigned mode, const char *output, size_t failure, XrCompileResourceLimits limits) {
    CHECK(!runtime_live && !runtime_bytes);
    instance_compile_zero();
    instance_compile_attempts = 0;
    instance_compile_fail_at = failure;
    instance_compile_injected = false;
    Consumer run = {0};
    run.status = XR_XIR_OUT_OF_MEMORY;
    run.context.limits = xr_xir_compile_default_limits();
    XrCompileResourceStatus opened = xr_compile_resources_new(&limits, &run.context.resources);
    if (opened != XR_COMPILE_RESOURCE_OK) {
        run.status = opened == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto done;
    }
    run.baseline = stats(&run);
    XrCompilerSession *session = NULL;
    XrXirSourceProduct *product = NULL;
    XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirArtifact *closed = NULL;
    XrXirCSource generated = {0};
    XrCompilerSessionStatus ss = xr_compile_session_new(run.context.resources, &session);
    if (ss != XR_COMPILER_SESSION_OK) {
        run.status = ss == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
        goto release;
    }
    const XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_CONSUMER_ROOT};
    const XrXirSourceProductRequest request = {
        {session, consumer_fixture->file, &authority, &run.context, consumer_stdlib_path(), NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    run.status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    if (run.status != XR_XIR_OK) {
        CHECK(!product);
        if (!instance_compile_injected)
            fprintf(stderr, "source status=%u stage=%u line=%d: %s\n", run.status,
                diagnostic.stage, diagnostic.source.line, diagnostic.source.message);
        goto release;
    }
    CHECK(xr_xir_compile_source_product_context(product)->resources == run.context.resources);
    consumer_timer_source(product);
    run.entry = xr_xir_compile_source_product_facts(product)->entry;
    XrXirSourceProductPacketView packet = {0};
    run.status = xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet);
    if (run.status != XR_XIR_OK)
        goto release;
    run.status = xr_xir_compile_checked_read(&run.context, packet.bytes, packet.length, &closed, NULL);
    if (run.status != XR_XIR_OK)
        goto release;
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    run.answer = run.private_answer = run.state_probe = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const char name[] = "consumerAnswer";
        if (fn->name_length == sizeof(name) - 1 && !memcmp(fn->name, name, sizeof(name) - 1)) {
            CHECK(run.answer == UINT32_MAX && !fn->parameter_count && fn->result == XR_XIR_I64);
            CHECK(module->declarations->functions[f].exported &&
                module->declarations->functions[f].module == module->declarations->root_module);
            run.answer = f;
        }
        if (!strcmp(XR_CONSUMER_NAME, "narrow_array") &&
            fn->name_length == 14 && !memcmp(fn->name, "consumerVisits", 14)) {
            CHECK(run.state_probe == UINT32_MAX && !fn->parameter_count && fn->result == XR_XIR_I64);
            CHECK(module->declarations->functions[f].exported &&
                module->declarations->functions[f].module == module->declarations->root_module);
            run.state_probe = f;
        }
        if (fn->name_length == 6 && !memcmp(fn->name, "answer", 6)) {
            CHECK(run.private_answer == UINT32_MAX && !module->declarations->functions[f].exported);
            run.private_answer = f;
        }
    }
    if (consumer_initializer_case()) {
        CHECK(run.answer == UINT32_MAX && run.private_answer == UINT32_MAX);
        run.answer = run.entry;
        run.private_answer = module->declarations->modules[module->declarations->root_module].initializer;
    } else {
        CHECK(run.answer != UINT32_MAX && run.private_answer != UINT32_MAX);
    }
    if (!strcmp(XR_CONSUMER_NAME, "narrow_array")) consumer_stateful_shape(module, &run);
    closed_shape(module, run.private_answer);
    run.parameterized_answer = !strcmp(XR_CONSUMER_NAME, "array_default_source") ?
        array_default_source_shape(module) : !strcmp(XR_CONSUMER_NAME, "array_default_escape") ?
        array_default_escape_shape(module) : !strcmp(XR_CONSUMER_NAME, "array_managed_escape") ?
        array_managed_escape_shape(module) : UINT32_MAX;
    if (!strcmp(XR_CONSUMER_NAME, "array_element_place")) array_element_place_shape(module, run.array_entries);
    /* Subsequent reads and execution own all data after the parse session dies. */
    xr_compile_session_free(session);
    session = NULL;
    run.status = xr_xir_compile_source_product_emit(product, "product_consumer", UINT64_C(16777216), &generated);
    if (run.status != XR_XIR_OK)
        goto release;
    CHECK(!strstr(generated.text, "TargetPlan"));
    XrXirCSource repeated = {0};
    run.status = xr_xir_compile_source_product_emit(product, "product_consumer", UINT64_C(16777216), &repeated);
    if (run.status == XR_XIR_OK)
        CHECK(generated.length == repeated.length && !memcmp(generated.text, repeated.text, generated.length));
    xr_xir_compile_c_source_free(&repeated);
    if (run.status != XR_XIR_OK)
        goto release;
    char digest[65];
    c_digest(&generated, digest);
    if (output) {
        FILE *file = fopen(output, "wb");
        CHECK(file && fwrite(generated.text, 1, generated.length, file) == generated.length);
        CHECK(fprintf(file, "\nconst char product_consumer_c_sha[65]=\"%s\";\n", digest) > 0);
        CHECK(!fclose(file));
    } else {
        if (consumer_fixture->native_program)
            CHECK(!strcmp(digest, product_consumer_c_sha));
        run.status = seal(&run, product, closed, mode);
    }
release:
    xr_xir_compile_c_source_free(&generated);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_xir_compile_source_product_free(product);
    if (closed && run.status == XR_XIR_OK)
        run.status = xr_xir_compile_artifact_verify(closed, NULL);
    xr_xir_compile_artifact_free(closed);
    xr_compile_session_free(session);
    if (run.status != XR_XIR_OK) {
        xr_xir_compile_program_drop(run.program);
        run.program = NULL;
    }
    run.stats = stats(&run);
done:
    run.attempts = instance_compile_attempts;
    instance_compile_fail_at = SIZE_MAX;
    return run;
}

static void release(Consumer *run) {
    xr_xir_compile_program_drop(run->program);
    if (run->context.resources) {
        CHECK(stats(run).live_bytes == run->baseline.live_bytes);
        xr_compile_resources_release(run->context.resources);
    }
    instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes);
}

/* A rejected build predicate still owns its caller ledger. Release that owner
 * before the assertion terminates the process, keeping the failed requirement. */
static void consumer_check_build(Consumer *run, bool accepted, const char *predicate) {
    if (accepted) return;
    release(run);
    instance_compile_report();
    fprintf(stderr, "consumer-build-guard case=%s actual=%u predicate=%s "
        "compiler-physical=0/0 runtime-physical=0/0 result=FAIL\n",
        XR_CONSUMER_NAME, run->status, predicate);
    CHECK(accepted);
}
#define CHECK_BUILT_RUN(run, condition) consumer_check_build(&(run), (condition), #condition)

#include "source_product_consumer_drive.inc.c"

static XrXirValue execute(XrXirInstance *instance, uint32_t entry, unsigned expected_resumes) {
    XrXirCallStatus status = xr_xir_instance_start(instance, entry, NULL, 0);
    if (status != XR_XIR_CALL_READY)
        fprintf(stderr, "start entry=%u status=%u\n", entry, status);
    CHECK(status == XR_XIR_CALL_READY);
    ConsumerCursor cursor = {0};
    size_t actions = 0;
    do {
        CHECK(++actions < 4096);
        status = consumer_advance(instance, &cursor, UINT64_C(1000000));
    } while (status == XR_XIR_CALL_READY);
    CHECK(status == XR_XIR_CALL_RETURNED && cursor.resumes == expected_resumes);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    if (expected_resumes)
        printf("coroutine-call entry=%u resumes=%zu actions=%zu value=%lld\n",
            entry, cursor.resumes, actions, (long long)(int64_t)value.payload);
    return value;
}

#include "source_product_consumer_output.inc.c"
#include "source_product_consumer_stateful.inc.c"
#include "source_product_consumer_array_default.inc.c"
#include "source_product_consumer_array_escape.inc.c"
#include "source_product_consumer_managed_escape.inc.c"
#include "source_product_consumer_element_place.inc.c"

static void normal(Consumer *run) {
    if (!strcmp(XR_CONSUMER_NAME, "array_element_place")) {
        consumer_element_place_normal(run);
        return;
    }
    if (!strcmp(XR_CONSUMER_NAME, "array_managed_escape")) {
        consumer_managed_escape_normal(run);
        return;
    }
    if (!strcmp(XR_CONSUMER_NAME, "array_default_escape")) {
        consumer_array_escape_normal(run);
        return;
    }
    if (!strcmp(XR_CONSUMER_NAME, "array_default_source")) {
        consumer_array_default_normal(run);
        return;
    }
    ConsumerOutput outputs[2] = {0};
    XrXirOutputSink sinks[2] = {0};
    XrXirInstance *instances[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config = consumer_config(&outputs[i], &sinks[i]);
        CHECK(xr_xir_instance_new(run->program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(run->program);
    run->program = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        size_t before = runtime_attempts;
        CHECK(xr_xir_instance_start(instances[i], run->private_answer, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_NEW && runtime_attempts == before);
        XrXirValue initialized = execute(instances[i], run->entry, 0);
        CHECK(initialized.type == XR_XIR_I64 && initialized.payload == 0);
        xr_xir_value_drop(&initialized);
        if (consumer_initializer_case())
            consumer_output_complete(&outputs[i]);
        for (unsigned repeat = 0; repeat < consumer_repeat_count(); ++repeat) {
            if (!consumer_initializer_case())
                consumer_output_reset(&outputs[i]);
            XrXirValue result = execute(instances[i], run->answer, consumer_wait_count());
            CHECK(result.type == XR_XIR_I64 && (int64_t)result.payload == XR_CONSUMER_EXPECTED);
            xr_xir_value_drop(&result);
            consumer_output_complete(&outputs[i]);
        }
        if (consumer_initializer_case())
            printf("initializer instance=%u groups=%zu bytes=%zu entry=0 repeats=%u\n",
                i, outputs[i].groups, outputs[i].bytes, consumer_repeat_count());
        if (consumer_stateful_case())
            consumer_stateful_finish(instances[i], run->answer, i);
        else
            CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    if (consumer_stateful_case())
        puts("two detached instances; two original fixed results; two repeated-call assertions; escaped panic owners released physically");
    else
        puts("two detached instances; four independent fixed results; final physical release");
}

static void compiler_axes(unsigned mode) {
    Consumer baseline = build(mode, NULL, SIZE_MAX, compiler_limits());
    CHECK_BUILT_RUN(baseline, baseline.status == XR_XIR_OK);
    XrCompileResourceStats measured = baseline.stats;
    release(&baseline);
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits limits = compiler_limits();
        uint64_t *boundary = !axis ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        *boundary = !axis ? measured.allocated_bytes : axis == 1 ? measured.peak_bytes : measured.work;
        CHECK(*boundary);
        Consumer exact = build(mode, NULL, SIZE_MAX, limits);
        CHECK_BUILT_RUN(exact, exact.status == XR_XIR_OK);
        release(&exact);
        --*boundary;
        Consumer refused = build(mode, NULL, SIZE_MAX, limits);
        if (refused.status != XR_XIR_BUDGET)
            fprintf(stderr, "axis=%u minus1 status=%u\n", axis, refused.status);
        CHECK_BUILT_RUN(refused, refused.status == XR_XIR_BUDGET && !refused.program);
        release(&refused);
    }
    printf("axes case=%s mode=%u allocated=%llu peak=%llu work=%llu boundaries=6 physical=0/0\n",
        XR_CONSUMER_NAME, mode, (unsigned long long)measured.allocated_bytes,
        (unsigned long long)measured.peak_bytes, (unsigned long long)measured.work);
}

static void compiler_shard(unsigned mode, size_t shard, size_t shards) {
    CHECK(shards && shard < shards);
    Consumer baseline = build(mode, NULL, SIZE_MAX, compiler_limits());
    CHECK_BUILT_RUN(baseline, baseline.status == XR_XIR_OK && baseline.attempts);
    size_t sites = baseline.attempts, covered = 0;
    release(&baseline);
    for (size_t ordinal = shard; ordinal < sites; ordinal += shards) {
        Consumer fault = build(mode, NULL, ordinal, compiler_limits());
        if (fault.status != XR_XIR_OUT_OF_MEMORY || !instance_compile_injected)
            fprintf(stderr, "compiler case=%s mode=%u ordinal=%zu/%zu attempts=%zu status=%u injected=%u\n",
                XR_CONSUMER_NAME, mode, ordinal, sites, fault.attempts, fault.status, instance_compile_injected);
        CHECK_BUILT_RUN(fault, instance_compile_injected && fault.status == XR_XIR_OUT_OF_MEMORY && !fault.program);
        CHECK_BUILT_RUN(fault, fault.attempts > ordinal);
        release(&fault);
        printf("compiler ordinal=%zu physical=0/0\n", ordinal);
        ++covered;
    }
    printf("compiler-summary case=%s mode=%u sites=%zu shard=%zu shards=%zu covered=%zu physical=0/0\n",
        XR_CONSUMER_NAME, mode, sites, shard, shards, covered);
}

#include "source_product_consumer_runtime.inc.c"

int xr_source_product_consumer_main(const SourceProductConsumerFixture *fixture, int argc, char **argv) {
    CHECK(fixture && !consumer_fixture && fixture->name && fixture->root && fixture->file);
    consumer_fixture = fixture;
    if (argc >= 3 && !strcmp(argv[2], "--cancel-prefix") && argc != 4) return 2;
    CHECK(argc >= 2 && argc <= 5);
    unsigned mode = (unsigned)strtoul(argv[1], NULL, 10);
    CHECK(mode <= 3);
    CHECK(!mode || fixture->native_program);
    if (argc == 4 && !strcmp(argv[2], "--cancel-prefix")) {
        size_t prefix = 0;
        if (!consumer_stateful_case() || !consumer_stateful_prefix_number(argv[3], &prefix)) return 2;
        consumer_stateful_cancel_prefixes(mode, prefix);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "--axes")) {
        compiler_axes(mode);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "--runtime-baseline")) {
        Consumer run = build(mode, NULL, SIZE_MAX, compiler_limits());
        CHECK_BUILT_RUN(run, run.status == XR_XIR_OK);
        RuntimeProbe probe = runtime_probe(&run, SIZE_MAX);
        CHECK(probe.status == XR_XIR_CALL_RETURNED);
        release(&run);
        printf("runtime-baseline case=%s mode=%u sites=%zu ticks=%zu physical=0/0\n",
            XR_CONSUMER_NAME, mode, probe.sites, probe.ticks);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "--runtime")) {
        runtime_faults(mode);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "--cancel")) {
        cancel_prefixes(mode);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "--output-status")) {
        output_statuses(mode);
        return 0;
    }
    if (argc == 5) {
        CHECK(!strcmp(argv[2], "--compiler-shard"));
        compiler_shard(mode, (size_t)strtoull(argv[3], NULL, 10), (size_t)strtoull(argv[4], NULL, 10));
        return 0;
    }
    if (argc == 4 && !strcmp(argv[2], "--runtime-site")) {
        size_t ordinal = (size_t)strtoull(argv[3], NULL, 10);
        Consumer run = build(mode, NULL, SIZE_MAX, compiler_limits());
        CHECK_BUILT_RUN(run, run.status == XR_XIR_OK);
        RuntimeProbe probe = runtime_probe(&run, ordinal);
        CHECK(probe.status == XR_XIR_CALL_OOM && probe.sites > ordinal);
        release(&run);
        printf("runtime-site case=%s mode=%u ordinal=%zu status=%u attempts=%zu physical=0/0\n",
            XR_CONSUMER_NAME, mode, ordinal, probe.status, probe.sites);
        return 0;
    }
    const char *emit = argc == 3 ? argv[2] : NULL;
    size_t failure = argc == 4 ? (size_t)strtoull(argv[3], NULL, 10) : SIZE_MAX;
    if (argc == 4)
        CHECK(!strcmp(argv[2], "--compiler-site"));
    Consumer run = build(mode, emit, failure, compiler_limits());
    if (failure != SIZE_MAX) {
        CHECK_BUILT_RUN(run, instance_compile_injected && run.status == XR_XIR_OUT_OF_MEMORY);
    } else {
        if (run.status != XR_XIR_OK) {
            CHECK(!run.program);
            release(&run);
            instance_compile_report();
            fprintf(stderr, "consumer-build-failure case=%s mode=%u actual=%u required=0 "
                "compiler-physical=0/0 runtime-physical=0/0 result=FAIL\n",
                XR_CONSUMER_NAME, mode, run.status);
            return 1;
        }
        if (!emit)
            normal(&run);
    }
    printf("consumer %s mode=%u status=%u compiler-sites=%zu allocated=%llu peak=%llu work=%llu\n",
        XR_CONSUMER_NAME, mode, run.status, run.attempts, (unsigned long long)run.stats.allocated_bytes,
        (unsigned long long)run.stats.peak_bytes, (unsigned long long)run.stats.work);
    release(&run);
    instance_compile_report();
    return 0;
}
