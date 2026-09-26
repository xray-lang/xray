/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_allocations.c - Allocation failure and physical release witnesses
 *
 * KEY CONCEPT:
 *   Compile the actual implementation against counted allocator wrappers and
 *   fail each allocation in turn, including post-transition verification.
 */

#include "base/xmalloc.h"
#include "xir/xxir.h"
#include <stdlib.h>

static const XrXirTarget fixture_target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "allocation check failed at line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static size_t calls, live, fail_at = SIZE_MAX;

static void *counted_malloc(size_t size) {
    if (calls++ == fail_at)
        return NULL;
    void *pointer = xr_malloc(size);
    if (pointer)
        ++live;
    return pointer;
}

static void *counted_calloc(size_t count, size_t size) {
    if (calls++ == fail_at)
        return NULL;
    void *pointer = xr_calloc(count, size);
    if (pointer)
        ++live;
    return pointer;
}

static void counted_free(void *pointer) {
    if (pointer) {
        CHECK(live > 0);
        --live;
    }
    xr_free(pointer);
}

static void *counted_realloc(void *pointer, size_t size) {
    if (calls++ == fail_at)
        return NULL;
    bool was_null = pointer == NULL;
    void *replacement = xr_realloc(pointer, size);
    if (replacement && was_null)
        ++live;
    return replacement;
}

#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(size) counted_malloc(size)
#define xr_calloc(count, size) counted_calloc(count, size)
#define xr_free(pointer) counted_free(pointer)
#define xr_realloc(pointer, size) counted_realloc(pointer, size)

#include "xir/xxir_generic.c"
#include "xir/xxir.c"
#include "xir/xxir_declarations.c"
#include "xir/xxir_verify.c"
#include "xir/xxir_layout.c"
#include "xir/xxir_value.c"
#include "xir/xxir_scalar.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_emit_c.c"
#include "xir/xxir_call.c"
#include "xir/xxir_program.c"
#include "xir/xxir_instance.c"
#include "xir/xxir_output.c"

#include "xir_string_fixture.h"
#include "xir_output_fixture.h"

static bool allocation_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    size_t *published = context;
    CHECK(stream == XR_XIR_STDERR && length == 1 && bytes[0] == 'x');
    ++*published; return true;
}
static size_t write_allocation_failures(void) {
    XrXirArtifact *artifact = output_fixture();
    XrXirCallEntry entry; XrXirVmBinding binding;
    CHECK(xr_xir_vm_bind(artifact, 1, &binding, &entry) == XR_XIR_OK);
    XrXirDomain *domain = NULL; XrXirValue argument = {0};
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "x", 1, &argument) == XR_XIR_VALUE_OK);
    size_t baseline = live, sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        size_t published = 0;
        XrXirOutputSink sink = {allocation_bytes, &published, 65536};
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {&entry, 1, NULL, 65536, 100, 10, &accounting, {xr_xir_output_render, &sink}};
        XrXirCall *call = NULL;
        calls = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
        XrXirCallStatus status = xr_xir_call_new(&config, 0, &argument, 1, &call);
        if (status == XR_XIR_CALL_READY) {
            XrXirCallResult result = xr_xir_call_poll(call);
            CHECK(result.status == XR_XIR_CALL_RETURNED && result.value.type == XR_XIR_BOOL);
            CHECK(result.value.payload == (attempt ? 0 : 1));
        } else CHECK(attempt && status == XR_XIR_CALL_OOM && !call);
        if (!attempt) sites = calls;
        CHECK(published == (attempt ? 0u : 1u));
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees && live == baseline);
    }
    fail_at = SIZE_MAX;
    xr_xir_value_drop(&argument); xr_xir_domain_drop(domain); xr_xir_artifact_free(artifact);
    CHECK(!live); return sites;
}

static bool allocation_output(void *context, const XrXirOutputGroup *group) {
    CHECK(group && !group->line && group->count == 1);
    XrXirOutputStream stream = group->stream;
    const XrXirValue *value = &group->values[0];
    (void) context; (void) stream;
    return value->type == XR_XIR_STRING;
}
static void managed_allocation_run(XrXirArtifact *artifact) {
    XrXirDomain *domain = NULL;
    XrXirValue arguments[2] = {{0}, {0}}, owned = {0};
    XrXirCall *call = NULL;
    XrXirCallEntry entries[3];
    XrXirVmBinding bindings[3];
    XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {entries, 3, NULL, 65536, 100, 10, &accounting, {allocation_output, NULL}};
    for (uint32_t i = 0; i < 3; ++i) {
        XrXirStatus status = xr_xir_vm_bind(artifact, i, &bindings[i], &entries[i]);
        if (status != XR_XIR_OK) { CHECK(status == XR_XIR_OUT_OF_MEMORY); goto done; }
    }
    XrXirValueStatus status = xr_xir_domain_new(65536, &domain);
    if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_OOM); goto done; }
    for (uint32_t i = 0; i < 2; ++i) {
        status = xr_xir_string_new(domain, "x", 1, &arguments[i]);
        if (status != XR_XIR_VALUE_OK) { CHECK(status == XR_XIR_VALUE_OOM); goto done; }
    }
    XrXirCallStatus admitted = xr_xir_call_new(&config, 0, arguments, 2, &call);
    if (admitted != XR_XIR_CALL_READY) { CHECK(admitted == XR_XIR_CALL_OOM); goto done; }
    XrXirCallResult result = xr_xir_call_poll(call);
    if (result.status == XR_XIR_CALL_SUSPENDED) {
        CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_READY);
        result = xr_xir_call_poll(call);
    }
    if (result.status == XR_XIR_CALL_RETURNED)
        CHECK(xr_xir_call_take_result(call, &owned) == XR_XIR_CALL_RETURNED);
    else CHECK(result.status == XR_XIR_CALL_OOM);
 done:
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
    xr_xir_domain_drop(domain);
    xr_xir_value_drop(&arguments[0]); xr_xir_value_drop(&arguments[1]); xr_xir_value_drop(&owned);
}
static size_t managed_allocation_failures(void) {
    fail_at = SIZE_MAX;
    XrXirArtifact *artifact = string_fixture(0);
    size_t baseline = live;
    calls = 0;
    managed_allocation_run(artifact);
    size_t sites = calls;
    CHECK(live == baseline);
    for (size_t i = 0; i < sites; ++i) {
        fail_at = i; calls = 0;
        managed_allocation_run(artifact);
        CHECK(live == baseline);
    }
    fail_at = SIZE_MAX;
    xr_xir_artifact_free(artifact);
    CHECK(!live);
    return sites;
}

typedef struct AllocationFrame { bool entered; XrXirValue argument; } AllocationFrame;
static XrXirAction allocation_resume(XrXirCallView *view) {
    AllocationFrame *frame = view->state;
    if (!frame->entered && view->arguments[0].payload) {
        frame->entered = true;
        frame->argument = (XrXirValue) {XR_XIR_I64, 0, view->arguments[0].payload - 1};
        return (XrXirAction) {XR_XIR_ACTION_CALL, 0, &frame->argument, 1, {0, 0, 0}};
    }
    return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, {XR_XIR_I64, 0,
        frame->entered ? view->inbox.value.payload + 1 : 0}};
}

static size_t call_allocation_failures(void) {
    XrXirType type = XR_XIR_I64;
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, &type, 1, XR_XIR_I64, sizeof(AllocationFrame), allocation_resume, NULL, NULL};
    size_t expected_calls = 0;
    for (size_t attempt = 0; attempt <= expected_calls; ++attempt) {
        fail_at = attempt ? attempt - 1 : SIZE_MAX;
        calls = 0;
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {&entry, 1, NULL, 65536, 100, 10, &accounting, {NULL, NULL}};
        XrXirValue argument = {XR_XIR_I64, 0, 3};
        XrXirCall *call = NULL;
        XrXirCallStatus status = xr_xir_call_new(&config, 0, &argument, 1, &call);
        if (status == XR_XIR_CALL_READY) {
            XrXirCallResult result = xr_xir_call_poll(call);
            CHECK(result.status == (attempt ? XR_XIR_CALL_OOM : XR_XIR_CALL_RETURNED));
            if (!attempt) CHECK(result.value.payload == 3);
            CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        } else CHECK(attempt && status == XR_XIR_CALL_OOM && !call);
        if (!attempt) expected_calls = calls;
        CHECK(live == 0 && accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
    }
    fail_at = SIZE_MAX;
    return expected_calls;
}

#include "xir_program_fixture.h"
static bool program_allocation_output(void *context, const XrXirOutputGroup *group) {
    CHECK(group && !group->line && group->count == 1);
    XrXirOutputStream stream = group->stream;
    const XrXirValue *value = &group->values[0];
    (void) context; (void) stream; (void) value; return true;
}
static size_t program_allocation_failures(void) {
    size_t expected_calls = 0;
    for (size_t attempt = 0; attempt <= expected_calls; ++attempt) {
        fail_at = SIZE_MAX;
        XrXirArtifact *artifact = program_fixture(0), *original = artifact;
        XrXirProgram *program = NULL;
        calls = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
        XrXirStatus sealed = xr_xir_vm_program_take(&artifact, 65536, &program);
        if (sealed != XR_XIR_OK) {
            CHECK(attempt && sealed == XR_XIR_OUT_OF_MEMORY && !program && artifact == original);
        } else {
            CHECK(!artifact);
            XrXirInstanceConfig config = xr_xir_instance_defaults();
            config.output.write = program_allocation_output;
            XrXirInstance *instance = NULL;
            XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
            if (status != XR_XIR_CALL_READY) CHECK(attempt && status == XR_XIR_CALL_OOM && !instance);
            else {
                status = xr_xir_instance_start(instance, 3, NULL, 0);
                if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll(instance).outcome.status;
                CHECK(status == (attempt ? XR_XIR_CALL_OOM : XR_XIR_CALL_RETURNED));
                if (xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED)
                    CHECK(xr_xir_instance_start(instance, 3, NULL, 0) == XR_XIR_CALL_OOM);
                CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
            }
            xr_xir_program_drop(program);
        }
        if (!attempt) expected_calls = calls;
        xr_xir_artifact_free(artifact);
        CHECK(live == 0);
    }
    fail_at = SIZE_MAX;
    return expected_calls;
}
static size_t declaration_allocation_failures(void) {
    XrXirArtifact *fixture = program_fixture(0);
    size_t baseline = live, total = 0;
    for (uint32_t phase = 0; phase < 3; ++phase) {
        size_t expected_calls = 0;
        for (size_t attempt = 0; attempt <= expected_calls; ++attempt) {
            calls = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
            XrXirArtifact *output = NULL;
            XrXirCSource source = {0};
            XrXirModule module = *xr_xir_artifact_module(fixture);
            module.stage = XR_XIR_BUILT;
            XrXirStatus status;
            if (phase == 0) status = xr_xir_check(&module, NULL, &output, NULL);
            else if (phase == 1) {
                fixture->module.stage = XR_XIR_CHECKED;
                status = xr_xir_lower(fixture, &fixture_target, NULL, &output, NULL);
                fixture->module.stage = XR_XIR_LOWERED;
            } else status = xr_xir_emit_c(fixture, "owned_program", 200000, &source);
            CHECK(status == (attempt ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK));
            if (!attempt) expected_calls = calls;
            else CHECK(!output && !source.text);
            xr_xir_artifact_free(output); xr_xir_c_source_free(&source);
            CHECK(live == baseline);
        }
        fail_at = SIZE_MAX; total += expected_calls;
    }
    xr_xir_artifact_free(fixture); CHECK(live == 0);
    return total;
}

int main(void) {
    XrXirInstruction ops[] = {
        {XR_XIR_CONST_I64, XR_XIR_I64, {0, 0}, {0, 0}, 42},
        {XR_XIR_COPY, XR_XIR_I64, {0, 0}, {0, 0}, 0},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0, 0}, 0},
    };
    XrXirBlock block = {0, 3};
    XrXirType parameter = XR_XIR_I64;
    XrXirFunction functions[] = {
        {"first", 5, NULL, 0, XR_XIR_I64, &block, 1, ops, 3, NULL, 0},
        {"second", 6, &parameter, 1, XR_XIR_I64, &block, 1, ops, 3, NULL, 0},
    };
    XrXirModule module = {XR_XIR_BUILT, functions, 2, NULL, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    XrXirBudget exact = xr_xir_default_budget();
    exact.metadata_bytes = sizeof(XrXirArtifact);
    for (size_t i = 0; i < 2; ++i)
        exact.metadata_bytes += sizeof(XrXirFunction) + functions[i].name_length +
            functions[i].parameter_count * sizeof(XrXirType) +
            sizeof(XrXirBlock) + 3 * sizeof(XrXirInstruction);
    --exact.metadata_bytes;
    CHECK(xr_xir_check(&module, &exact, &checked, NULL) == XR_XIR_BUDGET);
    CHECK(!checked && live == 0);
    ++exact.metadata_bytes;
    CHECK(xr_xir_check(&module, &exact, &checked, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    CHECK(live == 0);
    calls = 0;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    size_t check_calls = calls;
    CHECK(live > 0);
    xr_xir_artifact_free(checked);
    CHECK(live == 0);
    for (size_t i = 0; i < check_calls; ++i) {
        calls = 0;
        fail_at = i;
        checked = NULL;
        CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(checked == NULL);
        CHECK(live == 0);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_check(&module, NULL, &checked, NULL) == XR_XIR_OK);
    size_t checked_live = live;
    calls = 0;
    CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OK);
    size_t lower_calls = calls;
    xr_xir_artifact_free(lowered);
    CHECK(live == checked_live);
    for (size_t i = 0; i < lower_calls; ++i) {
        calls = 0;
        fail_at = i;
        lowered = NULL;
        CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(lowered == NULL);
        CHECK(live == checked_live);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_verify(xr_xir_artifact_module(checked), NULL, NULL) == XR_XIR_OK);
    CHECK(xr_xir_lower(checked, &fixture_target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_artifact_free(checked);
    size_t lowered_live = live;
    calls = 0;
    XrXirRunContext context = {3, 16, 0, 0, 0, 0};
    XrXirValue result;
    CHECK(xr_xir_vm_run(lowered, 0, &context, NULL, 0, &result) == XR_XIR_RUN_OK);
    CHECK(result.payload == 42 && live == lowered_live);
    size_t run_calls = calls;
    for (size_t i = 0; i < run_calls; ++i) {
        calls = 0;
        fail_at = i;
        context = (XrXirRunContext) {3, 16, 0, 0, 0, 0};
        result = (XrXirValue) {99, 99, 99};
        CHECK(xr_xir_vm_run(lowered, 0, &context, NULL, 0, &result) == XR_XIR_RUN_OUT_OF_MEMORY);
        CHECK(result.type == 0 && result.reserved == 0 && result.payload == 0);
        CHECK(context.live_bytes == 0 && context.allocations == context.frees);
        CHECK(live == lowered_live);
    }
    fail_at = SIZE_MAX;
    calls = 0;
    XrXirCSource source;
    CHECK(xr_xir_emit_leaf_c(lowered, "allocation", 65536, &source) == XR_XIR_OK);
    size_t emit_calls = calls;
    xr_xir_c_source_free(&source);
    CHECK(live == lowered_live);
    for (size_t i = 0; i < emit_calls; ++i) {
        calls = 0;
        fail_at = i;
        CHECK(xr_xir_emit_leaf_c(lowered, "allocation", 65536, &source) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!source.text && !source.length && live == lowered_live);
    }
    fail_at = SIZE_MAX;
    calls = 0;
    CHECK(xr_xir_emit_c(lowered, "resumable", 65536, &source) == XR_XIR_OK);
    size_t resume_emit_calls = calls;
    xr_xir_c_source_free(&source);
    CHECK(live == lowered_live);
    for (size_t i = 0; i < resume_emit_calls; ++i) {
        calls = 0;
        fail_at = i;
        CHECK(xr_xir_emit_c(lowered, "resumable", 65536, &source) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!source.text && !source.length && live == lowered_live);
    }
    fail_at = SIZE_MAX;
    xr_xir_artifact_free(lowered);
    CHECK(live == 0);
    size_t call_sites = call_allocation_failures();
    printf("XIR physical release passed at %zu check, %zu lower, %zu VM, %zu emit allocation sites\n",
           check_calls, lower_calls, run_calls, emit_calls);
    printf("Resumable physical release passed at %zu emitter and %zu activation/frame allocation sites\n",
           resume_emit_calls, call_sites);
    printf("Managed VM admission and execution physical release: %zu allocation sites\n", managed_allocation_failures());
    printf("Declaration stage/emission physical release: %zu allocation sites\n", declaration_allocation_failures());
    printf("Program seal, instance and initialization physical release: %zu allocation sites\n", program_allocation_failures());
    printf("Write-result activation and renderer physical release: %zu allocation sites\n", write_allocation_failures());
    return 0;
}
