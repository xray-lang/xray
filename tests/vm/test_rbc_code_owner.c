/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_rbc_code_owner.c - Executable code leaves and independently retained layouts
 *
 * KEY CONCEPT:
 *   Real compiled programs run after their shells are freed. Allocation failure
 *   sweeps include both unsealed compilation and published image preparations.
 */
#include "../lir/lir_test_programs.h"
#include "base/xmalloc.h"
#include "vm/register/xrbc_lir.h"
#include "xray_native_string_value.h"

static size_t attempts, fail_at = SIZE_MAX, tracked_live;
static void *watched_program, *watched_layout;
static bool program_freed, layout_freed;
static struct { void *pointer; size_t bytes; } tracked[8192];
static size_t allocation_index(void *pointer) {
    for (size_t i = 0; i < 8192; ++i) if (tracked[i].pointer == pointer) return i;
    CHECK(false); return 0;
}
static void *allocate(size_t bytes) {
    if (++attempts == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    if (memory) {
        size_t index = allocation_index(NULL);
        tracked[index].pointer = memory; tracked[index].bytes = bytes; ++tracked_live;
    }
    return memory;
}
static void *allocate_zero(size_t count, size_t bytes) {
    if (bytes && count > SIZE_MAX / bytes) return NULL;
    void *memory = allocate(count * bytes);
    if (memory) memset(memory, 0, count * bytes);
    return memory;
}
static void release(void *memory) {
    if (!memory) return;
    size_t index = allocation_index(memory);
    tracked[index].pointer = NULL; tracked[index].bytes = 0; CHECK(tracked_live); --tracked_live;
    if (memory == watched_program) program_freed = true;
    if (memory == watched_layout) layout_freed = true;
    xr_free(memory);
}
static void *resize(void *memory, size_t bytes) {
    if (++attempts == fail_at) return NULL;
    size_t index = allocation_index(memory);
    void *fresh = xr_malloc(bytes);
    if (!fresh) return NULL;
    if (memory) { memcpy(fresh, memory, tracked[index].bytes < bytes ? tracked[index].bytes : bytes); xr_free(memory); }
    else ++tracked_live;
    tracked[index].pointer = fresh; tracked[index].bytes = bytes;
    return fresh;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) allocate(bytes)
#define xr_calloc(count, bytes) allocate_zero(count, bytes)
#define xr_realloc(memory, bytes) resize(memory, bytes)
#define xr_free(memory) release(memory)
#include "runtime/native/xnative_locations.c"
#include "runtime/native/xnative_module_state.c"
#include "vm/register/xrbc.c"
#include "vm/register/xrbc_lir.c"
#include "../lir/lir_fixed_bytecode_fixture.h"
#include "../lir/lir_array_c_fixture.h"
#include "../lir/lir_callable_fixture.h"
#include "../lir/lir_callable_factory_fixture.h"

static XrLirModule *fixture(unsigned kind, XrLirFunction *root) {
    if (!kind) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirFunction print = test_print_declaration(module);
        *root = test_function(module, "native_leaf", NULL, 0, XR_LIR_I64);
        XrLirBlock block = test_block(module, *root, NULL, 0);
        OK(xr_lir_set_entry(module, *root, block));
        XrLirValue value = test_constant(module, block, XR_LIR_I64, 7);
        XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_UNIT, .callee=print,
            .operands=&value, .operand_count=1};
        OK(xr_lir_emit_effect(module, block, &call));
        test_return(module, block, value);
        OK(xr_lir_module_finish(module, NULL));
        return module;
    }
    if (kind == 1) {
        XrLirModule *module = NULL;
        callable_factory_fixture_mode(&module, root, true, true);
        return module;
    }
    if (kind == 2) return fixed_bytecode_state_fixture(17, 0, root);
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    *root = test_function(module, "text", NULL, 0, XR_LIR_STRING);
    XrLirBlock block = test_block(module, *root, NULL, 0);
    OK(xr_lir_set_entry(module, *root, block));
    XrLirLiteral literal = {0};
    OK(xr_lir_add_literal(module, "\303\251\0x", 4, &literal));
    XrLirInstruction instruction = {.op=XR_LIR_STRING_LITERAL, .type=XR_LIR_STRING, .bits=literal.id};
    XrLirValue value;
    OK(xr_lir_emit_value(module, block, &instruction, &value));
    test_return(module, block, value);
    OK(xr_lir_module_finish(module, NULL));
    return module;
}
static XrRbcProgram *compile_fixture(unsigned kind) {
    XrLirFunction root;
    XrLirModule *module = fixture(kind, &root);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    return program;
}
static void check_result(XrRbcExecution **execution, unsigned kind) {
    XrRbcOutcome *outcome = xr_rbc_execution_run(*execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK);
    xr_rbc_execution_free(execution);
    if (kind == 3) {
        const XrtManagedHeader *string = NULL;
        CHECK(xr_rbc_outcome_string(outcome, &string));
        const XrtStringBacking *text = (const XrtStringBacking *)string;
        CHECK(text->length == 4 && !memcmp(text->bytes, "\303\251\0x", 4));
    } else {
        uint64_t value = UINT64_MAX;
        CHECK(xr_rbc_outcome_bits(outcome, &value));
        CHECK(value == (kind == 1 ? 137u : kind == 2 ? 128u : 7u));
    }
    xr_rbc_outcome_free(&outcome);
}
static void prepared_after_program(unsigned kind) {
    CHECK(!tracked_live);
    XrRbcProgram *program = compile_fixture(kind);
    if (!kind) CHECK(program->module.natives == program->image->natives && program->module.natives != program->natives);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    CHECK(execution->module != &program->module && execution->module == &execution->module_snapshot);
    watched_program = program; watched_layout = execution->layout;
    program_freed = layout_freed = false;
    if (watched_layout) CHECK(atomic_load(&execution->layout->references) == 2);
    xr_rbc_program_destroy(program);
    CHECK(program_freed && (!watched_layout || !layout_freed));
    check_result(&execution, kind);
    CHECK(!watched_layout || layout_freed);
    watched_program = watched_layout = NULL;
    CHECK(!tracked_live);
}
static void code_lease_is_leaf(void) {
    XrRbcProgram *program = compile_fixture(2);
    XrRbcCodeLease lease = program->module.code_lease;
    CHECK(lease.owner && lease.owner != program && lease.retain(lease.owner));
    XrRbcCodeImage *image = lease.owner;
    CHECK(image->module.state_spec == program->module.state_spec &&
        image->module.functions == program->module.functions);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    CHECK(execution->layout && execution->state);
    watched_program = program; watched_layout = execution->layout;
    program_freed = layout_freed = false;
    xr_rbc_program_destroy(program);
    CHECK(program_freed && !layout_freed && tracked_live && image->module.function_count);
    xr_rbc_execution_free(&execution);
    CHECK(layout_freed && tracked_live && atomic_load(&image->code.references) == 1);
    CHECK(image->module.functions[0].code_count && xrt_locations_count(image->locations));
    watched_program = watched_layout = NULL;
    lease.release(lease.owner);
    CHECK(!tracked_live);
}
static void raw_borrow_and_refusal(void) {
    XrRbcProgram *program = compile_fixture(2);
    size_t baseline = tracked_live;
    XrRbcExecution *execution = NULL;
    XrRbcCodeImage *image = program->image;
    atomic_store(&image->code.references, UINT32_MAX);
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_VALUE_FAILURE && !execution);
    atomic_store(&image->code.references, 1); CHECK(tracked_live == baseline);
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    atomic_store(&execution->layout->references, INT32_MAX);
    CHECK(!xrt_module_layout_retain(execution->layout));
    atomic_store(&execution->layout->references, 2);
    xr_rbc_execution_free(&execution);
    CHECK(tracked_live == baseline && atomic_load(&image->code.references) == 1);
    XrRbcModule raw = program->module;
    raw.code_lease = (XrRbcCodeLease){0};
    CHECK(xr_rbc_module_prepare(&raw, 0, NULL, 0, program->locations, &execution) == XR_RBC_OK);
    CHECK(!execution->code_lease.owner && execution->module != &raw);
    check_result(&execution, 2);
    CHECK(tracked_live == baseline);
    xr_rbc_program_destroy(program); CHECK(!tracked_live);
}
static void failure_sweep(unsigned kind) {
    XrLirFunction root;
    XrLirModule *module = fixture(kind, &root);
    XrRbcProgram *program = NULL;
    attempts = 0;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    size_t compile_calls = attempts;
    xr_rbc_program_destroy(program); program = NULL; CHECK(!tracked_live);
    for (size_t point = 1; point <= compile_calls; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !tracked_live);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    size_t baseline = tracked_live;
    XrRbcExecution *execution = NULL;
    attempts = 0;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    size_t prepare_calls = attempts;
    attempts = 0;
    check_result(&execution, kind); CHECK(tracked_live == baseline);
    size_t run_calls = attempts;
    for (size_t point = 1; point <= prepare_calls; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OUT_OF_MEMORY && !execution);
        CHECK(tracked_live == baseline);
    }
    fail_at = SIZE_MAX;
    for (size_t point = 1; point <= run_calls; ++point) {
        CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
        attempts = 0; fail_at = point;
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        CHECK(outcome);
        XrRbcStatus status = xr_rbc_outcome_status(outcome);
        if (status == XR_RBC_VALUE_FAILURE) {
            XrXirValueStatus value_status; uint32_t source; const XrtLocationOwner *locations;
            CHECK(xr_rbc_outcome_value_failure(outcome, &value_status, &source, &locations) && value_status == XR_XIR_VALUE_OOM);
        } else CHECK(status == XR_RBC_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
        CHECK(tracked_live == baseline);
    }
    xr_rbc_program_destroy(program); CHECK(!tracked_live);
    printf("Code owner fixture %u: compile/prepare/run OOM sites %zu/%zu/%zu refunded\n", kind, compile_calls, prepare_calls, run_calls);
}
int main(void) {
    for (unsigned kind = 0; kind < 4; ++kind) { prepared_after_program(kind); failure_sweep(kind); }
    code_lease_is_leaf(); raw_borrow_and_refusal();
    puts("CodeImage leases, independent Layout snapshots and post-Program executions passed");
    return 0;
}
