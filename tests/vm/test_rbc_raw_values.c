/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_rbc_raw_values.c - Raw code metadata and independently prepared State
 */
#include "../lir/lir_test_programs.h"
#include "vm/register/xrbc_code_owner.h"
#include "lir/xlir_state.h"
#include "vm/register/xrbc_ctfe.h"
#include "vm/register/xrbc_lir.h"
#include "vm/register/xrbc_value_homes.h"
#include "shared/xr_pod_state.h"
#include "../lir/lir_record_fixture.h"

static void effect(XrLirModule *module, XrLirBlock block, XrLirOp operation,
    const XrLirValue *arguments, uint32_t count, uint64_t bits) {
    XrLirInstruction instruction = {.op=operation,.type=XR_LIR_UNIT,
        .operands=arguments,.operand_count=count,.bits=bits};
    OK(xr_lir_emit_effect(module,block,&instruction));
}
static XrLirModule *state_fixture(XrLirFunction *root) {
    XrLirModule *module = NULL; OK(xr_lir_module_create(NULL,&module));
    XrLirStateModule group = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_I64,.mutable=true,.size=8,.alignment=8};
    XrLirStateLayout layout = {.modules=&group,.module_count=1,.slots=&slot,.slot_count=1,
        .payload_size=8,.payload_alignment=8};
    OK(xr_lir_set_state_layout(module,&layout)); XrLirType parameter = XR_LIR_STATE;
    *root = test_function(module,"state_counter",&parameter,1,XR_LIR_I64);
    OK(xr_lir_set_state_entry(module,*root)); XrLirBlock block = test_block(module,*root,NULL,0);
    OK(xr_lir_set_entry(module,*root,block)); XrLirValue state;
    OK(xr_lir_function_parameter(module,*root,0,&state));
    effect(module,block,XR_LIR_MODULE_BEGIN,&state,1,0);
    XrLirValue arguments[] = {state,test_constant(module,block,XR_LIR_I64,17)}, reference;
    XrLirInstruction construct = {.op=XR_LIR_STATE_CONSTRUCT,.type=XR_LIR_STATE_REF,
        .operands=arguments,.operand_count=2};
    OK(xr_lir_emit_value(module,block,&construct,&reference));
    arguments[0] = reference; arguments[1] = test_constant(module,block,XR_LIR_I64,42);
    effect(module,block,XR_LIR_STATE_WRITE,arguments,2,0);
    effect(module,block,XR_LIR_STATE_PUBLISH,&reference,1,0);
    effect(module,block,XR_LIR_MODULE_READY,&state,1,0);
    XrLirValue result;
    XrLirInstruction read = {.op=XR_LIR_STATE_READ,.type=XR_LIR_I64,.operands=&reference,.operand_count=1};
    OK(xr_lir_emit_value(module,block,&read,&result));
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&result,.operand_count=1}));
    OK(xr_lir_module_finish(module,NULL)); return module;
}
typedef struct CodeForward { XrRbcCodeLease lease; size_t retained, dropped; } CodeForward;
static bool forward_retain(void *context) {
    CodeForward *forward = context;
    if (!forward->lease.retain(forward->lease.owner)) return false;
    ++forward->retained; return true;
}
static void forward_drop(void *context) {
    CodeForward *forward = context; ++forward->dropped; forward->lease.release(forward->lease.owner);
}
static void state_code_retirement(void) {
    XrLirFunction root; XrLirModule *module = state_fixture(&root); XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module,root,&program,NULL) == XR_RBC_COMPILE_OK);
    const XrRbcModule *view = xr_rbc_program_view(program);
    CHECK(view->state_spec && view->state_spec->slot_count == 1);
    const XrtValueImage *image = xr_rbc_module_value_image(view);
    CHECK(image && view->state_spec->values == image);
    uint32_t id = view->state_spec->slots[0].type;
    CHECK(id && id <= image->type_count && image->types[id - 1].kind == XRT_VALUE_I64);
    XrtBillingOwner *billing = NULL; CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&billing) == XRT_MANAGED_OK);
    CodeForward forward = {view->code_lease,0,0}; XrtModuleLayout *layout = NULL;
    CHECK(xrt_module_layout_new(billing,view->state_spec,
        (XrtManagedLease){&forward,forward_retain,forward_drop},&layout) == XRT_MANAGED_OK);
    CHECK(forward.retained && forward.retained == forward.dropped);
    XrRbcExecution *execution = NULL; CHECK(xr_rbc_program_prepare(program,NULL,0,&execution) == XR_RBC_OK);
    XrRbcModule snapshot = *view;
    CHECK(snapshot.code_lease.retain(snapshot.code_lease.owner));
    xr_rbc_program_destroy(program); xr_lir_module_destroy(module);
    CHECK(xr_rbc_module_value_image(&snapshot) == image && snapshot.state_spec->slots[0].type == id);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK);
    uint64_t result = 0; CHECK(xr_rbc_outcome_bits(outcome,&result) && result == 42);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    snapshot.code_lease.release(snapshot.code_lease.owner);
    /* The separately received plans and state no longer borrow any raw image. */
    XrtModuleState *state = NULL; CHECK(xrt_module_state_new(layout,billing,&state) == XRT_MANAGED_OK);
    CHECK(xrt_module_begin(state,0) == XRT_MANAGED_OK); xrt_module_state_drop(&state);
    xrt_module_layout_drop(&layout); CHECK(xrt_billing_owner_stats(billing).blocks == 1);
    xrt_billing_owner_drop(billing);
}
static void scalar_entry_receives_state_descriptions(void) {
    XrLirFunction root; XrLirModule *input = state_fixture(&root); XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(input,root,&program,NULL) == XR_RBC_COMPILE_OK);
    XrRbcModule module = *xr_rbc_program_view(program);
    CHECK(module.function_count == 1 && module.state_spec && module.state_spec->slot_count == 1);
    uint32_t code[2] = {0};
    CHECK(xr_rbc_encode(&code[0],(XrRbcOperands){.op=XR_RBC_LOADI,.immediate=42}));
    CHECK(xr_rbc_encode(&code[1],(XrRbcOperands){.op=XR_RBC_RET}));
    XrRbcFunction functions[2] = {module.functions[0],
        {.code=code,.code_count=2,.register_count=1}};
    module.functions = functions; module.function_count = 2;
    module.code_lease = (XrRbcCodeLease){0};
    CHECK(module.borrowed_value_codes && module.borrowed_value_code_count == 1);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&module,1,NULL,0,xr_rbc_program_locations(program),&execution) == XR_RBC_INVALID && !execution);
    /* The new scalar function has no typed accesses. Extend the real borrowed
     * directory with its absent description instead of reusing a short span. */
    const struct XrRbcValueCode *codes[] = {module.borrowed_value_codes[0],NULL};
    module.borrowed_value_codes = codes; module.borrowed_value_code_count = 2;
    XrtModuleLayoutSpec spec = *module.state_spec;
    XrtValueImage image = *spec.values;
    CHECK(image.type_count && image.type_count <= 64);
    XrtValueImageType types[64]; memcpy(types,image.types,image.type_count * sizeof(*types));
    XrtModuleSlotSpec slot = spec.slots[0];
    CHECK(slot.type && slot.type <= image.type_count && types[slot.type - 1].kind == XRT_VALUE_I64);
    /* Bounded raw spans cannot establish the primitive's natural layout. The
     * actual receiving boundary must still run for a non-State entry. */
    types[slot.type - 1].size = types[slot.type - 1].alignment = 1;
    slot.offset = 0; slot.size = slot.alignment = 1;
    image.types = types; spec.values = &image; spec.slots = &slot;
    spec.payload_size = spec.payload_alignment = 1; module.state_spec = &spec;
    CHECK(xr_rbc_module_prepare(&module,1,NULL,0,xr_rbc_program_locations(program),&execution) != XR_RBC_OK && !execution);
    module.state_spec = xr_rbc_program_view(program)->state_spec;
    CHECK(xr_rbc_module_prepare(&module,1,NULL,0,xr_rbc_program_locations(program),&execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution); uint64_t result = 0;
    CHECK(outcome && xr_rbc_outcome_bits(outcome,&result) && result == 42);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    xr_rbc_program_destroy(program); xr_lir_module_destroy(input);
}
static void scalar_and_mapping(void) {
    XrLirModule *module = NULL; OK(xr_lir_module_create(NULL,&module));
    XrLirFunction root = test_function(module,"scalar",NULL,0,XR_LIR_I64);
    XrLirBlock block = test_block(module,root,NULL,0); OK(xr_lir_set_entry(module,root,block));
    XrLirValue value = test_constant(module,block,XR_LIR_I64,42);
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&value,.operand_count=1}));
    XrLirFunction unused = test_function(module,"unused_string",NULL,0,XR_LIR_STRING);
    XrLirBlock unused_block = test_block(module,unused,NULL,0); OK(xr_lir_set_entry(module,unused,unused_block));
    XrLirLiteral literal = {0}; OK(xr_lir_add_literal(module,"unused",6,&literal));
    XrLirValue text = record_fixture_value(module,unused_block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,
        NULL,0,literal.id,(XrLirFunction){0});
    OK(xr_lir_set_terminator(module,unused_block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&text,.operand_count=1})); OK(xr_lir_module_finish(module,NULL));
    XrRbcProgram *program = NULL; CHECK(xr_rbc_compile(module,root,&program,NULL) == XR_RBC_COMPILE_OK);
    CHECK(!xr_rbc_module_value_image(xr_rbc_program_view(program)) &&
        !xr_rbc_module_value_homes(xr_rbc_program_view(program),0));
    xr_rbc_program_destroy(program); xr_lir_module_destroy(module);
    XrLirModule *samples[] = {test_fib(),test_sum()};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        OK(xr_lir_function_at(samples[i],1,&root)); program = NULL;
        CHECK(xr_rbc_compile(samples[i],root,&program,NULL) == XR_RBC_COMPILE_OK);
        CHECK(!xr_rbc_module_value_image(xr_rbc_program_view(program)) &&
            !xr_rbc_module_value_homes(xr_rbc_program_view(program),0));
        xr_rbc_program_destroy(program); xr_lir_module_destroy(samples[i]);
    }
    XrtValueKind kind = XRT_VALUE_STRING; CHECK(xr_pod_value_kind(XR_POD_UNIT,&kind) && kind == XRT_VALUE_UNIT);
    CHECK(!xr_pod_value_kind(XR_POD_SCALAR_COUNT,&kind) && kind == XRT_VALUE_UNIT);
}
static void inline_targets(void) {
    XrLirFunction root={0}; XrLirModule *module=record_fixture(&root);
    XrRbcProgram *program=NULL; CHECK(xr_rbc_compile(module,root,&program,NULL)==XR_RBC_COMPILE_OK && program);
    XrRbcExecution *execution=NULL; CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    xr_lir_module_destroy(module); xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution); uint64_t bits=0;
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK && xr_rbc_outcome_bits(outcome,&bits) && bits==100);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    module=NULL; OK(xr_lir_module_create(NULL,&module)); XrLirType type=0;
    XrLirRecordField field={"score",5,XR_LIR_I64, 0, 0}; OK(xr_lir_record_type(module,
        &(XrLirRecordIdentity){.module="tests.vm.rbc_raw_values", .module_length=sizeof("tests.vm.rbc_raw_values")-1,
            .name="ExternalRecord", .name_length=sizeof("ExternalRecord")-1},
        &field,1,&type));
    root=test_function(module,"external_record",NULL,0,type); XrLirBlock block=test_block(module,root,NULL,0);
    OK(xr_lir_set_entry(module,root,block)); XrLirValue number=test_constant(module,block,XR_LIR_I64,42);
    XrLirValue value=record_fixture_value(module,block,XR_LIR_RECORD_NEW,type,&number,1,0,(XrLirFunction){0});
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1}));
    OK(xr_lir_module_finish(module,NULL)); program=NULL; XrRbcCompileDiagnostic diagnostic={0};
    CHECK(xr_rbc_compile(module,root,&program,&diagnostic)==XR_RBC_COMPILE_UNSUPPORTED && !program);
    CHECK(!strcmp(diagnostic.message,"inline value results require an internal typed caller")); xr_lir_module_destroy(module);
}
static void ctfe_encode(uint32_t *code, const XrRbcOperands *operations, size_t count) {
    for (size_t index = 0; index < count; ++index) CHECK(xr_rbc_encode(&code[index], operations[index]));
}

static bool ctfe_current(void *context) {
    unsigned *remaining = context;
    if (!*remaining) return false;
    --*remaining;
    return true;
}

static XrtLocationOwner *ctfe_locations(void) {
    char path[] = "ctfe-control.xr";
    XrtSourceLocation points[] = {{path, sizeof(path) - 1, 10, 2, 0, 0},
                                  {path, sizeof(path) - 1, 20, 4, 0, 0}};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(points, 2, &owner) == XRT_LOCATION_OK);
    memset(path, 'x', sizeof(path));
    return owner;
}

static void ctfe_fuel_and_bounds(void) {
    uint32_t loop[8], add[2];
    ctfe_encode(loop, (XrRbcOperands[]){{.op=XR_RBC_LOADI, .a=1}, {.op=XR_RBC_MOV, .a=3},
        {.op=XR_RBC_CALL, .a=2, .immediate=1}, {.op=XR_RBC_ADD, .a=1, .b=1, .c=2},
        {.op=XR_RBC_SUBI, .immediate=1}, {.op=XR_RBC_GTI}, {.op=XR_RBC_JMP}, {.op=XR_RBC_RET, .a=1}}, 8);
    CHECK(xr_rbc_jump(&loop[6], 6, 1, 8));
    ctfe_encode(add, (XrRbcOperands[]){{.op=XR_RBC_ADDI, .a=1}, {.op=XR_RBC_RET, .a=1}}, 2);
    uint32_t source_ids[] = {1, 1, 1, 1, 1, 1, 2, 1};
    XrRbcFunction functions[] = {{.code=loop, .code_count=8, .register_count=4, .parameter_count=1,
                                  .source_ids=source_ids, .source_id_count=8},
                                 {.code=add, .code_count=2, .register_count=2, .parameter_count=1}};
    uint64_t arg = 3;
    XrtLocationOwner *locations = ctfe_locations();
    XrRbcCtfeRequest request = {.functions=functions, .function_count=2, .arguments=&arg,
        .argument_count=1, .locations=locations, .code_words=10, .memory_bytes=4096, .fuel=6};
    XrRbcCtfeOutcome output = {0};
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_OK && output.bits == 6 && !output.locations);
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_INVALID && output.bits == 6);
    xr_rbc_ctfe_outcome_free(&output);

    request.fuel = 5;
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_FUEL_LIMIT && !output.bits && output.source_id == 1);
    xr_rbc_ctfe_outcome_free(&output);
    request.fuel = 4;
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_FUEL_LIMIT && !output.bits && output.source_id == 2);
    CHECK(xrt_locations_get(output.locations, output.source_id)->line == 20);
    xr_rbc_ctfe_outcome_free(&output);
    request.fuel = 0;
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_FUEL_LIMIT && output.source_id == 1);
    xr_rbc_ctfe_outcome_free(&output);

    request.fuel = 6;
    request.code_words = 9;
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_CODE_LIMIT && !output.bits && !output.locations);
    request.code_words = 10;
    request.memory_bytes = 1;
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_MEMORY_LIMIT && output.source_id == 1);
    xr_rbc_ctfe_outcome_free(&output);
    request.memory_bytes = 4096;
    unsigned polls = 3;
    XrtHostExecutionControl control = {ctfe_current, &polls};
    request.control = &control;
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_CANCELLED && !output.bits && output.source_id == 2);
    xr_rbc_ctfe_outcome_free(&output);
    xrt_locations_release(locations);
}

static void ctfe_recursive_growth(void) {
    uint32_t code[8];
    ctfe_encode(code, (XrRbcOperands[]){{.op=XR_RBC_EQI}, {.op=XR_RBC_JMP},
        {.op=XR_RBC_SUBI, .a=2, .immediate=1}, {.op=XR_RBC_CALL, .a=1},
        {.op=XR_RBC_ADDI, .a=1, .b=1, .immediate=1}, {.op=XR_RBC_RET, .a=1},
        {.op=XR_RBC_LOADI, .a=1}, {.op=XR_RBC_RET, .a=1}}, 8);
    CHECK(xr_rbc_jump(&code[1], 1, 6, 8));
    uint32_t source_ids[] = {1, 1, 1, 2, 1, 1, 1, 1};
    XrRbcFunction function = {.code=code, .code_count=8, .register_count=3, .parameter_count=1,
                              .source_ids=source_ids, .source_id_count=8};
    uint64_t arg = 50;
    XrtLocationOwner *locations = ctfe_locations();
    XrRbcCtfeRequest request = {.functions=&function, .function_count=1, .arguments=&arg,
        .argument_count=1, .locations=locations, .code_words=8, .memory_bytes=256, .fuel=51};
    XrRbcCtfeOutcome output = {0};
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_MEMORY_LIMIT && output.source_id == 2);
    xr_rbc_ctfe_outcome_free(&output);
    request.memory_bytes = 16384;
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_OK && output.bits == arg);
    xr_rbc_ctfe_outcome_free(&output);
    xrt_locations_release(locations);
}

static void ctfe_admission_and_panic(void) {
    uint32_t code[2], output_call[2];
    ctfe_encode(code, (XrRbcOperands[]){{.op=XR_RBC_DIV, .c=1}, {.op=XR_RBC_RET}}, 2);
    ctfe_encode(output_call, (XrRbcOperands[]){{.op=XR_RBC_CALLN}, {.op=XR_RBC_RET0}}, 2);
    uint32_t source_ids[] = {2, 1};
    XrRbcFunction functions[] = {{.code=code, .code_count=2, .register_count=2, .parameter_count=2,
                                  .source_ids=source_ids, .source_id_count=2},
                                {.code=output_call, .code_count=2, .register_count=3,
                                  .source_ids=source_ids, .source_id_count=2}};
    XrtLocationOwner *locations = ctfe_locations();
    XrRbcNativeKind native = XR_RBC_NATIVE_PRINT_I64;
    XrRbcModule ordinary = {.functions=functions, .function_count=2, .natives=&native, .native_count=1};
    uint64_t args[] = {(uint64_t)INT64_MIN, 0};
    XrRbcCtfeRequest request = {.functions=functions, .function_count=2, .arguments=args,
        .argument_count=2, .locations=locations, .code_words=4, .memory_bytes=4096, .fuel=1};
    XrRbcCtfeOutcome output = {0};
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_UNSUPPORTED && output.source_id == 2 && !output.panic_code);
    xr_rbc_ctfe_outcome_free(&output);
    request.function_count = 1;
    source_ids[0] = 3;
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_INVALID && !output.locations);
    source_ids[0] = 2;
    uint32_t division = code[0];
    CHECK(xr_rbc_encode(code, (XrRbcOperands){.op=XR_RBC_FADD, .c=1}));
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_UNSUPPORTED && output.source_id == 2);
    xr_rbc_ctfe_outcome_free(&output);
    code[0] = division;
    code[1] = XR_RBC_JMP | (1u << 8);
    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_INVALID && !output.locations);
    code[1] = XR_RBC_RET;

    CHECK(xr_rbc_ctfe_evaluate(&request, &output) == XR_RBC_CTFE_PANIC && output.panic_code == 420 && !output.bits);
    XrRbcCtfeOutcome success = {0};
    args[1] = UINT64_MAX;
    CHECK(xr_rbc_ctfe_evaluate(&request, &success) == XR_RBC_CTFE_OK && success.bits == (uint64_t)INT64_MIN);
    xr_rbc_ctfe_outcome_free(&success);
    XrRbcExecution *execution = NULL;
    // Ordinary preparation also admits the unreachable native caller.
    CHECK(xr_rbc_module_prepare(&ordinary, 0, args, 2, locations, &execution) == XR_RBC_OK);
    XrRbcOutcome *result = xr_rbc_execution_run(execution);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_bits(result, &bits) && bits == (uint64_t)INT64_MIN);
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&result);

    xrt_locations_release(locations);
    memset(code, 0, sizeof(code));
    memset(source_ids, 0, sizeof(source_ids));
    const XrtSourceLocation *point = xrt_locations_get(output.locations, output.source_id);
    CHECK(point && point->line == 20 && point->column == 4 && point->path_length == 15 &&
        memcmp(point->path, "ctfe-control.xr", 15) == 0);
    xr_rbc_ctfe_outcome_free(&output);
    CHECK(!output.bits && !output.source_id && !output.panic_code && !output.locations);
}

int main(void) {
    scalar_and_mapping(); inline_targets(); state_code_retirement(); scalar_entry_receives_state_descriptions();
    ctfe_fuel_and_bounds(); ctfe_recursive_growth(); ctfe_admission_and_panic();
    puts("raw CodeImage State metadata and independent prepare passed"); return 0;
}
