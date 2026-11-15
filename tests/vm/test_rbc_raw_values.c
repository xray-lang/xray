/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_rbc_raw_values.c - Raw code metadata and independently prepared State
 */
#include "../lir/lir_test_programs.h"
#include "lir/xlir_state.h"
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
    XrRbcExecution *execution = NULL;
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
    XrLirRecordField field={"score",5,XR_LIR_I64}; OK(xr_lir_record_type(module,&field,1,&type));
    root=test_function(module,"external_record",NULL,0,type); XrLirBlock block=test_block(module,root,NULL,0);
    OK(xr_lir_set_entry(module,root,block)); XrLirValue number=test_constant(module,block,XR_LIR_I64,42);
    XrLirValue value=record_fixture_value(module,block,XR_LIR_RECORD_NEW,type,&number,1,0,(XrLirFunction){0});
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1}));
    OK(xr_lir_module_finish(module,NULL)); program=NULL; XrRbcCompileDiagnostic diagnostic={0};
    CHECK(xr_rbc_compile(module,root,&program,&diagnostic)==XR_RBC_COMPILE_UNSUPPORTED && !program);
    CHECK(!strcmp(diagnostic.message,"inline value results require an internal typed caller")); xr_lir_module_destroy(module);
}
int main(void) {
    scalar_and_mapping(); inline_targets(); state_code_retirement(); scalar_entry_receives_state_descriptions();
    puts("raw CodeImage State metadata and independent prepare passed"); return 0;
}
