/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_bytecode_alloc.c - Relocating compilation and representative OOMs.
 */
#include "../lir/lir_test_programs.h"
#include "base/xmalloc.h"
#include "vm/register/xrbc_lir.h"


// The sole guest result is owned; this test helper consumes it after inspection.
static XrRbcStatus execute(XrRbcProgram *program, const uint64_t *arguments,
    size_t count, uint64_t *bits) {
    XrRbcExecution *execution = NULL;
    XrRbcStatus status = xr_rbc_program_prepare(program, arguments, count, &execution);
    if (status != XR_RBC_OK) return status;
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome);
    status = xr_rbc_outcome_status(outcome);
    xr_rbc_outcome_bits(outcome, bits);
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
    return status;
}

static size_t attempts, fail_at = SIZE_MAX, live_allocations;
static bool in_compile_admission;
static unsigned admission_attempts,admission_failure;
static unsigned compile_admission_calls, admission_source_map_passes;
static size_t fail_home_size;
static enum {LITERAL_NO_FAULT,LITERAL_MAP_FAULT,LITERAL_PAYLOAD_FAULT,LITERAL_TABLE_FAULT,LITERAL_TABLE_INITIAL_FAULT} literal_fault;
static size_t literal_payload_count, literal_payload_failure, literal_table_requests;
static struct { void *pointer; size_t size; } buffers[32];

static void *allocate(size_t size) {
    if(in_compile_admission && ++admission_attempts==admission_failure) return NULL;
    if (++attempts == fail_at) return NULL;
    if(fail_home_size && size==fail_home_size){fail_home_size=0;return NULL;}
    if (literal_fault == LITERAL_PAYLOAD_FAULT && size == 29 &&
        ++literal_payload_count == literal_payload_failure) {literal_fault=LITERAL_NO_FAULT;return NULL;}
    void *pointer=xr_malloc(size);if(pointer)++live_allocations;return pointer;
}

static void *allocate_zero(size_t count, size_t size) {
    if(in_compile_admission && ++admission_attempts==admission_failure) return NULL;
    if (++attempts == fail_at) return NULL;
    if (literal_fault == LITERAL_MAP_FAULT && count == 20 && size == sizeof(uint32_t)) {
        literal_fault=LITERAL_NO_FAULT;return NULL;
    }
    void *pointer=xr_calloc(count,size);if(pointer)++live_allocations;return pointer;
}

static void release(void *pointer) {
    if(pointer){CHECK(live_allocations);--live_allocations;}
    for (size_t i = 0; i < 32; ++i)
        if (buffers[i].pointer == pointer) {
            buffers[i].pointer = NULL;
            buffers[i].size = 0;
        }
    xr_free(pointer);
}

static void *relocate(void *old, size_t size) {
    if (++attempts == fail_at) return NULL;
    // The first equal-sized initial buffer is the branch-patch table; the
    // second is the real literal table; production decides the error status.
    if(literal_fault == LITERAL_TABLE_INITIAL_FAULT && !old &&
        size == 16*sizeof(XrRbcLiteral) && ++literal_table_requests == 2) {
        literal_fault=LITERAL_NO_FAULT;return NULL;
    }
    if(literal_fault == LITERAL_TABLE_FAULT && old && size == 32*sizeof(XrRbcLiteral)) {
        literal_fault=LITERAL_NO_FAULT;return NULL;
    }
    size_t i = 0;
    while (i < 32 && buffers[i].pointer != old) ++i;
    CHECK(i < 32);
    void *fresh = xr_malloc(size);
    CHECK(fresh);
    if(!old)++live_allocations;
    if (old) {
        memcpy(fresh, old, buffers[i].size < size ? buffers[i].size : size);
        xr_free(old);
    }
    buffers[i].pointer = fresh;
    buffers[i].size = size;
    return fresh;
}

#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(size) allocate(size)
#define xr_calloc(count, size) allocate_zero(count, size)
#define xr_realloc(old, size) relocate(old, size)
#define xr_free(pointer) release(pointer)
static bool fail_location_allocation;
static void *allocate_location(size_t size) {
    if (fail_location_allocation) {
        fail_location_allocation = false;
        return NULL;
    }
    return allocate(size);
}
#undef xr_malloc
#define xr_malloc(size) allocate_location(size)
#include "runtime/native/xnative_locations.c"
#undef xr_malloc
#define xr_malloc(size) allocate(size)
/* Each successful full admission queries the location inventory once. Observe
 * that real receiver without adding a production admission counter or flag. */
static size_t observed_location_count(const XrtLocationOwner *owner) {
    ++admission_source_map_passes;
    return xrt_locations_count(owner);
}
#define xrt_locations_count observed_location_count
#include "vm/register/xrbc.c"
#undef xrt_locations_count
// Scope fault injection to the actual common admission called before publish.
static XrRbcStatus compile_admission(const XrRbcModule *module,const XrtLocationOwner *locations) {
    ++compile_admission_calls;
    in_compile_admission=true;admission_attempts=0;
    XrRbcStatus status=xr_rbc_module_admit(module,locations);
    in_compile_admission=false;return status;
}
#define xr_rbc_module_admit compile_admission
#include "vm/register/xrbc_lir.c"
#undef xr_rbc_module_admit
#include "../lir/lir_array_c_fixture.h"
#include "../lir/lir_array_ref_fixture.h"
#include "../lir/lir_fixed_bytecode_fixture.h"
#include "lir_array_copy_cases.h"
#include "lir_edge_spill_cases.h"
#include "../lir/lir_view_fixture.h"
#include "../lir/lir_callable_fixture.h"
#include "../lir/lir_callable_factory_fixture.h"
#include "../lir/lir_callable_composition_fixture.h"
#include "../lir/lir_tuple_fixture.h"
#include "../lir/lir_record_fixture.h"
#include "lir_bytecode_owned_prepare_cases.h"

static void callable_factory_failures(bool scopes) {
    XrLirModule *module = NULL;
    XrLirFunction root;
    callable_factory_fixture_mode(&module, &root, false, scopes);
    XrRbcProgram *program = NULL;
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    xr_rbc_program_destroy(program); program = NULL;
    CHECK(!live_allocations);
    for (size_t point = 1; point <= total; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !live_allocations && attempts >= point);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    size_t retained = live_allocations;
    uint64_t result = 0;
    attempts = 0;
    CHECK(execute(program, NULL, 0, &result) == XR_RBC_OK && result == 137);
    size_t runtime = attempts;
    CHECK(live_allocations == retained);
    for (size_t point = 1; point <= runtime; ++point) {
        attempts = 0; fail_at = point;
        CHECK(execute(program, NULL, 0, &result) == XR_RBC_OUT_OF_MEMORY);
        CHECK(live_allocations == retained);
    }
    fail_at = SIZE_MAX;
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    printf("Owned %sfactory compiler/runtime failure points: %zu/%zu\n", scopes ? "scoped " : "", total, runtime);
}

static void callable_scope_handle_reuse(void) {
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_factory_fixture(&module, &root, false);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    CHECK(execution->module->closure_entry_count == 1);
    XrRbcStack *stack = &execution->stack;
    CHECK(xr_xir_domain_new(UINT64_MAX, &stack->closure_domain) == XR_XIR_VALUE_OK);
    CHECK(xr_rbc_closure_scope_enter(stack));
    uint64_t baseline = xr_xir_domain_stats(stack->closure_domain).live_bytes;
    XrXirValue capture = {.type=XR_XIR_I64, .payload=40};
    uint64_t previous = 0;
    size_t index = SIZE_MAX;
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        CHECK(xr_rbc_closure_owner_reserve(stack));
        CHECK(xrt_callable_new(stack->closure_code, stack->closure_domain, 0,
            &capture, 1, &stack->closure_pending) == XR_XIR_VALUE_OK);
        uint64_t token = xr_rbc_closure_owner_commit(stack, 0, UINT32_MAX);
        CHECK(token != previous && stack->closure_count == 1);
        CHECK(xr_rbc_closure_slot(stack, token, &index) && index == 0);
        if (previous) CHECK(!xr_rbc_closure_slot(stack, previous, &index));
        xr_rbc_closure_scope_drop(stack, 0);
        CHECK(!xr_rbc_closure_slot(stack, token, &index));
        CHECK(xr_xir_domain_stats(stack->closure_domain).live_bytes == baseline);
        previous = token;
    }
    stack->closure_owners[0].generation = UINT32_C(0x3fffffff);
    CHECK(xrt_callable_new(stack->closure_code, stack->closure_domain, 0,
        &capture, 1, &stack->closure_pending) == XR_XIR_VALUE_OK);
    uint64_t retired = xr_rbc_closure_owner_commit(stack, 0, UINT32_MAX);
    xr_rbc_closure_scope_drop(stack, 0);
    CHECK(!stack->closure_free && !xr_rbc_closure_slot(stack, retired, &index));
    CHECK(xr_rbc_closure_owner_reserve(stack));
    CHECK(xrt_callable_new(stack->closure_code, stack->closure_domain, 0,
        &capture, 1, &stack->closure_pending) == XR_XIR_VALUE_OK);
    uint64_t token = xr_rbc_closure_owner_commit(stack, 0, UINT32_MAX);
    CHECK(stack->closure_count == 2 && xr_rbc_closure_slot(stack, token, &index) && index == 1);
    CHECK(!xr_rbc_closure_slot(stack, retired, &index));
    xr_rbc_closure_scope_drop(stack, 0);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    puts("Owned scope handles reuse cleared slots with generations and retire exhausted counters");
}

static void callable_compiler_failures(void) {
    for (unsigned capture = 0; capture < 5; ++capture) {
        XrLirModule *module = NULL;
        XrLirFunction root = {0};
        callable_fixture_capture(&module, &root, capture);
        XrRbcProgram *program = NULL;
        attempts = 0; fail_at = SIZE_MAX;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
        size_t total = attempts;
        xr_rbc_program_destroy(program); program = NULL;
        CHECK(!live_allocations);
        for (size_t point = 1; point <= total; ++point) {
            attempts = 0; fail_at = point;
            CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
            CHECK(!program && !live_allocations && attempts >= point);
        }
        fail_at = SIZE_MAX;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
        xr_lir_module_destroy(module);
        uint64_t result = 0;
        if (capture) {
            XrRbcFunction *functions = (XrRbcFunction *)xr_rbc_program_view(program)->functions;
            uint32_t callee = capture >= 3 ? xr_rbc_program_view(program)->closure_targets[0] :
                functions[0].environments[0].callee;
            XrRbcFunction saved = functions[callee];
            XrRbcRegisterKind kinds[XR_RBC_REGISTER_COUNT_MAX] = {0};
            for (uint32_t r = 0; r < saved.register_count; ++r) kinds[r] = saved.register_kinds[r];
            XrRbcRegisterKind cell_kinds[200] = {0};
            functions[callee].register_count = XR_RBC_REGISTER_COUNT_MAX;
            functions[callee].register_kinds = kinds;
            functions[callee].register_kind_count = XR_RBC_REGISTER_COUNT_MAX;
            functions[callee].cell_count = 200;
            functions[callee].cell_kinds = cell_kinds;
            functions[callee].cell_kind_count = 200;
            functions[callee].spill_count = 200;
            size_t retained = live_allocations;
            attempts = 0;
            CHECK(execute(program, NULL, 0, &result) == XR_RBC_OK && result == 42);
            size_t runtime = attempts;
            CHECK(live_allocations == retained);
            for (size_t point = 1; point <= runtime; ++point) {
                attempts = 0; fail_at = point;
                CHECK(execute(program, NULL, 0, &result) == XR_RBC_OUT_OF_MEMORY);
                CHECK(live_allocations == retained);
            }
            fail_at = SIZE_MAX;
            functions[callee] = saved;
            printf("Frame callable relocating runtime failure points: %zu\n", runtime);
        }
        CHECK(execute(program, NULL, 0, &result) == XR_RBC_OK && result == 42);
        xr_rbc_program_destroy(program);
        CHECK(!live_allocations);
        printf("Callable bytecode compiler failure points: %zu\n", total);
    }
}

static void callable_mutable_frame_storage(void) {
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_fixture_capture(&module, &root, 2);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    const XrRbcModule *view = xr_rbc_program_view(program);
    CHECK(!view->closure_entry_count && !view->closure_types);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    size_t slots = 0, cells = 0, spills = 0, environments = 0;
    for (size_t f = 0; f < view->function_count; ++f) {
        const XrRbcFunction *function = &view->functions[f];
        size_t environment, bytes;
        CHECK(xr_rbc_activation_size(function, &environment, &bytes));
        slots += function->register_count;
        cells += function->cell_count;
        spills += function->spill_count + bytes / 8u + (bytes % 8u != 0);
        environments += function->environment_count;
    }
    CHECK(cells == 1 && environments == 1);
    /* The real moving allocator reserves the complete three-frame call path.
     * Once its indexed banks exist, constructing and calling the mutable
     * closure cannot request a separate environment or canonical object. */
    CHECK(xr_rbc_reserve(&execution->stack, slots, view->function_count, cells, spills));
    size_t before = attempts, retained = live_allocations;
    fail_at = before + 1;
    XrRbcRegister result = {0};
    CHECK(xr_rbc_execute(view, &execution->stack, execution->pending->locations,
        &result, execution->entries) == XR_RBC_OK && result.bits == 42);
    CHECK(attempts == before && live_allocations == retained);
    CHECK(!execution->stack.depth && !execution->stack.cell_count);
    CHECK(!execution->stack.closure_code && !execution->stack.closure_domain &&
        !execution->stack.closures && !execution->stack.closure_scopes);
    fail_at = SIZE_MAX;
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    puts("mutable Frame capture uses one indexed 8-byte place and no construction allocation");
}

static void view_compiler_failures(void) {
    XrLirModule *module = NULL;
    OK(view_fixture_create(&module));
    XrRbcProgram *program = NULL;
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, (XrLirFunction){module, 1}, &program, NULL) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    xr_rbc_program_destroy(program); program = NULL;
    CHECK(!live_allocations);
    for (size_t point = 1; point <= total; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_compile(module, (XrLirFunction){module, 1}, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !live_allocations && attempts >= point);
    }
    fail_at = SIZE_MAX;
    xr_lir_module_destroy(module);
    printf("View bytecode compiler failure points: %zu\n", total);
}

static void array_compiler_failures(const char *name,uint64_t expected) {
    XrLirModule *m=NULL;XrLirFunction root={0};
    array_c_fixture(name,&m,&root);
    XrRbcProgram *program=NULL;XrRbcCompileDiagnostic diagnostic={0};
    attempts=0;fail_at=SIZE_MAX;
    CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OK);
    size_t total=attempts;
    CHECK(total>10);xr_rbc_program_destroy(program);program=NULL;CHECK(!live_allocations);
    for(size_t point=1;point<=total;++point) {
        attempts=0;fail_at=point;
        CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status==XR_RBC_COMPILE_OUT_OF_MEMORY && !live_allocations);
    }
    fail_at=SIZE_MAX;
    CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(m);
    uint64_t result=0;CHECK(execute(program,NULL,0,&result)==XR_RBC_OK && result==expected);
    xr_rbc_program_destroy(program);CHECK(!live_allocations);
    printf("Array %s compiler failure points: %zu\n",name,total);
}
static void array_reference_compiler_failures(void) {
    const XrLirType elements[] = {XR_LIR_I64, XR_LIR_STRING, XR_LIR_U8, XR_LIR_U64};
    for (uint32_t family = 0; family < 4; ++family) {
        XrLirModule *m = NULL;
        OK(array_ref_fixture(elements[family], &m));
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic diagnostic = {0};
        attempts = 0; fail_at = SIZE_MAX;
        CHECK(xr_rbc_compile(m, (XrLirFunction){m, 1}, &program, &diagnostic) == XR_RBC_COMPILE_OK);
        size_t total = attempts;
        xr_rbc_program_destroy(program); program = NULL;
        CHECK(total > 10 && !live_allocations);
        for (size_t point = 1; point <= total; ++point) {
            attempts = 0; fail_at = point;
            CHECK(xr_rbc_compile(m, (XrLirFunction){m, 1}, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
            CHECK(!program && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY && !live_allocations);
        }
        fail_at = SIZE_MAX;
        CHECK(xr_rbc_compile(m, (XrLirFunction){m, 1}, &program, &diagnostic) == XR_RBC_COMPILE_OK);
        xr_lir_module_destroy(m);
        uint64_t result = 99;
        CHECK(execute(program, NULL, 0, &result) == XR_RBC_OK && result == 0);
        xr_rbc_program_destroy(program);
        CHECK(!live_allocations);
        printf("Array Ref family %u compiler failure points: %zu\n", family, total);
    }
}

static void source_map_failures(void) {
    for (unsigned grown = 0; grown < 2; ++grown) {
        XrRbcCompileDiagnostic diagnostic = {0};
        RbcLirCompiler compiler = {.diagnostic = &diagnostic};
        size_t words = 0;
        if (grown) {
            compiler.current_source_id = 1;
            CHECK(rbc_lir_emit(&compiler, XR_RBC_LOADI, 0, 0, 0, 0));
            words = compiler.code_capacity;
        }
        for (size_t i = compiler.code_count; i < words; ++i) {
            compiler.current_source_id = (uint32_t)i + 1;
            CHECK(rbc_lir_emit(&compiler, XR_RBC_LOADI, 0, 0, 0, (int64_t)i));
        }
        // Code growth succeeds and relocates; the following map allocation fails.
        attempts = 0;
        fail_at = 2;
        CHECK(!rbc_lir_emit(&compiler, XR_RBC_LOADI, 0, 0, 0, 99));
        CHECK(compiler.status == XR_RBC_COMPILE_OUT_OF_MEMORY && compiler.code_count == words);
        CHECK(compiler.code && compiler.code_capacity > words && compiler.source_id_capacity == words);
        for (size_t i = 0; i < words; ++i)
            CHECK(compiler.source_ids[i] == i + 1);
        xr_free(compiler.code);
        xr_free(compiler.source_ids);
        fail_at = SIZE_MAX;
        attempts = 0;
    }
    XrRbcCompileDiagnostic diagnostic = {0};
    RbcLirCompiler compiler = {.diagnostic = &diagnostic};
    for (uint32_t i = 0; i < 40; ++i) {
        compiler.current_source_id = i + 1;
        CHECK(rbc_lir_emit(&compiler, XR_RBC_LOADI, 0, 0, 0, i));
    }
    CHECK(compiler.code_count == 40);
    for (size_t i = 0; i < 40; ++i) CHECK(compiler.source_ids[i] == i + 1);
    xr_free(compiler.code);
    xr_free(compiler.source_ids);
    attempts = 0;
}

static void compiled_source_maps(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = XR_LIR_I64;
    XrLirFunction root = test_function(m, "root", NULL, 0, type);
    XrLirFunction helper = test_function(m, "helper", &type, 1, type);
    XrLirBlock entry = test_block(m, root, NULL, 0), body = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    OK(xr_lir_set_entry(m, helper, body));
    XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = type, .bits = 42,
        .source = {"mapped.xr", 9, 2, 3, 2, 5}};
    XrLirValue value, argument;
    OK(xr_lir_emit_value(m, entry, &instruction, &value));
    test_return(m, entry, test_call(m, entry, helper, value));
    OK(xr_lir_function_parameter(m, helper, 0, &argument));
    test_return(m, body, argument);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(m);
    CHECK(program->module.function_count == 2 && xrt_locations_count(program->locations) == 1);
    bool located = false;
    for (size_t f = 0; f < 2; ++f) {
        const XrRbcFunction *function = &program->module.functions[f];
        CHECK(function->source_ids && function->source_id_count == function->code_count);
        if (!f) {
            CHECK((function->code[0] & 255) == XR_RBC_LOADI);
            CHECK(function->source_ids[0] == 1);
        }
        for (size_t pc = 0; pc < function->code_count; ++pc) {
            CHECK(function->source_ids[pc] <= 1);
            if (f) CHECK(function->source_ids[pc] == 0);
            located |= function->source_ids[pc] != 0;
        }
    }
    uint64_t bits = 0;
    CHECK(located && execute(program, NULL, 0, &bits) == XR_RBC_OK && bits == 42);
    xr_rbc_program_destroy(program);
}

static void register_workspace_failure(void) {
    XrLirModule *m = test_fib();
    XrLirFunction root;
    XrLirFunctionView view;
    OK(xr_lir_function_at(m, 1, &root));
    OK(xr_lir_function_view(m, root, &view));
    XrRbcCompileDiagnostic diagnostic = {0};
    RbcLirCompiler compiler = {.input = m, .function = root, .view = view,
        .diagnostic = &diagnostic, .status = XR_RBC_COMPILE_OK};
    // Fail the workspace request directly, without a compile allocation quota.
    fail_at = attempts + 1;
    CHECK(!rbc_lir_assign_slots(&compiler));
    CHECK(compiler.status == XR_RBC_COMPILE_OUT_OF_MEMORY);
    CHECK(diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY);
    fail_at = SIZE_MAX;
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    uint64_t input = 12, result = 0;
    CHECK(execute(program, &input, 1, &result) == XR_RBC_OK && result == 144);
    xr_rbc_program_destroy(program);
    xr_lir_module_destroy(m);
}


static XrLirModule *cell_compiler_module(XrLirFunction *root) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = XR_LIR_I64;
    *root = test_function(m, "cell_square", &type, 1, type);
    XrLirBlock body = test_block(m, *root, NULL, 0);
    OK(xr_lir_set_entry(m, *root, body));
    XrLirValue input, cell, value;
    OK(xr_lir_function_parameter(m, *root, 0, &input));
    XrLirInstruction instruction = {.op = XR_LIR_CELL_NEW, .type = XR_LIR_I64_CELL,
        .operands = &input, .operand_count = 1};
    OK(xr_lir_emit_value(m, body, &instruction, &cell));
    instruction = (XrLirInstruction) {.op = XR_LIR_CELL_READ, .type = XR_LIR_I64,
        .operands = &cell, .operand_count = 1};
    OK(xr_lir_emit_value(m, body, &instruction, &value));
    test_return(m, body, test_binary(m, body, XR_LIR_MUL, value, value, XR_LIR_PREDICATE_NONE));
    OK(xr_lir_module_finish(m, NULL));
    return m;
}


static void literal_allocation_failures(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirType flag=XR_LIR_I1,types[]={XR_LIR_PTR,XR_LIR_U64};
    XrLirFunction root=test_function(m,"literal_allocations",&flag,1,XR_LIR_UNIT),print;
    XrLirFunctionSpec spec={.name="xrt_print_string",.name_length=16,.parameters=types,
        .parameter_count=2,.result=XR_LIR_UNIT,.kind=XR_LIR_FUNCTION_EXTERNAL,
        .extern_kind=XR_LIR_EXTERN_PRINT_STRING};
    OK(xr_lir_declare_function(m,&spec,&print));
    XrLirBlock entry=test_block(m,root,NULL,0),output=test_block(m,root,NULL,0),done=test_block(m,root,NULL,0);
    OK(xr_lir_set_entry(m,root,entry));
    XrLirValue argument;OK(xr_lir_function_parameter(m,root,0,&argument));
    XrLirInstruction conditional={.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&argument,.operand_count=1,
        .edges={{output,NULL,0},{done,NULL,0}},.edge_count=2};
    OK(xr_lir_set_terminator(m,entry,&conditional));
    XrLirValue length=test_constant(m,output,XR_LIR_U64,29);
    for(unsigned n=0;n<20;++n) {
        char bytes[29];memset(bytes,(int)('A'+n),sizeof(bytes));bytes[1]=0;
        XrLirLiteral literal={0};OK(xr_lir_add_literal(m,bytes,sizeof(bytes),&literal));
        memset(bytes,'!',sizeof(bytes));
        XrLirValue args[2]={{0},length};OK(xr_lir_literal_address(m,output,literal,&args[0]));
        XrLirInstruction call={.op=XR_LIR_CALL,.type=XR_LIR_UNIT,.callee=print,.operands=args,.operand_count=2};
        OK(xr_lir_emit_effect(m,output,&call));
    }
    test_branch(m,output,done,NULL,0);test_return(m,done,(XrLirValue){0});
    OK(xr_lir_module_finish(m,NULL));
    for(unsigned stage=0;stage<6;++stage) {
        CHECK(!live_allocations);attempts=0;fail_at=SIZE_MAX;
        literal_payload_count=0;literal_table_requests=0;literal_payload_failure=stage==4?17:1;
        literal_fault=stage==1?LITERAL_MAP_FAULT:stage==2?LITERAL_TABLE_INITIAL_FAULT:
            stage==3||stage==4?LITERAL_PAYLOAD_FAULT:stage==5?LITERAL_TABLE_FAULT:LITERAL_NO_FAULT;
        XrRbcProgram *program=NULL;
        XrRbcCompileDiagnostic diagnostic;
        XrRbcCompileStatus status=xr_rbc_compile(m,root,&program,&diagnostic);
        if(stage) {
            CHECK(status==XR_RBC_COMPILE_OUT_OF_MEMORY&&!program&&literal_fault==LITERAL_NO_FAULT);
            CHECK(!live_allocations);
            CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OK);
        } else CHECK(status==XR_RBC_COMPILE_OK);
        CHECK(program->module.literal_count==20);
        for(unsigned n=0;n<20;++n) {
            CHECK(program->module.literals[n].length==29&&program->module.literals[n].bytes[1]==0);
            CHECK(program->module.literals[n].bytes[0]==(char)('A'+n));
        }
        uint64_t flag_value=0,bits=99;
        CHECK(execute(program,&flag_value,1,&bits)==XR_RBC_OK&&!bits);
        xr_rbc_program_destroy(program);CHECK(!live_allocations);
    }
    xr_lir_module_destroy(m);
}

static void compiled_spills(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirFunction root=test_function(m,"spilled_virtual_values",NULL,0,XR_LIR_I64);
    XrLirBlock body=test_block(m,root,NULL,0);OK(xr_lir_set_entry(m,root,body));
    XrLirValue value={0};for(uint32_t n=0;n<260;++n)value=test_constant(m,body,XR_LIR_I64,n);
    test_return(m,body,value);OK(xr_lir_module_finish(m,NULL));
    XrLirFunctionView input;OK(xr_lir_function_view(m,root,&input));
    XrRbcProgram *program=NULL;XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OK);
    CHECK(xr_rbc_program_view(program)->functions[0].spill_count);
    uint64_t bits=0;CHECK(execute(program,NULL,0,&bits)==XR_RBC_OK&&bits==259);
    xr_rbc_program_destroy(program);program=NULL;CHECK(!live_allocations);
    // The index, immutable IN rows and separate spill rows all fail before
    // publication. Recovery repeats the real admission, without an Execution.
    CHECK(admission_attempts==3);
    for(unsigned point=1;point<=3;++point) {
        admission_failure=point;
        CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status==XR_RBC_COMPILE_OUT_OF_MEMORY && !live_allocations);
        CHECK(admission_attempts==point);admission_failure=0;
        CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OK);
        CHECK(execute(program,NULL,0,&bits)==XR_RBC_OK && bits==259);
        xr_rbc_program_destroy(program);program=NULL;CHECK(!live_allocations);
    }
    fail_home_size=((size_t)input.value_count+1)*sizeof(uint32_t);
    CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OUT_OF_MEMORY);
    CHECK(!program&&!fail_home_size&&!live_allocations);
    CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OK);
    CHECK(execute(program,NULL,0,&bits)==XR_RBC_OK&&bits==259);
    xr_rbc_program_destroy(program);xr_lir_module_destroy(m);CHECK(!live_allocations);
}
static void frame_cfg_compiler_failures(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirFunction root=test_function(m,"frame_allocation",NULL,0,XR_LIR_I64);
    XrLirBlock entry=test_block(m,root,NULL,0),done=test_block(m,root,NULL,0);
    OK(xr_lir_set_entry(m,root,entry));XrLirLiteral literal={0};
    OK(xr_lir_add_literal(m,"frame\0x",7,&literal));XrLirValue text,cell,length;
    XrLirInstruction op={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    OK(xr_lir_emit_value(m,entry,&op,&text));
    op=(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_STRING_CELL,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_value(m,entry,&op,&cell));
    op=(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_effect(m,entry,&op));test_branch(m,entry,done,NULL,0);
    op=(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_STRING,.operands=&cell,.operand_count=1};
    OK(xr_lir_emit_value(m,done,&op,&text));
    op=(XrLirInstruction){.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_value(m,done,&op,&length));
    op=(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&text,.operand_count=1};
    OK(xr_lir_emit_effect(m,done,&op));
    op=(XrLirInstruction){.op=XR_LIR_STRING_CELL_DROP,.type=XR_LIR_UNIT,.operands=&cell,.operand_count=1};
    OK(xr_lir_emit_effect(m,done,&op));test_return(m,done,length);OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=NULL;XrRbcCompileDiagnostic diagnostic;
    attempts=0;fail_at=SIZE_MAX;CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OK);
    size_t total=attempts;CHECK(total>10);xr_rbc_program_destroy(program);program=NULL;CHECK(!live_allocations);
    for(size_t point=1;point<=total;++point) {
        attempts=0;fail_at=point;
        CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status==XR_RBC_COMPILE_OUT_OF_MEMORY && !live_allocations);
    }
    fail_at=SIZE_MAX;CHECK(xr_rbc_compile(m,root,&program,&diagnostic)==XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(m);uint64_t bits=99;
    CHECK(execute(program,NULL,0,&bits)==XR_RBC_OK && bits==7);
    xr_rbc_program_destroy(program);CHECK(!live_allocations);
}

static void physical_string_copy_cycle(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirFunction f=test_function(m,"forward_owned_copy",NULL,0,XR_LIR_I64);
    XrLirType texts[]={XR_LIR_STRING,XR_LIR_STRING};
    XrLirBlock entry=test_block(m,f,NULL,0),join=test_block(m,f,texts,2);
    OK(xr_lir_set_entry(m,f,entry));XrLirValue values[2]={{0},{0}},parameters[2]={{0},{0}};
    const char *bytes[]={"FIRST","LONG"};size_t lengths[]={5,4};
    for(unsigned n=0;n<2;++n) {
        XrLirLiteral literal={0};OK(xr_lir_add_literal(m,bytes[n],lengths[n],&literal));
        XrLirInstruction op={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
        OK(xr_lir_emit_value(m,entry,&op,&values[n]));OK(xr_lir_block_parameter(m,join,n,&parameters[n]));
    }
    test_branch(m,entry,join,values,2);XrLirValue length={0};
    XrLirInstruction op={.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,.operands=&parameters[0],.operand_count=1};
    OK(xr_lir_emit_value(m,join,&op,&length));
    for(unsigned n=0;n<2;++n) {
        op=(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&parameters[n],.operand_count=1};
        OK(xr_lir_emit_effect(m,join,&op));
    }
    test_return(m,join,length);OK(xr_lir_module_finish(m,NULL));XrLirFunctionView view;
    OK(xr_lir_function_view(m,f,&view));XrRbcCompileDiagnostic diagnostic={0};
    RbcLirCompiler c={.input=m,.function=f,.diagnostic=&diagnostic,.typed_spills=true,
        .slot_count=2,.temporary_strings=1,.temporary_bits=1,.scratch_count=2};
    c.slots=xr_calloc((size_t)view.value_count+1,sizeof(*c.slots));CHECK(c.slots);
    // White-box emitter fixture: force a physical swap for verified forward
    // LIR. This does not claim a loop-carried String producer is admitted.
    c.slots[values[0].id]=1;c.slots[values[1].id]=0;
    c.slots[parameters[0].id]=0;c.slots[parameters[1].id]=1;
    CHECK(rbc_lir_emit(&c,XR_RBC_STRING_LITERAL,1,0,0,0));
    CHECK(rbc_lir_emit(&c,XR_RBC_STRING_LITERAL,0,0,0,1));
    uint32_t actuals[]={values[0].id,values[1].id};bool scratch=false;
    CHECK(rbc_lir_copies(&c,(XrLirEdgeView){join.id,actuals,2},true,&scratch));
    CHECK(scratch && c.string_scratch && c.code_count==5);
    CHECK((c.code[2]&255)==XR_RBC_STRING_TAKE && ((c.code[2]>>8)&255)==2);
    CHECK(rbc_lir_emit(&c,XR_RBC_STRING_LEN,3,0,0,0));
    CHECK(rbc_lir_emit(&c,XR_RBC_STRING_DROP,0,0,0,0));
    CHECK(rbc_lir_emit(&c,XR_RBC_STRING_DROP,1,0,0,0));
    CHECK(rbc_lir_emit(&c,XR_RBC_RET,3,0,0,0));
    XrRbcRegisterKind kinds[]={XR_RBC_STRING,XR_RBC_STRING,XR_RBC_STRING,XR_RBC_BITS};
    XrRbcFunction function={.code=c.code,.code_count=c.code_count,.register_count=4,
        .register_kinds=kinds,.register_kind_count=4,.spill_count=1};
    XrRbcLiteral literals[]={{bytes[0],lengths[0]},{bytes[1],lengths[1]}};
    XrRbcModule module={.functions=&function,.function_count=1,.literals=literals,.literal_count=2};
    XrtLocationOwner *locations=NULL;CHECK(xrt_locations_create(NULL,0,&locations)==XRT_LOCATION_OK);
    for(unsigned entry_count=0;entry_count<2;++entry_count) {
        XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_OK);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);uint64_t result=0;
        CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK);
        CHECK(xr_rbc_outcome_bits(outcome,&result) && result==5);
        xr_rbc_execution_free(&execution);xr_rbc_outcome_free(&outcome);
    }
    xrt_locations_release(locations);
    c.temporary_strings=0;CHECK(rbc_lir_copy_temporary(&c,true)==UINT32_MAX);
    CHECK(c.status==XR_RBC_COMPILE_LIMIT);
    xr_free(c.slots);xr_free(c.code);xr_free(c.source_ids);xr_lir_module_destroy(m);
    CHECK(!live_allocations);
}

static void jump_threading(void) {
    uint32_t code[9], original[9], source_ids[9];
    for (unsigned pc = 0; pc < 9; ++pc) {
        code[pc] = XR_RBC_RET0;
        source_ids[pc] = pc + 1;
    }
    CHECK(xr_rbc_encode(&code[0], (XrRbcOperands) {XR_RBC_LT, 0, 1, 0, 0}));
    const size_t jumps[][2] = {{1, 3}, {2, 3}, {3, 4}, {5, 6}, {6, 5}, {7, 5}};
    for (size_t j = 0; j < sizeof(jumps) / sizeof(jumps[0]); ++j)
        CHECK(xr_rbc_jump(&code[jumps[j][0]], jumps[j][0], jumps[j][1], 9));
    memcpy(original, code, sizeof(code));
    XrRbcCompileDiagnostic diagnostic = {0};
    RbcLirCompiler compiler = {
        .code = code, .source_ids = source_ids, .code_count = 9, .diagnostic = &diagnostic};
    size_t live = live_allocations;
    fail_at = attempts + 1;
    CHECK(!rbc_lir_thread_jumps(&compiler));
    CHECK(compiler.status == XR_RBC_COMPILE_OUT_OF_MEMORY);
    CHECK(!memcmp(code, original, sizeof(code)) && live_allocations == live);
    fail_at = SIZE_MAX;
    compiler.status = XR_RBC_COMPILE_OK;
    diagnostic = (XrRbcCompileDiagnostic) {0};
    CHECK(rbc_lir_thread_jumps(&compiler));
    CHECK(live_allocations == live && (code[1] & 255) == XR_RBC_JMP);
    for (size_t pc = 1; pc <= 3; ++pc)
        CHECK(rbc_lir_jump_target(&compiler, pc) == 4);
    for (size_t pc = 5; pc <= 7; ++pc)
        CHECK(rbc_lir_jump_target(&compiler, pc) == 5);
    for (unsigned pc = 0; pc < 9; ++pc)
        CHECK(source_ids[pc] == pc + 1);
    XrRbcFunction function = {
        .code = code, .code_count = 9, .register_count = 2, .parameter_count = 2};
    XrRbcModule module = {.functions = &function, .function_count = 1};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_OK);
    CHECK(xr_rbc_module_admit(&module, owner) == XR_RBC_OK);
    xrt_locations_release(owner);

    size_t count = 8193;
    uint32_t *chain = xr_malloc(count * sizeof(*chain));
    CHECK(chain);
    for (size_t pc = 0; pc + 1 < count; ++pc)
        CHECK(xr_rbc_jump(&chain[pc], pc, pc + 1, count));
    chain[count - 1] = XR_RBC_RET0;
    compiler = (RbcLirCompiler) {.code = chain, .code_count = count, .diagnostic = &diagnostic};
    CHECK(rbc_lir_thread_jumps(&compiler));
    for (size_t pc = 0; pc + 1 < count; ++pc)
        CHECK(rbc_lir_jump_target(&compiler, pc) == count - 1);
    xr_free(chain);
    CHECK(live_allocations == live);
    attempts = 0;
}
static void jump_compaction(void) {
    uint32_t code[13], original[13], source_ids[13];
    for (size_t pc = 0; pc < 13; ++pc) {
        code[pc] = XR_RBC_RET0;
        source_ids[pc] = (uint32_t)pc + 1;
    }
    CHECK(xr_rbc_encode(&code[2], (XrRbcOperands){XR_RBC_EQ, 0, 1, 0, 0}));
    CHECK(xr_rbc_encode(&code[5], (XrRbcOperands){XR_RBC_LOADI, 2, 0, 0, 17}));
    CHECK(xr_rbc_encode(&code[7], (XrRbcOperands){XR_RBC_LOADI, 2, 0, 0, 23}));
    CHECK(xr_rbc_encode(&code[9], (XrRbcOperands){XR_RBC_RET, 2, 0, 0, 0}));
    const size_t jumps[][2] = {{0, 2}, {3, 7}, {4, 5}, {6, 9}, {8, 9}, {11, 10}};
    for (size_t j = 0; j < sizeof(jumps) / sizeof(jumps[0]); ++j)
        CHECK(xr_rbc_jump(&code[jumps[j][0]], jumps[j][0], jumps[j][1], 13));
    memcpy(original, code, sizeof(code));
    XrRbcCompileDiagnostic diagnostic = {0};
    RbcLirCompiler compiler = {
        .code = code, .source_ids = source_ids, .code_count = 13, .diagnostic = &diagnostic};
    size_t live = live_allocations;
    fail_at = attempts + 1;
    CHECK(!rbc_lir_compact_jumps(&compiler));
    CHECK(compiler.status == XR_RBC_COMPILE_OUT_OF_MEMORY && compiler.code_count == 13);
    CHECK(!memcmp(code, original, sizeof(code)) && live_allocations == live);
    for (unsigned pc = 0; pc < 13; ++pc) CHECK(source_ids[pc] == pc + 1);
    fail_at = SIZE_MAX;
    compiler.status = XR_RBC_COMPILE_OK;
    diagnostic = (XrRbcCompileDiagnostic){0};
    CHECK(rbc_lir_compact_jumps(&compiler) && compiler.code_count == 6);
    CHECK(rbc_lir_jump_target(&compiler, 1) == 4 && rbc_lir_jump_target(&compiler, 3) == 5);
    const uint32_t sources[] = {3, 4, 6, 7, 8, 10};
    CHECK(!memcmp(source_ids, sources, sizeof(sources)));
    XrRbcFunction function = {
        .code = code, .code_count = compiler.code_count, .register_count = 3, .parameter_count = 2};
    XrRbcModule module = {.functions = &function, .function_count = 1};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_OK);
    CHECK(xr_rbc_module_admit(&module, owner) == XR_RBC_OK);
    for (uint64_t argument = 1; argument <= 2; ++argument) {
        uint64_t arguments[] = {1, argument}, bits = 0;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, arguments, 2, owner, &execution) == XR_RBC_OK);
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK);
        CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == (argument == 1 ? 23u : 17u));
        xr_rbc_execution_free(&execution);
        xr_rbc_outcome_free(&outcome);
    }
    xrt_locations_release(owner);

    CHECK(xr_rbc_encode(&code[0], (XrRbcOperands){XR_RBC_EQ, 0, 1, 0, 0}));
    CHECK(xr_rbc_jump(&code[1], 1, 2, 3));
    code[2] = XR_RBC_RET0;
    compiler.code_count = 3;
    CHECK(rbc_lir_compact_jumps(&compiler) && compiler.code_count == 3);
    CHECK((code[1] & 255) == XR_RBC_JMP && rbc_lir_jump_target(&compiler, 1) == 2);
    CHECK(xr_rbc_jump(&code[0], 0, 0, 2));
    code[1] = XR_RBC_RET0;
    compiler.code_count = 2;
    CHECK(rbc_lir_compact_jumps(&compiler) && compiler.code_count == 1);
    CHECK(rbc_lir_jump_target(&compiler, 0) == 0 && live_allocations == live);
    const XrRbcAggregateOperation returns[] = {XR_RBC_AGGREGATE_RETURN, XR_RBC_FUNCTION_RETURN};
    for (size_t n = 0; n < sizeof(returns) / sizeof(returns[0]); ++n) {
        XrRbcAggregateAccess access = {.operation = returns[n]};
        CHECK(xr_rbc_jump(&code[0], 0, 1, 2));
        CHECK(xr_rbc_encode(&code[1], (XrRbcOperands){XR_RBC_AGGREGATE_ACCESS, 0, 0, 0, 0}));
        compiler.code_count = 2;
        compiler.aggregate_accesses = &access;
        compiler.aggregate_access_count = 1;
        CHECK(rbc_lir_compact_jumps(&compiler) && compiler.code_count == 1);
        CHECK((code[0] & 255) == XR_RBC_AGGREGATE_ACCESS && live_allocations == live);
    }
    attempts = 0;
}

static void compiled_float_constants(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = XR_LIR_F64;
    XrLirFunction root = test_function(m, "pooled_comparison", &type, 1, XR_LIR_I1);
    XrLirBlock body = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, body));
    XrLirValue input, result = {0};
    OK(xr_lir_function_parameter(m, root, 0, &input));
    for (unsigned n = 0; n < 40; ++n) {
        XrLirValue constant = test_constant(m, body, type,
            UINT64_C(0x4000000000000000) + ((uint64_t)n << 44));
        XrLirValue operands[] = {input, constant};
        XrLirInstruction comparison = {.op = XR_LIR_FCMP, .type = XR_LIR_I1,
            .predicate = XR_LIR_LE, .operands = operands, .operand_count = 2};
        OK(xr_lir_emit_value(m, body, &comparison, &result));
    }
    test_return(m, body, result);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    attempts = 0;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    CHECK(program->module.functions[0].constant_count == 40);
    uint64_t argument = 0, bits = 0;
    CHECK(execute(program, &argument, 1, &bits) == XR_RBC_OK && bits == 1);
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    // Relocation always moves the pool. Fail each real compilation allocation,
    // including its initial reservation and both growth steps, before publish.
    for (size_t point = 1; point <= total; ++point) {
        attempts = 0;
        fail_at = point;
        program = NULL;
        CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY && !live_allocations);
    }
    fail_at = SIZE_MAX;
    attempts = 0;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    CHECK(execute(program, &argument, 1, &bits) == XR_RBC_OK && bits == 1);
    xr_rbc_program_destroy(program);
    xr_lir_module_destroy(m);
    CHECK(!live_allocations);
    attempts = 0;
}

static void aggregate_type_projection_failures(void) {
    XrLirModule *module = NULL;
    OK(xr_lir_module_create(NULL, &module));
    XrLirType unused = 0;
    OK(xr_lir_fixed_array_type(module, XR_LIR_U8, 11, &unused));
    OK(xr_lir_fixed_array_type(module, XR_LIR_U8, 256, &unused));
    XrLirFunction root = test_function(module, "physical_types", NULL, 0, XR_LIR_UNIT);
    XrLirBlock body = test_block(module, root, NULL, 0);
    OK(xr_lir_set_entry(module, root, body));
    test_return(module, body, (XrLirValue){0});
    OK(xr_lir_module_finish(module, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    attempts = 0;
    CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    CHECK(program->module.aggregate_type_count == 2 && program->module.aggregate_types[0].count == 11 &&
        program->module.aggregate_types[1].count == 256);
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    for (size_t point = 1; point <= total; ++point) {
        attempts = 0; fail_at = point; program = NULL;
        CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY && !live_allocations);
    }
    attempts = 0; fail_at = SIZE_MAX; program = NULL;
    CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    CHECK(program->module.aggregate_type_count == 2 && program->module.aggregate_types[1].count == 256);
    uint64_t bits = 91;
    CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OK && !bits);
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    printf("aggregate type projection %zu allocation failures and retired LIR PASS\n", total);
    attempts = 0;
}

static void aggregate_value_compiler_failures(uint32_t count, bool dynamic, bool edges) {
    XrLirFunction root = {0};
    XrLirModule *module = edges ? fixed_bytecode_loop_fixture(count, &root) :
        fixed_bytecode_fixture(count, dynamic, false, &root);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    CHECK(total > 20 && program->module.functions[0].aggregate_value_count == (edges ? 10 : 1));
    xr_rbc_program_destroy(program); program = NULL;
    CHECK(!live_allocations);
    for (size_t point = 1; point <= total; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY && !live_allocations);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    uint64_t arguments[] = {edges ? 3 : 37, count - 1}, bits = 0;
    CHECK(execute(program, arguments, edges ? 1 : 2, &bits) == XR_RBC_OK &&
        bits == (edges ? 1107 : dynamic ? (37u + count - 1) & 255u : 37u));
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    printf("aggregate %u-byte %s %zu allocation failures and retired LIR PASS\n",
        count, edges ? "selected edges" : "construction", total);
    attempts = 0;
}

static void composition_compiler_failures(void) {
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_composition_fixture(&module, &root);
    XrRbcProgram *program = NULL;
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    xr_rbc_program_destroy(program); program = NULL;
    CHECK(total > 30 && !live_allocations);
    for (size_t point = 1; point <= total; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !live_allocations);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    size_t retained = live_allocations;
    attempts = 0;
    uint64_t bits = 0;
    CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OK && bits == 42);
    size_t runtime = attempts;
    CHECK(live_allocations == retained);
    for (size_t point = 1; point <= runtime; ++point) {
        attempts = 0; fail_at = point;
        CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OUT_OF_MEMORY);
        CHECK(live_allocations == retained);
    }
    fail_at = SIZE_MAX;
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    printf("Owned Fn capture compiler/runtime allocation failures: %zu/%zu PASS\n", total, runtime);
    attempts = 0;
}

static void tuple_compiler_failures(void) {
    XrLirModule *module = tuple_fixture();
    XrLirFunction root = {module, 1};
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    xr_rbc_program_destroy(program); program = NULL;
    CHECK(total > 20 && !live_allocations);
    for (size_t point = 1; point <= total; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !live_allocations);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    uint64_t bits = 0;
    CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OK && bits == 42);
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    printf("Tuple compiler %zu allocation failures and retired LIR PASS\n", total);
    attempts = 0;
}

static void aggregate_state_compiler_failures(void) {
    XrLirFunction root = {0};
    XrLirModule *module = fixed_bytecode_state_fixture(17, 0, &root);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    xr_rbc_program_destroy(program); program = NULL;
    CHECK(total > 30 && !live_allocations);
    for (size_t point = 1; point <= total; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY && !live_allocations);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    uint64_t bits = 0;
    CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OK && bits == 128);
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    printf("aggregate State %zu allocation failures and retired LIR PASS\n", total);
    attempts = 0;
}

static void aggregate_call_compiler_failures(bool aggregate) {
    XrLirFunction root = {0};
    XrLirModule *module = fixed_bytecode_calls_fixture(17, aggregate, true, &root);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    xr_rbc_program_destroy(program); program = NULL;
    CHECK(total > 50 && !live_allocations);
    for (size_t point = 1; point <= total; ++point) {
        attempts = 0; fail_at = point;
        CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY && !live_allocations);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution); xr_rbc_program_destroy(program);
    if (aggregate) {
        XrPodType type = {0};
        const uint8_t *bytes = NULL;
        CHECK(xr_rbc_outcome_aggregate(outcome, &type, &bytes) && type.count == 17);
        for (uint32_t n = 0; n < type.count; ++n) CHECK(bytes[n] == 37);
    } else {
        uint64_t bits = 0;
        CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == 3737);
    }
    xr_rbc_outcome_free(&outcome);
    CHECK(!live_allocations);
    printf("aggregate calls %s result %zu allocation failures and all metadata retired PASS\n",
        aggregate ? "owned byte" : "scalar", total);
    attempts = 0;
}

#include "lir_display_alloc_cases.h"
#include "lir_string_alloc_cases.h"
#include "lir_map_alloc_cases.h"

static void record_compiler_failures(void) {
    XrLirFunction root = {0};
    XrLirModule *module = record_fixture(&root);
    XrRbcProgram *program = NULL;
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    xr_rbc_program_destroy(program); program = NULL;
    CHECK(total && !live_allocations);
    for (size_t n = 1; n <= total; ++n) {
        attempts = 0; fail_at = n;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !live_allocations);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    size_t retained = live_allocations;
    uint64_t bits = 0;
    attempts = 0;
    CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OK && bits == 100);
    size_t runtime = attempts;
    CHECK(live_allocations == retained);
    for (size_t n = 1; n <= runtime; ++n) {
        attempts = 0; fail_at = n;
        CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OUT_OF_MEMORY);
        CHECK(live_allocations == retained);
    }
    fail_at = SIZE_MAX;
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    printf("Record/Nullable compiler/runtime allocation failures %zu/%zu drained\n", total, runtime);
    attempts = 0;
}
int main(void) {
    owned_program_admission();
    record_compiler_failures();
    map_compiler_failures();
    composition_compiler_failures();
    tuple_compiler_failures();
    typed_edge_spill_cycles();
    string_reference_compiler_failures();
    aggregate_call_compiler_failures(false);
    aggregate_call_compiler_failures(true);
    aggregate_state_compiler_failures();
    aggregate_value_compiler_failures(17, false, false);
    aggregate_value_compiler_failures(300, true, false);
    aggregate_value_compiler_failures(17, false, true);
    aggregate_type_projection_failures();
    array_reference_compiler_failures();
    physical_array_copy_cycle();
    array_compiler_failures("string",511);
    array_compiler_failures("large",1500);
    array_compiler_failures("array-spills-string",1500);
    compiled_float_constants();
    jump_threading();
    jump_compaction();
    physical_string_copy_cycle();
    compiled_spills();
    literal_allocation_failures();
    register_workspace_failure();
    source_map_failures();
    XrLirFunction root;
    XrLirModule *m = cell_compiler_module(&root);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    for (size_t f = 0; f < program->module.function_count; ++f) {
        const XrRbcFunction *function = &program->module.functions[f];
        CHECK(function->source_ids && function->source_id_count == function->code_count);
        for (size_t pc = 0; pc < function->code_count; ++pc)
            CHECK(!function->source_ids[pc] ||
                xrt_locations_get(program->locations, function->source_ids[pc]));
    }
    size_t total = attempts;
    xr_rbc_program_destroy(program);
    const size_t failures[] = {1, total / 2, total};
    for (size_t i = 0; i < 3; ++i) {
        attempts = 0; fail_at = failures[i]; program = NULL;
        CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY);
        fail_at = SIZE_MAX;
        CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
        uint64_t n = 12, result = 0;
        CHECK(execute(program, &n, 1, &result) == XR_RBC_OK && result == 144);
        xr_rbc_program_destroy(program);
    }
    program = NULL;
    fail_location_allocation = true;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OUT_OF_MEMORY);
    CHECK(!program && !fail_location_allocation && diagnostic.status == XR_RBC_COMPILE_OUT_OF_MEMORY);
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    CHECK(xr_rbc_program_locations(program));
    xr_rbc_program_destroy(program);
    xr_lir_module_destroy(m);
    view_compiler_failures();
    callable_compiler_failures();
    callable_mutable_frame_storage();
    callable_factory_failures(false);
    callable_factory_failures(true);
    callable_scope_handle_reuse();
    display_compiler_failures();
    compiled_source_maps();
    frame_cfg_compiler_failures();
    CHECK(!live_allocations);
    puts("lir_bytecode_alloc: passed (representative compiler/owner failures, relocating buffers)");
    return 0;
}
