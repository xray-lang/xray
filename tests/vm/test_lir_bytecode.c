/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_bytecode.c - Hand-built SSA execution, ownership and capacity tests.
 */
#include "../lir/lir_test_programs.h"
#include "vm/register/xrbc_code_owner.h"
#include "../lir/lir_text_call_fixture.h"
#include "base/xmalloc.h"
#include "lir/xlir_internal.h"
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_values.h"
#include "shared/xr_value_format_core.h"
#include "vm/register/xrbc_lir.h"
#include "vm/register/xrbc_value_code.h"
#include "xray_native_string_value.h"


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

static XrRbcProgram *compile(XrLirModule *m, XrLirFunction root) {
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    XrRbcCompileStatus status = xr_rbc_compile(m, root, &program, &diagnostic);
    if (status != XR_RBC_COMPILE_OK) fprintf(stderr, "compile f%u b%u i%u status%u %s\n",
        diagnostic.function, diagnostic.block, diagnostic.instruction, (unsigned)status, diagnostic.message);
    CHECK(status == XR_RBC_COMPILE_OK);
    CHECK(program && diagnostic.status == XR_RBC_COMPILE_OK);
    return program;
}

static uint64_t run(XrRbcProgram *program, const uint64_t *arguments, size_t count) {
    uint64_t result = UINT64_MAX;
    CHECK(execute(program, arguments, count, &result) == XR_RBC_OK);
    return result;
}

static XrLirValue parameter(XrLirModule *m, XrLirFunction f, uint32_t index) {
    XrLirValue value;
    OK(xr_lir_function_parameter(m, f, index, &value));
    return value;
}

static XrLirValue block_parameter(XrLirModule *m, XrLirBlock b, uint32_t index) {
    XrLirValue value;
    OK(xr_lir_block_parameter(m, b, index, &value));
    return value;
}

static void test_directory_frame_after_entry_jump(void) {
    const XrLirSource read = {"count.xr", 8, 12, 7, 12, 7};
    const XrLirSource close = {"count.xr", 8, 9, 5, 9, 5};
    const XrLirSource returned = {"count.xr", 8, 15, 9, 15, 9};
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType scalar = XR_LIR_I64, status = XR_LIR_U64, optional = XR_LIR_TYPE_INVALID;
    OK(xr_lir_nullable_type(m, XR_LIR_STRING, &optional));
    XrLirFunction caller = test_function(m, "caller", NULL, 0, scalar);
    XrLirFunction count = test_function(m, "count", NULL, 0, scalar), cleanup = {0};
    OK(xr_lir_declare_function(m, &(XrLirFunctionSpec){.name="cleanup",.name_length=7,
        .parameters=&scalar,.parameter_count=1,.result=XR_LIR_UNIT,.cleanup_owner=count.id}, &cleanup));
    OK(xr_lir_define_function(m, cleanup));
    XrLirBlock body = test_block(m, cleanup, NULL, 0);
    OK(xr_lir_set_entry(m, cleanup, body));
    XrLirValue formal = parameter(m, cleanup, 0);
    OK(xr_lir_emit_effect(m, body, &(XrLirInstruction){.op=XR_LIR_HOST_DIRECTORY_CLEANUP_CLOSE,
        .type=XR_LIR_UNIT,.operands=&formal,.operand_count=1,.source=close}));
    test_return(m, body, (XrLirValue){0});

    XrLirBlock normal = test_block(m, count, &optional, 1);
    XrLirBlock failure = test_block(m, count, &status, 1);
    XrLirBlock active = test_block(m, count, NULL, 0), eof = test_block(m, count, NULL, 0);
    XrLirBlock entry = test_block(m, count, NULL, 0);
    OK(xr_lir_set_entry(m, count, entry));
    OK(xr_lir_emit_effect(m, entry, &(XrLirInstruction){.op=XR_LIR_HOST_DIRECTORY_FRAME,
        .type=XR_LIR_UNIT,.source=read}));
    XrLirValue token = test_constant(m, entry, scalar, 0);
    OK(xr_lir_internal_cleanup_block(m, entry, 0, cleanup.id, &token, 1));
    test_branch(m, entry, active, NULL, 0);
    OK(xr_lir_internal_cleanup_block(m, active, cleanup.id, 0, NULL, 0));
    OK(xr_lir_set_terminator(m, active, &(XrLirInstruction){.op=XR_LIR_HOST_DIRECTORY_NEXT_CHECKED,
        .type=optional,.operands=&token,.operand_count=1,.edge_count=2,
        .edges={{.target=normal},{.target=failure}},.source=read}));

    OK(xr_lir_internal_cleanup_block(m, normal, cleanup.id, 0, NULL, 0));
    XrLirValue name = block_parameter(m, normal, 0), some = {0};
    OK(xr_lir_emit_value(m, normal, &(XrLirInstruction){.op=XR_LIR_NULLABLE_IS_SOME,
        .type=XR_LIR_I1,.operands=&name,.operand_count=1,.source=read}, &some));
    OK(xr_lir_emit_effect(m, normal, &(XrLirInstruction){.op=XR_LIR_VALUE_DROP,
        .type=XR_LIR_UNIT,.operands=&name,.operand_count=1,.source=read}));
    OK(xr_lir_set_terminator(m, normal, &(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&some,.operand_count=1,.edge_count=2,.edges={{.target=active},{.target=eof}}}));

    XrLirInstruction retire = {.op=XR_LIR_HOST_DIRECTORY_CLOSE_CHECKED,.type=status,
        .bits=cleanup.id,.operands=&token,.operand_count=1,.source=close};
    OK(xr_lir_internal_cleanup_block(m, eof, cleanup.id, 0, NULL, 0));
    XrLirValue closed = {0};
    OK(xr_lir_emit_value(m, eof, &retire, &closed));
    OK(xr_lir_emit_effect(m, eof, &(XrLirInstruction){.op=XR_LIR_HOST_DIRECTORY_RETURN_GUARD,
        .type=XR_LIR_UNIT,.operands=&closed,.operand_count=1,.source=returned}));
    OK(xr_lir_set_terminator(m, eof, &(XrLirInstruction){.op=XR_LIR_RET,
        .type=XR_LIR_UNIT,.operands=&token,.operand_count=1,.source=returned}));
    XrLirValue failed[2] = {block_parameter(m, failure, 0), {0}};
    OK(xr_lir_emit_value(m, failure, &retire, &failed[1]));
    OK(xr_lir_set_terminator(m, failure, &(XrLirInstruction){.op=XR_LIR_HOST_DIRECTORY_FAIL,
        .type=XR_LIR_UNIT,.operands=failed,.operand_count=2,.source=read}));

    XrLirBlock call = test_block(m, caller, NULL, 0);
    OK(xr_lir_set_entry(m, caller, call));
    XrLirValue value = {0};
    OK(xr_lir_emit_value(m, call, &(XrLirInstruction){.op=XR_LIR_CALL,
        .type=scalar,.callee=count}, &value));
    test_return(m, call, value);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, caller);
    xr_lir_module_destroy(m);

    const XrRbcModule *module = xr_rbc_program_view(program);
    const XrRbcFunction *count_code = NULL;
    for (uint32_t n = 0; n < module->function_count; ++n) {
        const XrRbcValueCode *values = xr_rbc_module_value_code(module, n);
        if (values && values->directory_frame_source) count_code = &module->functions[n];
    }
    CHECK(count_code && (count_code->code[0] & 255u) == XR_RBC_JMP);
    size_t marker = 1 + (count_code->code[0] >> 8);
    CHECK(marker > 1 && xr_rbc_value_instruction_valid(module, count_code, marker));
    uint32_t *words = (uint32_t *)(void *)count_code->code;
    uint32_t original = words[0];
    const uint32_t wrong_entries[] = {XR_RBC_RET0, XR_RBC_JMP,
        XR_RBC_JMP | ((uint32_t)XR_RBC_JUMP_MAX << 8), XR_RBC_JMP | UINT32_C(0xfffffe00)};
    for (size_t n = 0; n < sizeof(wrong_entries) / sizeof(*wrong_entries); ++n) {
        words[0] = wrong_entries[n];
        CHECK(!xr_rbc_value_instruction_valid(module, count_code, marker));
    }
    words[0] = original;
    CHECK(xr_rbc_value_instruction_valid(module, count_code, marker));

    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    XrXirValueStatus fault = XR_XIR_VALUE_OK;
    uint32_t source = 0;
    const XrtLocationOwner *locations = NULL;
    CHECK(xr_rbc_outcome_value_failure(outcome, &fault, &source, &locations));
    const XrtSourceLocation *location = xrt_locations_get(locations, source);
    CHECK(fault == XR_XIR_VALUE_BAD_ARGUMENT && location &&
        location->line == read.line && location->column == read.column);
    CHECK(xr_rbc_execution_free(&execution) == XR_OS_IO_OK);
    xr_rbc_outcome_free(&outcome);
    xr_rbc_program_destroy(program);
}

static void test_raw_typed_error_rejected(void) {
    XrtLocationOwner *locations=NULL;
    CHECK(xrt_locations_create(NULL,0,&locations)==XRT_LOCATION_OK);
    uint32_t word=XR_RBC_RET0;
    XrRbcRegisterKind kind=XR_RBC_BITS;
    XrRbcFunction function={.code=&word,.code_count=1,.register_count=1,
        .register_kinds=&kind,.register_kind_count=1};
    XrRbcValueLocation value_location={0};
    XrRbcValueCode code={.locations=&value_location};
    const XrRbcValueCode *borrowed[]={&code};
    XrRbcModule module={.functions=&function,.function_count=1,
        .borrowed_value_codes=borrowed,.borrowed_value_code_count=1};
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_OK && execution);
    xr_rbc_execution_free(&execution);
    code.error_type=1;
    CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_INVALID && !execution);
    code.error_type=0;
    XrRbcValueOperation typed[]={XR_RBC_VALUE_INVOKE,XR_RBC_VALUE_INVOKE_RESULT,
        XR_RBC_VALUE_INVOKE_ERROR,XR_RBC_VALUE_RETURN_ERROR,XR_RBC_VALUE_PANIC_ERROR,XR_RBC_VALUE_UNCAUGHT_ERROR};
    XrRbcValueAccess access={0};
    code.accesses=&access;
    code.access_count=1;
    for (size_t n=0;n<sizeof(typed)/sizeof(*typed);++n) {
        access.operation=typed[n];
        CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&execution)==XR_RBC_INVALID && !execution);
    }
    xrt_locations_release(locations);
}

typedef struct NativeError {
    const XrRbcLiteral *metadata;
    struct { uint8_t tag; int64_t code; } value;
    bool uncaught;
} NativeError;

static void native_error_root(void *context) {
    const NativeError *error = context;
    const uint8_t *metadata = (const uint8_t *)error->metadata->bytes;
    if (error->uncaught)
        xrt_uncaught_error(metadata, error->metadata->length, &error->value, sizeof(error->value));
    xrt_try_assert_panic(metadata, error->metadata->length, &error->value, sizeof(error->value), 1);
}

static void native_error_reenter(void *context) { ++*(unsigned *)context; }

static void test_native_error_snapshot(const XrRbcLiteral *metadata, bool uncaught) {
    const XrtSourceLocation location={"native-try.xr",13,15,12,15,23};
    size_t baseline=0;
    for (unsigned mode=0;mode<3;++mode) {
        XrtValueRoot root={0};
        XrtBillingOwner *billing=NULL;
        CHECK(xrt_native_values_prepare(&root,&billing)==XRT_MANAGED_OK && xrt_billing_owner_retain(billing));
        XrtLocationOwner *map=NULL;
        CHECK(xrt_locations_create(&location,1,&map)==XRT_LOCATION_OK);
        NativeError error={.metadata=metadata,.value={(uint8_t)(mode==0 ? 2u : 1u),23},.uncaught=uncaught};
        if (mode==1) xrt_billing_owner_fail_after(billing,0);
        XrtExecutionFailure failure={0};
        CHECK(xrt_run_embedded_owned(map,&failure,native_error_root,&error,&root)==XRT_EXECUTION_FAILURE);
        CHECK(xrt_value_root_empty(&root) && !xrt_value_root_active());
        unsigned again=0;
        XrtExecutionFailure unused={0};
        CHECK(xrt_run_embedded(map,&unused,native_error_reenter,&again)==XRT_EXECUTION_COMPLETED && again==1);
        xrt_locations_release(map);
        if (mode<2) {
            CHECK(failure.kind==XRT_FAILURE_VALUE && failure.detail.value.source_id==(uncaught ? 0u : 1u) &&
                failure.detail.value.status==(mode==0 ? XR_XIR_VALUE_BAD_ARGUMENT : XR_XIR_VALUE_OOM));
            CHECK(xrt_billing_owner_stats(billing).blocks==1);
            if (mode==0) baseline=xrt_billing_owner_stats(billing).bytes;
        } else {
            XrtExecutionFailureKind kind=uncaught ? XRT_FAILURE_UNCAUGHT_ERROR : XRT_FAILURE_TRY_ASSERT;
            CHECK(failure.kind==kind && xrt_billing_owner_stats(billing).blocks==2);
            const XrtErrorSnapshot *snapshot=uncaught ? failure.detail.error : failure.detail.assertion;
            const XrtSourceLocation *where=xrt_try_assert_location(snapshot);
            XrtTryAssertValue value={0};
            uint32_t scalar=0;
            uint64_t bits=0;
            if (uncaught) CHECK(!where);
            else CHECK(where && where->line==15 && where->column==12 && where->path_length==13 &&
                !memcmp(where->path,"native-try.xr",13));
            CHECK(xrt_try_assert_value(snapshot,&value) && value.variant==1 && value.field_count==1 &&
                value.metadata_size==metadata->length && !memcmp(value.metadata,metadata->bytes,metadata->length));
            CHECK(xrt_try_assert_field(snapshot,0,&scalar,&bits) && scalar==XR_POD_I64 && bits==23);
        }
        xrt_execution_failure_drop(&failure);
        xrt_execution_failure_drop(&failure);
        XrtBillingStats stats=xrt_billing_owner_stats(billing);
        CHECK(stats.blocks==1 && stats.bytes==baseline);
        xrt_billing_owner_drop(billing);
    }
}

static XrLirType test_nominal_error(XrLirModule *m, XrLirLiteral *metadata) {
    XrLirRecordIdentity key = {.module="fixture",.module_length=7,.name="ProbeErr",.name_length=8};
    XrLirRecordField field = {.name="code",.name_length=4,.type=XR_LIR_I64};
    XrLirSumVariant variants[] = {{.name="Missing",.name_length=7},
        {.name="Bad",.name_length=3,.field_count=1}};
    XrLirType type;
    OK(xr_lir_error_sum_type(m,&key,&field,1,variants,2,&type));
    OK(xr_lir_error_literal(m,type,metadata));
    return type;
}

static void test_cleanup_root_authority(bool typed) {
    XrLirModule *m=NULL;
    OK(xr_lir_module_create(NULL,&m));
    XrLirType scalar=XR_LIR_I64;
    XrLirFunction owner=test_function(m,"owner",NULL,0,scalar),cleanup={0};
    OK(xr_lir_declare_function(m,&(XrLirFunctionSpec){.name="cleanup",.name_length=7,
        .parameters=&scalar,.parameter_count=1,.result=XR_LIR_UNIT,.cleanup_owner=owner.id},&cleanup));
    OK(xr_lir_define_function(m,cleanup));
    XrLirBlock body=test_block(m,cleanup,NULL,0);
    OK(xr_lir_set_entry(m,cleanup,body));
    if (typed) {
        XrLirLiteral metadata={0};
        XrLirType error_type=test_nominal_error(m,&metadata);
        XrLirValue payload=parameter(m,cleanup,0),value={0};
        OK(xr_lir_emit_value(m,body,&(XrLirInstruction){.op=XR_LIR_SUM_NEW,.type=error_type,
            .operands=&payload,.operand_count=1,.bits=1},&value));
    }
    test_return(m,body,(XrLirValue){0});
    XrLirBlock registration=test_block(m,owner,NULL,0),retire=test_block(m,owner,NULL,0);
    OK(xr_lir_set_entry(m,owner,registration));
    XrLirValue saved=test_constant(m,registration,scalar,17);
    OK(xr_lir_internal_cleanup_block(m,registration,0,cleanup.id,&saved,1));
    test_branch(m,registration,retire,NULL,0);
    OK(xr_lir_internal_cleanup_block(m,retire,cleanup.id,0,NULL,0));
    OK(xr_lir_emit_effect(m,retire,&(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_UNIT,
        .callee=cleanup,.operands=&saved,.operand_count=1}));
    test_return(m,retire,saved);
    OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(m,cleanup,&program,NULL)==XR_RBC_COMPILE_INVALID && !program);
    program=compile(m,owner);
    xr_lir_module_destroy(m);
    const XrRbcModule *module=xr_rbc_program_view(program);
    XrtLocationOwner *locations=xr_rbc_program_locations(program);
    CHECK(module->function_count==2 && !module->functions[0].cleanup_only &&
        module->functions[1].cleanup_only && module->functions[1].parameter_count==1 &&
        (xr_rbc_module_value_code(module,1)!=NULL)==typed);
    CHECK(run(program,NULL,0)==17);
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(module,0,NULL,0,locations,&execution)==XR_RBC_OK && execution);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    uint64_t bits=0;
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_OK);
    xr_rbc_outcome_bits(outcome,&bits);
    CHECK(bits==17);
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
    uint64_t argument=17;
    CHECK(xr_rbc_module_prepare(module,1,&argument,1,locations,&execution)==XR_RBC_INVALID && !execution);

    if (!typed) {
        XrRbcModule raw={.functions=module->functions,.function_count=module->function_count};
        CHECK(xr_rbc_module_prepare(&raw,1,&argument,1,locations,&execution)==XR_RBC_INVALID && !execution);
        uint32_t word=XR_RBC_RET0;
        XrRbcFunction ordinary={.code=&word,.code_count=1,.register_count=1};
        raw=(XrRbcModule){.functions=&ordinary,.function_count=1};
        CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,locations,&execution)==XR_RBC_OK && execution);
        xr_rbc_execution_free(&execution);
    }
    xr_rbc_program_destroy(program);
}

static void reject_forged_error_literal(const XrRbcProgram *program, const uint64_t *arguments, size_t count) {
    const XrRbcModule *module = xr_rbc_program_view(program);
    CHECK(module->literal_count==1 && module->literals && module->code_lease.owner);
    XrRbcLiteral literal = module->literals[0];
    char *forged = xr_malloc(literal.length);
    CHECK(forged && literal.length>47);
    memcpy(forged,literal.bytes,literal.length);
    memcpy(forged+39,"FakeType",8);
    literal.bytes=forged;
    XrRbcModule fake=*module;
    fake.literals=&literal; /* The old lease cannot attest new diagnostic bytes. */
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(&fake,0,arguments,count,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID &&
        !execution);
    xr_free(forged);
}

static XrLirFunction test_throwing_leaf(XrLirModule *m, XrLirType error_type, XrLirType result_type) {
    XrLirType flag=XR_LIR_I1;
    XrLirFunction leaf={0};
    XrLirFunctionSpec spec={.name="leaf",.name_length=4,.parameters=&flag,.parameter_count=1,
        .result=result_type,.error=error_type};
    OK(xr_lir_declare_function(m,&spec,&leaf));
    OK(xr_lir_define_function(m,leaf));
    XrLirBlock leaf_entry=test_block(m,leaf,NULL,0),leaf_ok=test_block(m,leaf,NULL,0),leaf_error=test_block(m,leaf,NULL,0);
    OK(xr_lir_set_entry(m,leaf,leaf_entry));
    XrLirValue fail=parameter(m,leaf,0);
    XrLirInstruction select={.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&fail,.operand_count=1,
        .edges={{.target=leaf_error},{.target=leaf_ok}},.edge_count=2};
    OK(xr_lir_set_terminator(m,leaf_entry,&select));
    XrLirValue result={0};
    if (result_type==XR_LIR_ARRAY) {
        XrLirValue byte=test_constant(m,leaf_ok,XR_LIR_U8,17);
        uint32_t constructor=0;
        OK(xr_lir_set_array_result(m,leaf,&(XrLirArrayShape){XR_LIR_U8}));
        OK(xr_lir_array_operation(m,XR_LIR_U8,XR_LIR_ARRAY_OP_NEW,&constructor));
        OK(xr_lir_emit_value(m,leaf_ok,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,
            .intrinsic=constructor,.operands=&byte,.operand_count=1},&result));
    } else {
        result=test_constant(m,leaf_ok,XR_LIR_I64,17);
        if (result_type!=XR_LIR_I64) {
            XrLirValue payload=result;
            XrLirOp op=result_type==XR_LIR_STRING ? XR_LIR_STRING_I64 : XR_LIR_NULLABLE_SOME;
            OK(xr_lir_emit_value(m,leaf_ok,&(XrLirInstruction){.op=op,.type=result_type,
                .operands=&payload,.operand_count=1},&result));
        }
    }
    test_return(m,leaf_ok,result);
    XrLirValue spelling=test_constant(m,leaf_error,XR_LIR_I64,23),value={0};
    XrLirInstruction make={.op=XR_LIR_SUM_NEW,.type=error_type,.bits=1,.operands=&spelling,.operand_count=1};
    OK(xr_lir_emit_value(m,leaf_error,&make,&value));
    OK(xr_lir_set_terminator(m,leaf_error,&(XrLirInstruction){.op=XR_LIR_RETURN_ERROR,
        .type=XR_LIR_UNIT,.operands=&value,.operand_count=1}));
    return leaf;
}

static void test_owned_try_assert(void) {
    XrLirModule *m=NULL;
    OK(xr_lir_module_create(NULL,&m));
    XrLirLiteral metadata={0};
    XrLirType type=test_nominal_error(m,&metadata),flag=XR_LIR_I1,i64=XR_LIR_I64;
    XrLirFunction leaf=test_throwing_leaf(m,type,i64);
    XrLirFunction asserting=test_function(m,"asserting",&flag,1,i64);
    XrLirBlock entry=test_block(m,asserting,NULL,0),ok=test_block(m,asserting,&i64,1);
    XrLirBlock error=test_block(m,asserting,&type,1);
    OK(xr_lir_set_entry(m,asserting,entry));
    XrLirValue fail=parameter(m,asserting,0);
    OK(xr_lir_set_terminator(m,entry,&(XrLirInstruction){.op=XR_LIR_INVOKE,.type=i64,.callee=leaf,
        .operands=&fail,.operand_count=1,.edges={{.target=ok},{.target=error}},.edge_count=2}));
    test_return(m,ok,block_parameter(m,ok,0));
    XrLirValue value=block_parameter(m,error,0);
    const char *path="ownership.xr";
    OK(xr_lir_set_terminator(m,error,&(XrLirInstruction){.op=XR_LIR_PANIC_ERROR,.type=XR_LIR_UNIT,
        .operands=&value,.operand_count=1,.bits=metadata.id,.source={path,12,15,12,15,23}}));
    XrLirFunction root=asserting;
    for (uint32_t n=0;n<32;++n) {
        char name[24];
        CHECK(snprintf(name,sizeof(name),"parent-%u",n)>0);
        XrLirFunction caller=test_function(m,name,&flag,1,i64);
        XrLirBlock body=test_block(m,caller,NULL,0);
        OK(xr_lir_set_entry(m,caller,body));
        fail=parameter(m,caller,0);
        XrLirInstruction call_site={.op=XR_LIR_CALL,.type=i64,.callee=root,.operands=&fail,.operand_count=1,
            .source={path,12,19+n,12,19+n,23}};
        OK(xr_lir_emit_value(m,body,&call_site,&value));
        test_return(m,body,value);
        root=caller;
    }
    OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *rejected=NULL;
    CHECK(xr_rbc_compile(m,leaf,&rejected,NULL)==XR_RBC_COMPILE_UNSUPPORTED && !rejected);
    XrRbcProgram *program=compile(m,root);
    xr_lir_module_destroy(m);
    uint64_t argument=0;
    CHECK(run(program,&argument,1)==17);
    const XrRbcModule *module=xr_rbc_program_view(program);
    XrtLocationOwner *locations=xr_rbc_program_locations(program);
    CHECK(module->literal_count==1 && module->literals && module->code_lease.owner);
    XrRbcLiteral literal=module->literals[0];
    test_native_error_snapshot(&literal, false);
    test_native_error_snapshot(&literal, true);

    size_t count=xrt_locations_count(locations);
    XrtSourceLocation *forged_points=xr_malloc(count*sizeof(*forged_points));
    CHECK(count && forged_points);
    for (size_t n=0;n<count;++n)
        forged_points[n]=(XrtSourceLocation){"forged.xr",9,999,7,999,7};
    XrtLocationOwner *forged_map=NULL;
    CHECK(xrt_locations_create(forged_points,count,&forged_map)==XRT_LOCATION_OK);
    xr_free(forged_points);
    XrRbcExecution *forged_execution=NULL;
    CHECK(xr_rbc_module_prepare(module,0,&argument,1,forged_map,&forged_execution)==XR_RBC_INVALID &&
        !forged_execution);
    xrt_locations_release(forged_map);

    reject_forged_error_literal(program,&argument,1);
    for (uint32_t n=0;n<module->function_count;++n) {
        const XrRbcValueCode *code=xr_rbc_module_value_code(module,n);
        if (code && code->error_type) {
            XrRbcExecution *entry_execution=NULL;
            CHECK(xr_rbc_module_prepare(module,n,&argument,1,locations,&entry_execution)==XR_RBC_INVALID &&
                !entry_execution);
        }
    }
    argument=1;
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,&argument,1,&execution)==XR_RBC_OK);
    xr_rbc_execution_enable_try_trace(execution);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_TRY_ASSERT);
    const XrtTryAssertFailure *failure=xr_rbc_outcome_try_assert(outcome);
    const XrtSourceLocation *where=xrt_try_assert_location(failure);
    CHECK(where && where->line==15 && where->column==12 && where->path_length==12 &&
        !memcmp(where->path,path,12));
    CHECK(xr_rbc_outcome_try_trace_count(outcome)==33);
    CHECK(xr_rbc_outcome_try_trace_at(outcome,1)->line==19);
    CHECK(xr_rbc_outcome_try_trace_at(outcome,32)->line==50);
    XrtTryAssertValue owned={0};
    CHECK(xrt_try_assert_value(failure,&owned) && owned.variant==1 && owned.field_count==1 && owned.metadata_size>40);
    CHECK(xr_error_metadata_u32(owned.metadata+4)==type && !memcmp(owned.metadata+39,"ProbeErr",8));
    uint32_t scalar=0; uint64_t bits=0;
    CHECK(xrt_try_assert_field(failure,0,&scalar,&bits) && scalar==XR_POD_I64 && bits==23);
    CHECK(!xrt_try_assert_field(failure,1,&scalar,&bits) && bits==23);
    xr_rbc_outcome_free(&outcome);
    CHECK(!outcome);
}

static void test_nullable_invoke_type_identity(const XrRbcProgram *program) {
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    CHECK(image && image->types && module->code_lease.owner);
    uint32_t other_type=0;
    for (uint32_t n=0;n<image->type_count;++n) {
        const XrtValueImageType *row=&image->types[n];
        if (row->kind==XRT_VALUE_NULLABLE && row->nullable_element && row->nullable_element<=image->type_count &&
            image->types[row->nullable_element-1].kind==XRT_VALUE_U64)
            other_type=n+1;
    }
    CHECK(other_type);
    for (uint32_t n=0;n<module->function_count;++n) {
        const XrRbcFunction *function=&module->functions[n];
        const XrRbcValueCode *code=xr_rbc_module_value_code(module,n);
        if (!code) continue;
        for (size_t pc=0;pc<function->code_count;++pc) {
            uint32_t word=function->code[pc],index=word>>16;
            if ((word&65535u)!=XR_RBC_VALUE_ACCESS || index>=code->access_count ||
                code->accesses[index].operation!=XR_RBC_VALUE_INVOKE) continue;
            uint32_t result=code->accesses[index].result;
            CHECK(code->homes && result && result<=code->homes->value_count);
            XrLirValueHome *home=(XrLirValueHome *)&code->homes->values[result];
            uint32_t original=home->type;
            CHECK(original && original<=image->type_count);
            const XrtValueImageType *before=&image->types[original-1],*after=&image->types[other_type-1];
            CHECK(before->kind==XRT_VALUE_NULLABLE && before->nullable_element && before->nullable_element<=image->type_count &&
                image->types[before->nullable_element-1].kind==XRT_VALUE_I64);
            CHECK(!before->owned_count && !after->owned_count && before->size==after->size &&
                before->alignment==after->alignment && before->nullable_tag==after->nullable_tag &&
                before->nullable_payload==after->nullable_payload);
            CHECK(xr_rbc_value_instruction_valid(module,function,pc));
            /* Keep the real code lease and physical location. Check only admission while
             * the success home refers to a different same-layout Nullable image row. */
            home->type=other_type;
            bool admitted=xr_rbc_value_instruction_valid(module,function,pc);
            home->type=original;
            CHECK(!admitted && xr_rbc_value_instruction_valid(module,function,pc));
            return;
        }
    }
    CHECK(false);
}

static void test_nullable_invoke_result(void) {
    XrLirModule *m=NULL;
    OK(xr_lir_module_create(NULL,&m));
    XrLirLiteral metadata={0};
    XrLirType error_type=test_nominal_error(m,&metadata),flag=XR_LIR_I1,optional=0,other=0;
    OK(xr_lir_nullable_type(m,XR_LIR_I64,&optional));
    OK(xr_lir_nullable_type(m,XR_LIR_U64,&other));
    XrLirFunction leaf=test_throwing_leaf(m,error_type,optional);
    XrLirFunction root=test_function(m,"nullable_invoke",&flag,1,XR_LIR_I64);
    XrLirBlock entry=test_block(m,root,NULL,0),ok=test_block(m,root,&optional,1);
    XrLirBlock error=test_block(m,root,&error_type,1),unwrap=test_block(m,root,&optional,1);
    XrLirBlock unexpected=test_block(m,root,NULL,0);
    OK(xr_lir_set_entry(m,root,entry));
    XrLirValue fail=parameter(m,root,0);
    OK(xr_lir_set_terminator(m,entry,&(XrLirInstruction){.op=XR_LIR_INVOKE,.type=optional,.callee=leaf,
        .operands=&fail,.operand_count=1,.edges={{.target=ok},{.target=error}},.edge_count=2}));
    XrLirValue result=block_parameter(m,ok,0),none={0},present={0};
    OK(xr_lir_emit_value(m,ok,&(XrLirInstruction){.op=XR_LIR_NULLABLE_NONE,.type=other},&none));
    OK(xr_lir_emit_value(m,ok,&(XrLirInstruction){.op=XR_LIR_NULLABLE_IS_SOME,.type=flag,
        .operands=&none,.operand_count=1},&present));
    OK(xr_lir_set_terminator(m,ok,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&present,.operand_count=1,.edges={{.target=unexpected},{.target=unwrap,.arguments=&result,.argument_count=1}},
        .edge_count=2}));
    test_return(m,unexpected,test_constant(m,unexpected,XR_LIR_I64,0));
    XrLirValue wrapped=block_parameter(m,unwrap,0),payload={0};
    OK(xr_lir_emit_value(m,unwrap,&(XrLirInstruction){.op=XR_LIR_NULLABLE_UNWRAP,.type=XR_LIR_I64,
        .operands=&wrapped,.operand_count=1},&payload));
    test_return(m,unwrap,payload);
    XrLirValue thrown=block_parameter(m,error,0);
    OK(xr_lir_set_terminator(m,error,&(XrLirInstruction){.op=XR_LIR_PANIC_ERROR,.type=XR_LIR_UNIT,
        .operands=&thrown,.operand_count=1,.bits=metadata.id,.source={"nullable.xr",11,1,1,1,5}}));
    OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,root);
    xr_lir_module_destroy(m);
    test_nullable_invoke_type_identity(program);
    uint64_t argument=0,bits=0;
    CHECK(run(program,&argument,1)==17);
    argument=1;
    CHECK(execute(program,&argument,1,&bits)==XR_RBC_TRY_ASSERT);
    xr_rbc_program_destroy(program);
}

static void test_unsupported_nullable_invoke_result(bool managed) {
    XrLirModule *m=NULL;
    OK(xr_lir_module_create(NULL,&m));
    XrLirLiteral metadata={0};
    XrLirType error_type=test_nominal_error(m,&metadata),optional=0;
    XrLirType element=error_type;
    if (managed) {
        /* Exercise two presence layers with an owned payload. */
        OK(xr_lir_nullable_type(m,XR_LIR_STRING,&element));
    }
    OK(xr_lir_nullable_type(m,element,&optional));
    XrLirTypeLayout layout={0};
    OK(xr_lir_type_layout(m,optional,&layout));
    CHECK(layout.size && layout.managed==managed);
    XrLirFunction function={0};
    XrLirFunctionSpec spec={.name="leaf",.name_length=4,.result=optional,.error=error_type};
    CHECK(xr_lir_declare_function(m,&spec,&function)==XR_LIR_INVALID_ARGUMENT && !function.id);
    xr_lir_module_destroy(m);
}

static void test_array_invoke_receivers(const XrRbcProgram *program) {
    const XrRbcModule *module=xr_rbc_program_view(program);
    const XrtValueImage *image=xr_rbc_module_value_image(module);
    CHECK(image && image->types && module->code_lease.owner);
    uint32_t other_type=0;
    for (uint32_t n=0;n<image->type_count;++n)
        if (image->types[n].kind==XRT_VALUE_ARRAY_I64) other_type=n+1;
    CHECK(other_type);
    for (uint32_t n=0;n<module->function_count;++n) {
        const XrRbcFunction *function=&module->functions[n];
        const XrRbcValueCode *code=xr_rbc_module_value_code(module,n);
        if (!code) continue;
        for (size_t pc=0;pc<function->code_count;++pc) {
            uint32_t word=function->code[pc],index=word>>16;
            if ((word&65535u)!=XR_RBC_VALUE_ACCESS || index>=code->access_count ||
                code->accesses[index].operation!=XR_RBC_VALUE_INVOKE) continue;
            XrRbcValueAccess call=code->accesses[index];
            CHECK(xr_rbc_value_instruction_valid(module,function,pc));
            XrLirValueHome *home=(XrLirValueHome *)&code->homes->values[call.result];
            uint32_t original=home->type;
            CHECK(image->types[original-1].kind==XRT_VALUE_ARRAY_U8 && home->owner_count==1 && !home->parameter &&
                image->types[original-1].size==image->types[other_type-1].size);
            home->type=other_type;
            bool wrong_family=xr_rbc_value_instruction_valid(module,function,pc);
            home->type=original;
            CHECK(!wrong_family);

            XrRbcValueAccess *error=(XrRbcValueAccess *)&code->accesses[function->code[pc+3]>>16];
            CHECK(error->operation==XR_RBC_VALUE_INVOKE_ERROR && error->result==call.other && call.other!=call.result);
            error->result=call.result;
            bool wrong_edge=xr_rbc_value_instruction_valid(module,function,pc);
            error->result=call.other;
            CHECK(!wrong_edge && xr_rbc_value_instruction_valid(module,function,pc));
            return;
        }
    }
    CHECK(false);
}

static void test_owned_array_invoke(bool wrong_element) {
    XrLirModule *m=NULL;
    OK(xr_lir_array_module_create(NULL,&m));
    XrLirLiteral metadata={0};
    XrLirType error_type=test_nominal_error(m,&metadata),flag=XR_LIR_I1,array=XR_LIR_ARRAY;
    XrLirType element=wrong_element ? XR_LIR_I64 : XR_LIR_U8;
    XrLirFunction leaf=test_throwing_leaf(m,error_type,array);
    XrLirFunction root=test_function(m,"invoke_array",&flag,1,XR_LIR_I64);
    XrLirBlock entry=test_block(m,root,NULL,0),normal=test_block(m,root,&array,1);
    XrLirBlock failure=test_block(m,root,&error_type,1);
    OK(xr_lir_set_entry(m,root,entry));
    XrLirValue fail=parameter(m,root,0),result=block_parameter(m,normal,0);
    OK(xr_lir_set_array_shape(m,result,&(XrLirArrayShape){element}));
    XrLirStatus invoke=xr_lir_set_terminator(m,entry,&(XrLirInstruction){.op=XR_LIR_INVOKE,.type=array,.callee=leaf,
        .operands=&fail,.operand_count=1,.edges={{.target=normal},{.target=failure}},.edge_count=2});
    if (wrong_element) {
        XrLirDiagnostic diagnostic={0};
        CHECK(invoke==XR_LIR_VERIFY_ERROR && xr_lir_module_finish(m,&diagnostic)==XR_LIR_VERIFY_ERROR &&
            diagnostic.function==root.id && diagnostic.block==entry.id);
        xr_lir_module_destroy(m);
        return;
    }
    CHECK(invoke==XR_LIR_OK);
    uint32_t length_operation=0,other_constructor=0;
    XrLirValue length={0},other_array={0};
    OK(xr_lir_array_operation(m,element,XR_LIR_ARRAY_OP_LEN,&length_operation));
    OK(xr_lir_emit_value(m,normal,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_I64,
        .intrinsic=length_operation,.operands=&result,.operand_count=1},&length));
    OK(xr_lir_emit_effect(m,normal,&(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,
        .operands=&result,.operand_count=1}));
    OK(xr_lir_array_operation(m,XR_LIR_I64,XR_LIR_ARRAY_OP_NEW,&other_constructor));
    OK(xr_lir_emit_value(m,normal,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,
        .intrinsic=other_constructor},&other_array));
    OK(xr_lir_emit_effect(m,normal,&(XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,
        .operands=&other_array,.operand_count=1}));
    test_return(m,normal,length);
    XrLirValue thrown=block_parameter(m,failure,0);
    OK(xr_lir_set_terminator(m,failure,&(XrLirInstruction){.op=XR_LIR_PANIC_ERROR,.type=XR_LIR_UNIT,
        .operands=&thrown,.operand_count=1,.bits=metadata.id,.source={"array.xr",7,1,1,1,5}}));
    OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,root);
    xr_lir_module_destroy(m);
    test_array_invoke_receivers(program);
    uint64_t argument=0,bits=0;
    CHECK(run(program,&argument,1)==1 && !xrt_value_root_active());
    argument=1;
    CHECK(execute(program,&argument,1,&bits)==XR_RBC_TRY_ASSERT && !xrt_value_root_active());
    argument=0;
    CHECK(run(program,&argument,1)==1 && !xrt_value_root_active());
    xr_rbc_program_destroy(program);
}

static void test_owned_string_invoke_drop(void) {
    XrLirModule *m=NULL;
    OK(xr_lir_module_create(NULL,&m));
    XrLirLiteral metadata={0};
    XrLirType error_type=test_nominal_error(m,&metadata),flag=XR_LIR_I1,text=XR_LIR_STRING;
    XrLirFunction leaf=test_throwing_leaf(m,error_type,text);
    XrLirFunction root=test_function(m,"discard_invoke_string",&flag,1,XR_LIR_I64);
    XrLirBlock entry=test_block(m,root,NULL,0),ok=test_block(m,root,&text,1);
    XrLirBlock error=test_block(m,root,&error_type,1);
    OK(xr_lir_set_entry(m,root,entry));
    XrLirValue fail=parameter(m,root,0);
    OK(xr_lir_set_terminator(m,entry,&(XrLirInstruction){.op=XR_LIR_INVOKE,.type=text,.callee=leaf,
        .operands=&fail,.operand_count=1,.edges={{.target=ok},{.target=error}},.edge_count=2}));
    XrLirValue received=block_parameter(m,ok,0),length={0};
    OK(xr_lir_emit_value(m,ok,&(XrLirInstruction){.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,
        .operands=&received,.operand_count=1},&length));
    OK(xr_lir_emit_effect(m,ok,&(XrLirInstruction){.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,
        .operands=&received,.operand_count=1}));
    test_return(m,ok,length);
    XrLirValue thrown=block_parameter(m,error,0);
    OK(xr_lir_set_terminator(m,error,&(XrLirInstruction){.op=XR_LIR_PANIC_ERROR,.type=XR_LIR_UNIT,
        .operands=&thrown,.operand_count=1,.bits=metadata.id,.source={"drop.xr",7,1,1,1,5}}));
    XrLirDiagnostic diagnostic={0};
    XrLirStatus status=xr_lir_module_finish(m,&diagnostic);
    if (status!=XR_LIR_OK) fprintf(stderr,"String INVOKE with drop: finish f%u b%u i%u status %u %s\n",
        diagnostic.function,diagnostic.block,diagnostic.instruction,(unsigned)status,diagnostic.message);
    CHECK(status==XR_LIR_OK);
    XrRbcProgram *program=compile(m,root);
    xr_lir_module_destroy(m);

    const XrRbcModule *module=xr_rbc_program_view(program);
    bool landing=false,dropped=false;
    for (uint32_t n=0;n<module->function_count;++n) {
        const XrRbcValueCode *code=xr_rbc_module_value_code(module,n);
        if (!code || !code->homes || code->homes->source_function!=root.id) continue;
        CHECK(code->homes->values && received.id<=code->homes->value_count);
        const XrLirValueHome *home=&code->homes->values[received.id];
        CHECK(home->owner_count==1 && !home->parameter);
        for (size_t k=0;k<code->access_count;++k) {
            XrRbcValueAccess a=code->accesses[k];
            landing|=a.operation==XR_RBC_VALUE_INVOKE_RESULT && a.result==received.id;
            dropped|=a.operation==XR_RBC_VALUE_DROP && a.source==received.id;
        }
    }
    CHECK(landing && dropped);
    uint64_t argument=0;
    CHECK(run(program,&argument,1)==2 && !xrt_value_root_active());
    argument=1;
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,&argument,1,&execution)==XR_RBC_OK);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_TRY_ASSERT && !xrt_value_root_active());
    argument=0;
    CHECK(run(program,&argument,1)==2 && !xrt_value_root_active());
    xr_rbc_program_destroy(program);
    const XrtErrorSnapshot *failure=xr_rbc_outcome_try_assert(outcome);
    XrtTryAssertValue error_value={0};
    CHECK(xrt_try_assert_value(failure,&error_value) && error_value.variant==1 && error_value.field_count==1 &&
        error_value.metadata_size>=47 && xr_error_metadata_u32(error_value.metadata+4)==error_type &&
        !memcmp(error_value.metadata+39,"ProbeErr",8));
    uint32_t scalar=0;
    uint64_t bits=0;
    CHECK(xrt_try_assert_field(failure,0,&scalar,&bits) && scalar==XR_POD_I64 && bits==23);
    xr_rbc_outcome_free(&outcome);
}

static void test_owned_startup_error(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL,&m));
    XrLirLiteral metadata = {0};
    XrLirType type = test_nominal_error(m,&metadata);
    XrLirFunction initializer = {0};
    XrLirFunctionSpec spec = {.name="initializer",.name_length=11,.result=XR_LIR_UNIT,
        .error=type,.initializer_module=1};
    OK(xr_lir_declare_function(m,&spec,&initializer));
    OK(xr_lir_define_function(m,initializer));
    XrLirBlock init = test_block(m,initializer,NULL,0);
    OK(xr_lir_set_entry(m,initializer,init));
    XrLirValue payload = test_constant(m,init,XR_LIR_I64,23), value = {0};
    OK(xr_lir_emit_value(m,init,&(XrLirInstruction){.op=XR_LIR_SUM_NEW,.type=type,
        .bits=1,.operands=&payload,.operand_count=1},&value));
    OK(xr_lir_set_terminator(m,init,&(XrLirInstruction){.op=XR_LIR_RETURN_ERROR,.type=XR_LIR_UNIT,
        .operands=&value,.operand_count=1}));

    XrLirFunction root = test_function(m,"startup",NULL,0,XR_LIR_UNIT);
    XrLirBlock entry = test_block(m,root,NULL,0), ok = test_block(m,root,NULL,0);
    XrLirBlock error = test_block(m,root,&type,1);
    OK(xr_lir_set_entry(m,root,entry));
    OK(xr_lir_set_startup_entry(m,root,1));
    OK(xr_lir_set_terminator(m,entry,&(XrLirInstruction){.op=XR_LIR_INVOKE,.type=XR_LIR_UNIT,
        .callee=initializer,.edge_count=2,.edges={{.target=ok},{.target=error}}}));
    OK(xr_lir_set_terminator(m,ok,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT}));
    value=block_parameter(m,error,0);
    OK(xr_lir_set_terminator(m,error,&(XrLirInstruction){.op=XR_LIR_UNCAUGHT_ERROR,.type=XR_LIR_UNIT,
        .operands=&value,.operand_count=1,.bits=metadata.id}));
    OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,root);
    xr_lir_module_destroy(m);
    reject_forged_error_literal(program,NULL,0);
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(program);
    xr_rbc_execution_enable_try_trace(execution);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    CHECK(xr_rbc_outcome_status(outcome)==XR_RBC_UNCAUGHT_ERROR);
    CHECK(!xr_rbc_outcome_try_assert(outcome) && !xr_rbc_outcome_try_trace_count(outcome));
    const XrtErrorSnapshot *failure=xr_rbc_outcome_uncaught_error(outcome);
    CHECK(failure && !xrt_try_assert_location(failure));
    XrtTryAssertValue owned={0};
    CHECK(xrt_try_assert_value(failure,&owned) && owned.variant==1 && owned.field_count==1 && owned.metadata_size>47);
    CHECK(xr_error_metadata_u32(owned.metadata+4)==type && !memcmp(owned.metadata+39,"ProbeErr",8));
    uint32_t scalar=0;
    uint64_t bits=0;
    CHECK(xrt_try_assert_field(failure,0,&scalar,&bits) && scalar==XR_POD_I64 && bits==23);
    xr_rbc_outcome_free(&outcome);
    CHECK(!outcome);
}

static void conditional(XrLirModule *m, XrLirBlock b, XrLirValue condition,
    XrLirEdge yes, XrLirEdge no) {
    XrLirInstruction branch = {.op = XR_LIR_CONDBR, .type = XR_LIR_UNIT,
        .operands = &condition, .operand_count = 1, .edges = {yes, no}, .edge_count = 2};
    OK(xr_lir_set_terminator(m, b, &branch));
}

static XrLirValue call(XrLirModule *m, XrLirBlock b, XrLirFunction callee,
    XrLirType type, const XrLirValue *arguments, uint32_t count) {
    XrLirInstruction i = {.op = XR_LIR_CALL, .type = type, .callee = callee,
        .operands = arguments, .operand_count = count};
    XrLirValue result = {0};
    if (type == XR_LIR_UNIT) OK(xr_lir_emit_effect(m, b, &i));
    else OK(xr_lir_emit_value(m, b, &i, &result));
    return result;
}

#include "lir_bytecode_moves_cases.h"

static void test_integer_identity_cast(XrLirType source, XrLirType target) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = test_function(m, "identity_cast", &source, 1, target);
    XrLirBlock block = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, block));
    XrLirValue input = parameter(m, f, 0), result;
    char path[] = "identity-cast.xr";
    for (uint32_t n = 0; n < 2; ++n) {
        XrLirInstruction instruction = {.op = XR_LIR_INTCAST, .type = target,
            .operands = &input, .operand_count = 1,
            .source = {path, sizeof(path) - 1, 4 + n, 2, 4 + n, 10}};
        OK(xr_lir_emit_value(m, block, &instruction, &result));
        input = result;
    }
    test_return(m, block, input);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, f);
    memset(path, 'x', sizeof(path) - 1);
    xr_lir_module_destroy(m);
    const XrtSourceLocation *location = xrt_locations_get(xr_rbc_program_locations(program), 1);
    CHECK(location && location->line == 4 && location->path_length == sizeof(path) - 1 &&
        !memcmp(location->path, "identity-cast.xr", sizeof(path) - 1));
    const uint64_t cases[] = {0, 1, INT64_MAX, UINT64_C(0x8000000000000000),
        UINT64_MAX, UINT64_C(0xa5a55a5adeadbeef)};
    for (size_t n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n)
        CHECK(run(program, &cases[n], 1) == cases[n]);
    xr_rbc_program_destroy(program);
}

static void test_u64_arithmetic_and_edges(void) {
    const XrLirOp operations[] = {XR_LIR_ADD, XR_LIR_SUB, XR_LIR_MUL, XR_LIR_NEG};
    const uint64_t inputs[][2] = {{0, 1}, {UINT64_MAX, 1}, {0, UINT64_MAX},
        {UINT64_C(0x8000000000000000), 2}, {UINT64_MAX, UINT64_MAX},
        {UINT64_C(0xa5a55a5adeadbeef), UINT64_C(0x0102030405060708)}};
    for (size_t op = 0; op < sizeof(operations) / sizeof(*operations); ++op) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType types[] = {XR_LIR_U64, XR_LIR_U64, XR_LIR_I1}, type = XR_LIR_U64;
        XrLirFunction f = test_function(m, "unsigned_edges", types, 3, type);
        XrLirBlock entry = test_block(m, f, NULL, 0), join = test_block(m, f, &type, 1);
        OK(xr_lir_set_entry(m, f, entry));
        XrLirValue operands[] = {parameter(m, f, 0), parameter(m, f, 1)}, result;
        XrLirInstruction arithmetic = {.op = operations[op], .type = type,
            .operands = operands, .operand_count = operations[op] == XR_LIR_NEG ? 1u : 2u};
        OK(xr_lir_emit_value(m, entry, &arithmetic, &result));
        conditional(m, entry, parameter(m, f, 2), (XrLirEdge){join, &result, 1},
            (XrLirEdge){join, &operands[0], 1});
        test_return(m, join, block_parameter(m, join, 0));
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, f);
        xr_lir_module_destroy(m);
        for (size_t n = 0; n < sizeof(inputs) / sizeof(*inputs); ++n) {
            uint64_t left = inputs[n][0], right = inputs[n][1], expected = 0;
            switch (operations[op]) {
            case XR_LIR_ADD: expected = left + right; break;
            case XR_LIR_SUB: expected = left - right; break;
            case XR_LIR_MUL: expected = left * right; break;
            case XR_LIR_NEG: expected = UINT64_C(0) - left; break;
            default: CHECK(false);
            }
            uint64_t arguments[] = {left, right, 0};
            CHECK(run(program, arguments, 3) == left);
            arguments[2] = 1;
            CHECK(run(program, arguments, 3) == expected);
        }
        xr_rbc_program_destroy(program);
    }
}

static void test_u64_ordered_comparisons(void) {
    // Every pair distinguishes the ordered meaning from its signed reading.
    static const struct {
        XrLirPredicate predicate;
        uint64_t left, right, expected;
    } cases[] = {
        {XR_LIR_LT, UINT64_C(0x8000000000000000), 1, 0},
        {XR_LIR_GT, UINT64_C(0x8000000000000000), 1, 1},
        {XR_LIR_LE, 1, UINT64_C(0x8000000000000000), 1},
        {XR_LIR_GE, 1, UINT64_C(0x8000000000000000), 0},
        {XR_LIR_LT, 0, UINT64_MAX, 1},
        {XR_LIR_GE, UINT64_MAX, 0, 1},
        {XR_LIR_LT, UINT64_MAX, UINT64_MAX, 0},
    };
    for (size_t n = 0; n < sizeof(cases) / sizeof(*cases); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType types[] = {XR_LIR_U64, XR_LIR_U64};
        XrLirFunction f = test_function(m, "unsigned_compare", types, 2, XR_LIR_I1);
        XrLirBlock entry = test_block(m, f, NULL, 0);
        OK(xr_lir_set_entry(m, f, entry));
        XrLirValue operands[] = {parameter(m, f, 0), parameter(m, f, 1)}, result;
        XrLirInstruction compare = {.op = XR_LIR_ICMP, .type = XR_LIR_I1,
            .operands = operands, .operand_count = 2, .predicate = cases[n].predicate};
        OK(xr_lir_emit_value(m, entry, &compare, &result));
        test_return(m, entry, result);
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, f);
        xr_lir_module_destroy(m);
        uint64_t arguments[] = {cases[n].left, cases[n].right};
        CHECK(run(program, arguments, 2) == cases[n].expected);
        // The same ordering must hold against an encoded bound, which the
        // compiler folds into the immediate comparison form.
        xr_rbc_program_destroy(program);
    }
    // A literal right operand is materialized and compared in registers: only
    // an i64 constant can be folded into the encoded-bound form, so a u64
    // ordered comparison must never emit the signed immediate opcode.
    for (uint64_t bound = 0; bound <= 2; ++bound) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType types[] = {XR_LIR_U64};
        XrLirFunction f = test_function(m, "unsigned_compare_literal", types, 1, XR_LIR_I1);
        XrLirBlock entry = test_block(m, f, NULL, 0);
        OK(xr_lir_set_entry(m, f, entry));
        XrLirValue bound_value = test_constant(m, entry, XR_LIR_U64, (int64_t)bound);
        XrLirValue operands[] = {parameter(m, f, 0), bound_value}, result;
        XrLirInstruction compare = {.op = XR_LIR_ICMP, .type = XR_LIR_I1,
            .operands = operands, .operand_count = 2, .predicate = XR_LIR_GT};
        OK(xr_lir_emit_value(m, entry, &compare, &result));
        test_return(m, entry, result);
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, f);
        xr_lir_module_destroy(m);
        const XrRbcFunction *code = &xr_rbc_program_view(program)->functions[0];
        unsigned unsigned_compares = 0;
        for (size_t pc = 0; pc < code->code_count; ++pc) {
            unsigned op = code->code[pc] & 255;
            CHECK(op != XR_RBC_LT && op != XR_RBC_LE);
            unsigned_compares += op == XR_RBC_LTU || op == XR_RBC_LEU;
        }
        CHECK(unsigned_compares == 1);
        const uint64_t argument = UINT64_C(0x8000000000000000);
        CHECK(run(program, &argument, 1) == 1);
        xr_rbc_program_destroy(program);
    }
}

static void test_recursive_and_loop(void) {
    XrLirModule *m = test_fib();
    XrLirFunction root;
    OK(xr_lir_function_at(m, 1, &root));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    const XrRbcFunction *fib = &xr_rbc_program_view(program)->functions[0];
    unsigned subtracts = 0, comparisons = 0;
    for (size_t pc = 0; pc < fib->code_count; ++pc) {
        unsigned op = fib->code[pc] & 255;
        subtracts += op == XR_RBC_SUBI;
        comparisons += op == XR_RBC_LTI;
        CHECK(op != XR_RBC_SUB && op != XR_RBC_LT);
    }
    CHECK(subtracts == 2 && comparisons == 1);
    uint64_t n = 32;
    CHECK(run(program, &n, 1) == 2178309);
    CHECK(xr_rbc_program_view(program)->function_count == 1);
    xr_rbc_program_destroy(program);
    m = test_sum();
    OK(xr_lir_function_at(m, 1, &root));
    program = compile(m, root);
    xr_lir_module_destroy(m);
    n = 1000;
    CHECK(run(program, &n, 1) == 499500);
    xr_rbc_program_destroy(program);
}

static void test_immediate_selection(void) {
    static const struct { XrLirOp op; int64_t literal; bool left, encoded; } cases[] = {
        {XR_LIR_ADD, -128, false, true}, {XR_LIR_ADD, 127, true, true},
        {XR_LIR_SUB, -1, false, true}, {XR_LIR_SUB, 1, true, false},
        {XR_LIR_ADD, 128, false, false}, {XR_LIR_SUB, -129, false, false},
        {XR_LIR_MUL, -128, false, true}, {XR_LIR_MUL, 127, true, true},
        {XR_LIR_MUL, 128, false, false}, {XR_LIR_REM, -1, false, true},
        {XR_LIR_REM, -128, false, true}, {XR_LIR_REM, 127, false, true},
        {XR_LIR_REM, 128, false, false}, {XR_LIR_REM, 7, true, false},
        {XR_LIR_REM, 0, false, true}
    };
    const uint64_t inputs[] = {0, 1, INT64_MAX, UINT64_C(0x8000000000000000), UINT64_MAX};
    for (size_t n = 0; n < sizeof(cases) / sizeof(*cases); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType type = XR_LIR_I64;
        XrLirFunction f = test_function(m, "immediate", &type, 1, type);
        XrLirBlock block = test_block(m, f, NULL, 0);
        OK(xr_lir_set_entry(m, f, block));
        XrLirValue input = parameter(m, f, 0);
        XrLirValue constant = test_constant(m, block, type, (uint64_t)cases[n].literal);
        XrLirValue result = test_binary(m, block, cases[n].op,
            cases[n].left ? constant : input, cases[n].left ? input : constant,
            XR_LIR_PREDICATE_NONE);
        // A non-immediate use keeps a real constant register alongside encoded uses.
        XrLirValue one = test_constant(m, block, type, 1);
        (void)test_binary(m, block, XR_LIR_DIV, constant, one, XR_LIR_PREDICATE_NONE);
        result = test_binary(m, block, XR_LIR_MUL, result, constant, XR_LIR_PREDICATE_NONE);
        test_return(m, block, result);
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, f);
        xr_lir_module_destroy(m);
        const XrRbcFunction *bytecode = &xr_rbc_program_view(program)->functions[0];
        XrRbcOp wanted;
        switch (cases[n].op) {
        case XR_LIR_ADD: wanted = XR_RBC_ADDI; break;
        case XR_LIR_SUB: wanted = XR_RBC_SUBI; break;
        case XR_LIR_MUL: wanted = XR_RBC_MULI; break;
        default: wanted = XR_RBC_REMI; break;
        }
        unsigned immediate = 0;
        for (size_t pc = 0; pc < bytecode->code_count; ++pc) {
            unsigned op = bytecode->code[pc] & 255;
            immediate += op == (unsigned)wanted;
        }
        unsigned expected_count = (unsigned)cases[n].encoded;
        if (cases[n].op == XR_LIR_MUL) expected_count *= 2;
        CHECK(immediate == expected_count);
        for (size_t a = 0; a < sizeof(inputs) / sizeof(*inputs); ++a) {
            uint64_t literal = (uint64_t)cases[n].literal;
            uint64_t lhs = cases[n].left ? literal : inputs[a];
            uint64_t rhs = cases[n].left ? inputs[a] : literal;
            uint64_t expected = 0;
            if (cases[n].op == XR_LIR_REM) {
                uint64_t bits = UINT64_MAX;
                if (!rhs) {
                    CHECK(execute(program, &inputs[a], 1, &bits) == XR_RBC_TYPED_PANIC);
                    continue;
                }
                int64_t left, right;
                memcpy(&left, &lhs, sizeof(left)); memcpy(&right, &rhs, sizeof(right));
                expected = left == INT64_MIN && right == -1 ? 0 : (uint64_t)(left % right);
            } else if (cases[n].op == XR_LIR_MUL) expected = lhs * rhs;
            else expected = cases[n].op == XR_LIR_ADD ? lhs + rhs : lhs - rhs;
            expected *= literal;
            CHECK(run(program, &inputs[a], 1) == expected);
        }
        xr_rbc_program_destroy(program);
    }
}

static void test_u8_immediate_selection(void) {
    static const struct {
        XrLirOp operation;
        XrRbcOp opcode;
        uint64_t argument, literal, expected;
        bool literal_first;
    } cases[] = {
        {XR_LIR_ADD, XR_RBC_ADDI, 250, 127, 121, false},
        {XR_LIR_SUB, XR_RBC_SUBI, 0, 1, 255, false},
        {XR_LIR_MUL, XR_RBC_MULI, 200, 2, 144, true},
        {XR_LIR_ADD, XR_RBC_ADD, 250, 128, 122, false},
        {XR_LIR_SUB, XR_RBC_SUB, 2, 1, 255, true},
    };
    for (size_t n = 0; n < sizeof(cases) / sizeof(*cases); ++n) {
        XrLirModule *module = NULL;
        OK(xr_lir_module_create(NULL, &module));
        XrLirType type = XR_LIR_U8;
        XrLirFunction function = test_function(module, "byte_arithmetic", &type, 1, type);
        XrLirBlock entry = test_block(module, function, NULL, 0);
        OK(xr_lir_set_entry(module, function, entry));
        XrLirValue input = parameter(module, function, 0);
        XrLirValue literal = test_constant(module, entry, type, cases[n].literal);
        XrLirValue operands[] = {
            cases[n].literal_first ? literal : input,
            cases[n].literal_first ? input : literal,
        }, result;
        OK(xr_lir_emit_value(module, entry, &(XrLirInstruction){
            .op = cases[n].operation, .type = type, .operands = operands, .operand_count = 2,
        }, &result));
        test_return(module, entry, result);
        OK(xr_lir_module_finish(module, NULL));
        XrRbcProgram *program = compile(module, function);
        xr_lir_module_destroy(module);

        const XrRbcFunction *bytecode = &xr_rbc_program_view(program)->functions[0];
        unsigned operations = 0, normalizations = 0;
        for (size_t pc = 0; pc < bytecode->code_count; ++pc) {
            unsigned op = bytecode->code[pc] & 255;
            operations += op == (unsigned)cases[n].opcode;
            normalizations += op == XR_RBC_ZEXT;
        }
        CHECK(operations == 1 && normalizations == 1);
        CHECK(run(program, &cases[n].argument, 1) == cases[n].expected);
        xr_rbc_program_destroy(program);
    }
}

static void test_location_ownership(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction helper = test_function(m, "helper", NULL, 0, XR_LIR_I64);
    XrLirFunction root = test_function(m, "root", NULL, 0, XR_LIR_I64);
    XrLirFunction unused = test_function(m, "unused", NULL, 0, XR_LIR_I64);
    char path[400];
    memset(path, 'p', sizeof(path));
    memcpy(path + sizeof(path) - 7, "\xc3\xa9\n.xr!", 7);
    XrLirSource source = {path, sizeof(path) - 1, 7, 9, 7, 9};
    XrLirBlock finish = test_block(m, root, NULL, 0);
    XrLirBlock entry = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = XR_LIR_I64,
        .bits = 1, .source = source};
    XrLirValue ignored;
    OK(xr_lir_emit_value(m, entry, &instruction, &ignored));
    XrLirValue value = call(m, entry, helper, XR_LIR_I64, NULL, 0);
    test_branch(m, entry, finish, NULL, 0);
    instruction = (XrLirInstruction) {.op = XR_LIR_RET, .type = XR_LIR_UNIT,
        .operands = &value, .operand_count = 1, .source = source};
    OK(xr_lir_set_terminator(m, finish, &instruction));
    XrLirBlock block = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, helper, block));
    instruction = (XrLirInstruction) {.op = XR_LIR_CONST, .type = XR_LIR_I64,
        .bits = 7, .source = {"helper.xr", 9, 99, 2, 100, 3}};
    OK(xr_lir_emit_value(m, block, &instruction, &value));
    test_return(m, block, value);
    block = test_block(m, unused, NULL, 0);
    OK(xr_lir_set_entry(m, unused, block));
    instruction.source.line = 200;
    instruction.source.end_line = 201;
    OK(xr_lir_emit_value(m, block, &instruction, &value));
    test_return(m, block, value);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    XrtLocationOwner *owner = xr_rbc_program_locations(program);
    CHECK(owner && xrt_locations_count(owner) == 3);
    CHECK(xrt_locations_retain(owner));
    xr_lir_module_destroy(m);
    CHECK(run(program, NULL, 0) == 7);
    xr_rbc_program_destroy(program);
    const XrtSourceLocation *first = xrt_locations_get(owner, 1);
    const XrtSourceLocation *duplicate = xrt_locations_get(owner, 2);
    const XrtSourceLocation *last = xrt_locations_get(owner, 3);
    CHECK(first && duplicate && last && first->path_length == sizeof(path) - 1);
    CHECK(!memcmp(first->path, path, sizeof(path) - 1));
    memset(path, '?', sizeof(path));
    CHECK(first->line == 7 && duplicate->line == 7 && last->line == 99);
    CHECK(first->column == 9 && first->end_line == 7 && first->end_column == 9);
    CHECK(!memcmp(first->path, duplicate->path, first->path_length));
    CHECK(last->path_length == 9 && !memcmp(last->path, "helper.xr", 9));
    CHECK(!xrt_locations_get(owner, 0) && !xrt_locations_get(owner, 4));
    xrt_locations_release(owner);
    CHECK(!xr_rbc_program_locations(NULL));

    m = test_sum();
    OK(xr_lir_function_at(m, 1, &root));
    program = compile(m, root);
    owner = xr_rbc_program_locations(program);
    CHECK(owner && !xrt_locations_count(owner) && xrt_locations_retain(owner));
    xrt_locations_release(owner);
    CHECK(!xrt_locations_count(xr_rbc_program_locations(program)));
    xr_rbc_program_destroy(program);
    xr_lir_module_destroy(m);
}

static void test_comparisons(void) {
    const uint64_t inputs[][2] = {{0, 0}, {UINT64_MAX, 2}, {INT64_MAX, UINT64_C(1) << 63}};
    const bool expected[][6] = {{true, false, false, true, false, true},
                                {false, true, true, true, false, false},
                                {false, true, false, false, true, true}};
    for (uint32_t predicate = XR_LIR_EQ; predicate <= XR_LIR_GE; ++predicate)
        for (unsigned control = 0; control < 3; ++control) {
            XrLirModule *m = NULL;
            OK(xr_lir_module_create(NULL, &m));
            XrLirType types[] = {XR_LIR_I64, XR_LIR_I64}, boolean = XR_LIR_I1;
            XrLirFunction root = test_function(m, "predicate", types, 2, boolean);
            XrLirBlock entry = test_block(m, root, NULL, 0);
            XrLirBlock join = test_block(m, root, &boolean, 1);
            OK(xr_lir_set_entry(m, root, entry));
            XrLirValue yes = {0}, no = {0};
            if (control == 1) {
                yes = test_constant(m, entry, boolean, 1);
                no = test_constant(m, entry, boolean, 0);
            }
            XrLirValue result = test_binary(m, entry, XR_LIR_ICMP, parameter(m, root, 0),
                                            parameter(m, root, 1), (XrLirPredicate) predicate);
            if (control == 0)
                test_branch(m, entry, join, &result, 1);
            else {
                if (control == 2)
                    yes = no = result;
                conditional(m, entry, result, (XrLirEdge) {join, &yes, 1},
                            (XrLirEdge) {join, &no, 1});
            }
            test_return(m, join, block_parameter(m, join, 0));
            OK(xr_lir_module_finish(m, NULL));
            XrRbcProgram *program = compile(m, root);
            if (control == 1) {
                const XrRbcFunction *function = &xr_rbc_program_view(program)->functions[0];
                unsigned loads = 0;
                for (size_t pc = 0; pc < function->code_count; ++pc)
                    if ((function->code[pc] & 255) == XR_RBC_LOADI)
                        ++loads;
                CHECK(loads == 2);
            }
            xr_lir_module_destroy(m);
            for (size_t n = 0; n < 3; ++n)
                CHECK(run(program, inputs[n], 2) == expected[n][predicate - XR_LIR_EQ]);
            xr_rbc_program_destroy(program);
        }
}

static void test_arithmetic(void) {
    const XrLirOp operations[] = {XR_LIR_ADD, XR_LIR_SUB, XR_LIR_MUL,
                                  XR_LIR_DIV, XR_LIR_REM, XR_LIR_NEG};
    const uint64_t expected[] = {UINT64_C(9223372036854775807),
                                 UINT64_C(9223372036854775809),
                                 UINT64_C(9223372036854775808),
                                 UINT64_C(9223372036854775808),
                                 0,
                                 UINT64_C(9223372036854775808)};
    for (size_t n = 0; n < sizeof(operations) / sizeof(operations[0]); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirType types[] = {XR_LIR_I64, XR_LIR_I64};
        XrLirFunction root = test_function(m, "arithmetic", types, 2, XR_LIR_I64);
        XrLirBlock entry = test_block(m, root, NULL, 0);
        OK(xr_lir_set_entry(m, root, entry));
        XrLirValue args[] = {parameter(m, root, 0), parameter(m, root, 1)}, result;
        XrLirInstruction i = {.op = operations[n], .type = XR_LIR_I64, .operands = args,
            .operand_count = operations[n] == XR_LIR_NEG ? 1 : 2};
        OK(xr_lir_emit_value(m, entry, &i, &result));
        test_return(m, entry, result);
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, root);
        xr_lir_module_destroy(m);
        uint64_t values[] = {UINT64_C(9223372036854775808), UINT64_MAX};
        CHECK(run(program, values, 2) == expected[n]);
        if (operations[n] == XR_LIR_DIV || operations[n] == XR_LIR_REM) {
            values[0] = (uint64_t) -7; values[1] = 3;
            CHECK(run(program, values, 2) == (uint64_t) (operations[n] == XR_LIR_DIV ? -2 : -1));
            values[1] = 0;
            uint64_t sentinel = 123;
            CHECK(execute(program, values, 2, &sentinel) == XR_RBC_TYPED_PANIC);
            CHECK(sentinel == 123);
        }
        xr_rbc_program_destroy(program);
    }
}

static void test_constants_and_bool_calls(void) {
    const uint64_t constants[] = {0, 1, INT16_MAX, (uint64_t) INT16_MIN,
        32768, (uint64_t) -32769, INT64_MAX, UINT64_C(1) << 63, UINT64_MAX};
    for (size_t n = 0; n < sizeof(constants) / sizeof(constants[0]); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunction root = test_function(m, "constant", NULL, 0, XR_LIR_I64);
        XrLirBlock entry = test_block(m, root, NULL, 0);
        OK(xr_lir_set_entry(m, root, entry));
        test_return(m, entry, test_constant(m, entry, XR_LIR_I64, constants[n]));
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, root);
        xr_lir_module_destroy(m);
        CHECK(run(program, NULL, 0) == constants[n]);
        xr_rbc_program_destroy(program);
    }
    for (XrLirPredicate predicate = XR_LIR_EQ; predicate <= XR_LIR_NE; ++predicate) {
        XrLirModule *m = NULL;
        XrLirType boolean = XR_LIR_I1, pair[] = {XR_LIR_I1, XR_LIR_I1};
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunction identity = test_function(m, "identity", &boolean, 1, boolean);
        XrLirBlock body = test_block(m, identity, NULL, 0);
        OK(xr_lir_set_entry(m, identity, body));
        test_return(m, body, parameter(m, identity, 0));
        XrLirFunction root = test_function(m, "bool_call", pair, 2, boolean);
        XrLirBlock entry = test_block(m, root, NULL, 0);
        OK(xr_lir_set_entry(m, root, entry));
        XrLirValue result = test_binary(m, entry, XR_LIR_ICMP, parameter(m, root, 0),
            parameter(m, root, 1), predicate);
        test_return(m, entry, call(m, entry, identity, boolean, &result, 1));
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, root);
        xr_lir_module_destroy(m);
        for (uint64_t a = 0; a < 2; ++a) for (uint64_t b = 0; b < 2; ++b) {
            uint64_t arguments[] = {a, b};
            CHECK(run(program, arguments, 2) == (uint64_t) (predicate == XR_LIR_EQ ? a == b : a != b));
        }
        xr_rbc_program_destroy(program);
    }
}

static void test_parallel_cycle(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType types[] = {XR_LIR_I64, XR_LIR_I64, XR_LIR_I64, XR_LIR_I64};
    XrLirFunction root = test_function(m, "rotate", types, 4, XR_LIR_I64);
    // Entry is deliberately the third block in construction order.
    XrLirBlock done = test_block(m, root, NULL, 0), loop = test_block(m, root, types, 4);
    XrLirBlock entry = test_block(m, root, NULL, 0), body = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue initial[4], carried[4];
    for (uint32_t i = 0; i < 4; ++i) {
        initial[i] = parameter(m, root, i);
        carried[i] = block_parameter(m, loop, i);
    }
    XrLirValue zero = test_constant(m, entry, XR_LIR_I64, 0);
    XrLirValue one = test_constant(m, entry, XR_LIR_I64, 1);
    test_branch(m, entry, loop, initial, 4);
    XrLirValue condition = test_binary(m, loop, XR_LIR_ICMP, carried[3], zero, XR_LIR_GT);
    conditional(m, loop, condition, (XrLirEdge) {.target = body}, (XrLirEdge) {.target = done});
    XrLirValue next = test_binary(m, body, XR_LIR_SUB, carried[3], one, XR_LIR_PREDICATE_NONE);
    XrLirValue rotated[] = {carried[1], carried[2], carried[0], next};
    test_branch(m, body, loop, rotated, 4);
    XrLirValue ten = test_constant(m, done, XR_LIR_I64, 10);
    XrLirValue a = test_binary(m, done, XR_LIR_MUL, carried[0], ten, XR_LIR_PREDICATE_NONE);
    a = test_binary(m, done, XR_LIR_ADD, a, carried[1], XR_LIR_PREDICATE_NONE);
    a = test_binary(m, done, XR_LIR_MUL, a, ten, XR_LIR_PREDICATE_NONE);
    a = test_binary(m, done, XR_LIR_ADD, a, carried[2], XR_LIR_PREDICATE_NONE);
    test_return(m, done, a);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    const uint64_t expected[] = {123, 231, 312, 123};
    for (uint64_t n = 0; n < 4; ++n) {
        uint64_t values[] = {1, 2, 3, n};
        CHECK(run(program, values, 4) == expected[n]);
    }
    xr_rbc_program_destroy(program);
}

static void test_same_target_edges(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType types[] = {XR_LIR_I1, XR_LIR_I64, XR_LIR_I64}, pair[] = {XR_LIR_I64, XR_LIR_I64};
    XrLirFunction root = test_function(m, "select_pair", types, 3, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0), done = test_block(m, root, pair, 2);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue a = parameter(m, root, 1), b = parameter(m, root, 2);
    XrLirValue yes[] = {a, a}, no[] = {b, a};
    conditional(m, entry, parameter(m, root, 0),
        (XrLirEdge) {done, yes, 2}, (XrLirEdge) {done, no, 2});
    XrLirValue ten = test_constant(m, done, XR_LIR_I64, 10);
    XrLirValue value = test_binary(m, done, XR_LIR_MUL, block_parameter(m, done, 0), ten,
        XR_LIR_PREDICATE_NONE);
    value = test_binary(m, done, XR_LIR_ADD, value, block_parameter(m, done, 1),
        XR_LIR_PREDICATE_NONE);
    test_return(m, done, value);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    uint64_t values[] = {1, 1, 2};
    CHECK(run(program, values, 3) == 11);
    values[0] = 0;
    CHECK(run(program, values, 3) == 21);
    values[0] = 2;
    uint64_t sentinel = 123;
    CHECK(execute(program, values, 3, &sentinel) == XR_RBC_INVALID && sentinel == 123);
    xr_rbc_program_destroy(program);
}

static void test_call_windows(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = XR_LIR_I64, pair[] = {XR_LIR_I64, XR_LIR_I64};
    XrLirFunction zero = test_function(m, "zero", NULL, 0, type);
    XrLirFunction one = test_function(m, "one", &type, 1, type);
    XrLirFunction two = test_function(m, "two", pair, 2, type);
    XrLirFunction unit = test_function(m, "unit", NULL, 0, XR_LIR_UNIT);
    XrLirFunction root = test_function(m, "root", &type, 1, type);
    XrLirBlock z = test_block(m, zero, NULL, 0), o = test_block(m, one, NULL, 0);
    XrLirBlock t = test_block(m, two, NULL, 0), u = test_block(m, unit, NULL, 0);
    XrLirBlock r = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, zero, z)); OK(xr_lir_set_entry(m, one, o));
    OK(xr_lir_set_entry(m, two, t)); OK(xr_lir_set_entry(m, unit, u));
    OK(xr_lir_set_entry(m, root, r));
    test_return(m, z, test_constant(m, z, type, 7));
    XrLirValue three = test_constant(m, o, type, 3);
    test_return(m, o, test_binary(m, o, XR_LIR_MUL, parameter(m, one, 0), three,
        XR_LIR_PREDICATE_NONE));
    XrLirValue ten = test_constant(m, t, type, 10);
    XrLirValue combined = test_binary(m, t, XR_LIR_MUL, parameter(m, two, 0), ten,
        XR_LIR_PREDICATE_NONE);
    test_return(m, t, test_binary(m, t, XR_LIR_ADD, combined, parameter(m, two, 1),
        XR_LIR_PREDICATE_NONE));
    test_return(m, u, (XrLirValue) {0});
    XrLirValue sentinel = test_constant(m, r, type, 1000), n = parameter(m, root, 0);
    XrLirValue a = call(m, r, zero, type, NULL, 0);
    XrLirValue b = call(m, r, one, type, &n, 1);
    XrLirValue reverse[] = {b, a}, duplicate[] = {n, n};
    XrLirValue c = call(m, r, two, type, reverse, 2);
    XrLirValue d = call(m, r, two, type, duplicate, 2);
    call(m, r, unit, XR_LIR_UNIT, NULL, 0);
    XrLirValue answer = test_binary(m, r, XR_LIR_ADD, c, d, XR_LIR_PREDICATE_NONE);
    answer = test_binary(m, r, XR_LIR_ADD, answer, sentinel, XR_LIR_PREDICATE_NONE);
    answer = test_binary(m, r, XR_LIR_ADD, answer, a, XR_LIR_PREDICATE_NONE);
    test_return(m, r, answer);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    XrRbcProgram *empty = compile(m, unit);
    xr_lir_module_destroy(m);
    uint64_t input = 2;
    CHECK(run(program, &input, 1) == 1096);
    CHECK(run(empty, NULL, 0) == 0);
    xr_rbc_program_destroy(empty);
    xr_rbc_program_destroy(program);
}

/* Parameters past the register window arrive in the spill bank, so a function
 * compiles to an addressable register file however many parameters it takes. */
static void test_capacity(uint32_t count, bool scratch) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType types[300];
    uint64_t arguments[300];
    for (uint32_t n = 0; n < count; ++n) { types[n] = XR_LIR_I64; arguments[n] = n; }
    if (scratch) types[0] = XR_LIR_I1;
    XrLirFunction root = test_function(m, "capacity", types, count, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue value = parameter(m, root, count - 1);
    if (scratch) {
        XrLirType type = XR_LIR_I64;
        XrLirBlock join = test_block(m, root, &type, 1);
        conditional(m, entry, parameter(m, root, 0),
            (XrLirEdge) {join, &value, 1}, (XrLirEdge) {join, &value, 1});
        test_return(m, join, block_parameter(m, join, 0));
    } else test_return(m, entry, value);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    XrRbcCompileStatus status = xr_rbc_compile(m, root, &program, &diagnostic);
    CHECK(status == XR_RBC_COMPILE_OK);
    const XrRbcFunction *compiled = &xr_rbc_program_view(program)->functions[0];
    uint32_t window = count < XR_RBC_REGISTER_PARAMETER_MAX ? count : XR_RBC_REGISTER_PARAMETER_MAX;
    CHECK(compiled->register_count <= XR_RBC_REGISTER_COUNT_MAX && compiled->parameter_count == window &&
        compiled->spill_parameter_count == count - window);
    CHECK(run(program, arguments, count) == count - 1);
    if (scratch) {
        arguments[0] = 1;
        CHECK(run(program, arguments, count) == count - 1);
    }
    xr_lir_module_destroy(m);
    xr_rbc_program_destroy(program);
}

static void test_call_capacity(uint32_t count) {
    XrLirModule *m = NULL;
    XrLirType types[300];
    for (uint32_t n = 0; n < count; ++n) types[n] = XR_LIR_I64;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction callee = test_function(m, "callee", NULL, 0, XR_LIR_I64);
    XrLirBlock body = test_block(m, callee, NULL, 0);
    OK(xr_lir_set_entry(m, callee, body));
    test_return(m, body, test_constant(m, body, XR_LIR_I64, 71));
    XrLirFunction root = test_function(m, "window_capacity", types, count, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    test_return(m, entry, call(m, entry, callee, XR_LIR_I64, NULL, 0));
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    // Wide parameters leave room for an outgoing call window in the same frame.
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    uint64_t arguments[300] = {0};
    CHECK(run(program, arguments, count) == 71);
    xr_lir_module_destroy(m);
    xr_rbc_program_destroy(program);
}

static void test_unused_and_invalid(void) {
    XrLirType type = XR_LIR_I64;
    XrLirIntrinsic intrinsic = {123, &type, 1, type, true, 0};
    XrLirRegistry registry = {&intrinsic, 1};
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(&registry, &m));
    XrLirFunction unused = test_function(m, "unused_narrow", NULL, 0, XR_LIR_U16);
    XrLirBlock float_body = test_block(m, unused, NULL, 0);
    OK(xr_lir_set_entry(m, unused, float_body));
    test_return(m, float_body, test_constant(m, float_body, XR_LIR_U16, 0));
    XrLirFunction root = test_function(m, "root", NULL, 0, XR_LIR_I64);
    XrLirBlock body = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, body));
    test_return(m, body, test_constant(m, body, XR_LIR_I64, 17));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_INVALID);
    OK(xr_lir_module_finish(m, NULL));
    program = compile(m, root);
    XrRbcProgram *preserved = program;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_INVALID);
    CHECK(program == preserved);
    XrRbcProgram *unsupported = NULL;
    CHECK(xr_rbc_compile(m, unused, &unsupported, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED);
    CHECK(!unsupported);
    xr_lir_module_destroy(m);
    CHECK(run(program, NULL, 0) == 17);
    uint64_t result = 44;
    CHECK(execute(program, NULL, 1, &result) == XR_RBC_INVALID && result == 44);
    xr_rbc_program_destroy(program);

    // Publication rejects a value defined only on one sibling branch.
    m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    root = test_function(m, "bad_dominance", NULL, 0, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0);
    XrLirBlock left = test_block(m, root, NULL, 0), right = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue condition = test_constant(m, entry, XR_LIR_I1, 1);
    XrLirValue sibling = test_constant(m, left, XR_LIR_I64, 1);
    conditional(m, entry, condition, (XrLirEdge) {.target = left}, (XrLirEdge) {.target = right});
    test_return(m, left, sibling);
    test_return(m, right, sibling);
    XrLirStatus finished = xr_lir_module_finish(m, NULL);
    CHECK(finished == XR_LIR_VERIFY_ERROR && !xr_lir_module_is_readable(m));
    program = NULL;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_INVALID);
    CHECK(!program);
    xr_lir_module_destroy(m);
}

static void test_owned_diagnostic_and_rejection(void) {
    XrLirType type = XR_LIR_I64;
    XrLirIntrinsic intrinsic = {123, &type, 1, type, true, 0};
    XrLirRegistry registry = {&intrinsic, 1};
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(&registry, &m));
    XrLirFunction root = test_function(m, "intrinsic", &type, 1, type);
    XrLirBlock entry = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue arg = parameter(m, root, 0), result;
    char path[] = "owned-source.xr";
    XrLirInstruction i = {.op = XR_LIR_CALL_INTRINSIC, .type = type,
        .operands = &arg, .operand_count = 1, .intrinsic = 123,
        .source = {path, sizeof(path) - 1, 17, 9, 17, 15}};
    OK(xr_lir_emit_value(m, entry, &i, &result));
    memset(path, 'x', sizeof(path) - 1);
    test_return(m, entry, result);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED);
    CHECK(!program);
    xr_lir_module_destroy(m);
    CHECK(strcmp(diagnostic.path, "owned-source.xr") == 0);
    CHECK(diagnostic.line == 17 && diagnostic.column == 9 && diagnostic.instruction == 1);

    m = test_sum();
    OK(xr_lir_function_at(m, 2, &root));
    CHECK(xr_rbc_compile(m, root, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    CHECK(xr_rbc_program_view(program)->native_count == XR_RBC_NATIVE_COUNT &&
        xr_rbc_program_view(program)->natives[0] == XR_RBC_NATIVE_PRINT_I64);
    xr_lir_module_destroy(m);
    xr_rbc_program_destroy(program);
    program = NULL;
    CHECK(xr_rbc_compile(NULL, (XrLirFunction) {0}, &program, &diagnostic) == XR_RBC_COMPILE_INVALID);
}

static void test_i64_to_f64_admission(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType input = XR_LIR_I64;
    XrLirFunction f = test_function(m, "conversion", &input, 1, XR_LIR_F64);
    XrLirBlock b = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, b));
    XrLirValue a = parameter(m, f, 0), result;
    XrLirInstruction i = {.op = XR_LIR_ITOF, .type = XR_LIR_F64,
        .operands = &a, .operand_count = 1,
        .source = {"conversion.xr", 13, 3, 5, 3, 9}};
    OK(xr_lir_emit_value(m, b, &i, &result));
    test_return(m, b, result);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, f);
    xr_lir_module_destroy(m);
    const uint64_t arguments[] = {3, (uint64_t) -INT64_C(3), INT64_MAX};
    const uint64_t expected[] = {UINT64_C(0x4008000000000000),
        UINT64_C(0xc008000000000000), UINT64_C(0x43e0000000000000)};
    for (size_t n = 0; n < sizeof(arguments) / sizeof(arguments[0]); ++n)
        CHECK(run(program, &arguments[n], 1) == expected[n]);
    xr_rbc_program_destroy(program);
}

static void test_f64_constant_comparison(void) {
    const uint64_t constants[] = {0, UINT64_C(0x8000000000000000),
        UINT64_C(0x4010000000000000), UINT64_C(0x7ff0000000000000),
        UINT64_C(0x7ff8000000000000)};
    for (unsigned n = 0; n < sizeof(constants) / sizeof(*constants); ++n) {
        for (unsigned multiuse = 0; multiuse < 2; ++multiuse) {
            XrLirModule *m = NULL;
            OK(xr_lir_module_create(NULL, &m));
            XrLirType type = XR_LIR_F64;
            XrLirFunction f = test_function(m, "constant_comparison", &type, 1, XR_LIR_I1);
            XrLirBlock body = test_block(m, f, NULL, 0);
            OK(xr_lir_set_entry(m, f, body));
            XrLirValue input = parameter(m, f, 0);
            XrLirValue constant = test_constant(m, body, type, constants[n]);
            XrLirValue operands[] = {input, constant}, result;
            XrLirInstruction comparison = {.op = XR_LIR_FCMP, .type = XR_LIR_I1,
                .predicate = XR_LIR_LE, .operands = operands, .operand_count = 2};
            OK(xr_lir_emit_value(m, body, &comparison, &result));
            if (multiuse) {
                XrLirInstruction add = {.op = XR_LIR_FADD, .type = type,
                    .operands = operands, .operand_count = 2};
                XrLirValue unused;
                OK(xr_lir_emit_value(m, body, &add, &unused));
            }
            test_return(m, body, result);
            OK(xr_lir_module_finish(m, NULL));
            XrRbcProgram *program = compile(m, f);
            xr_lir_module_destroy(m);
            const XrRbcFunction *bytecode = &xr_rbc_program_view(program)->functions[0];
            unsigned encoded = 0, loaded = 0;
            for (size_t pc = 0; pc < bytecode->code_count; ++pc) {
                uint32_t op = bytecode->code[pc] & 255;
                encoded += op == XR_RBC_FLEK;
                loaded += op == XR_RBC_LOADK;
            }
            CHECK(encoded == (unsigned)(n != 0));
            CHECK(loaded == (unsigned)(n != 0 && multiuse));
            CHECK(bytecode->constant_count == (size_t)(n != 0));
            const uint64_t arguments[] = {0, UINT64_C(0x4010000000000000),
                UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000000000000)};
            for (unsigned a = 0; a < sizeof(arguments) / sizeof(*arguments); ++a) {
                bool expected = n != 4 && a != 3 &&
                    (n == 3 || a == 0 || (n == 2 && a == 1));
                CHECK(run(program, &arguments[a], 1) == (uint64_t)expected);
            }
            xr_rbc_program_destroy(program);
        }
    }
}

static void test_f64_constant_spills_and_phi(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType type = XR_LIR_F64;
    XrLirFunction f = test_function(m, "constant_spill_phi", &type, 1, XR_LIR_I1);
    XrLirBlock body = test_block(m, f, NULL, 0), yes = test_block(m, f, NULL, 0);
    XrLirBlock no = test_block(m, f, NULL, 0), join = test_block(m, f, &type, 1);
    OK(xr_lir_set_entry(m, f, body));
    XrLirValue input = parameter(m, f, 0), constants[257], result = {0};
    for (unsigned n = 0; n < 257; ++n)
        constants[n] = test_constant(m, body, type,
            UINT64_C(0x3ff0000000000000) + ((uint64_t)n << 44));
    for (unsigned n = 0; n < 257; ++n) {
        XrLirValue operands[] = {input, constants[n]};
        XrLirInstruction comparison = {.op = XR_LIR_FCMP, .type = XR_LIR_I1,
            .predicate = XR_LIR_LE, .operands = operands, .operand_count = 2};
        OK(xr_lir_emit_value(m, body, &comparison, &result));
    }
    conditional(m, body, result, (XrLirEdge){yes, NULL, 0}, (XrLirEdge){no, NULL, 0});
    test_branch(m, yes, join, constants, 1);
    test_branch(m, no, join, constants + 256, 1);
    XrLirValue operands[] = {input, block_parameter(m, join, 0)};
    XrLirInstruction comparison = {.op = XR_LIR_FCMP, .type = XR_LIR_I1,
        .predicate = XR_LIR_LE, .operands = operands, .operand_count = 2};
    OK(xr_lir_emit_value(m, join, &comparison, &result));
    test_return(m, join, result);
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, f);
    xr_lir_module_destroy(m);
    const XrRbcFunction *bytecode = &xr_rbc_program_view(program)->functions[0];
    CHECK(bytecode->spill_count && bytecode->constant_count == 257);
    unsigned encoded = 0, ordinary = 0;
    bool last_encodable = false, first_fallback = false;
    for (size_t pc = 0; pc < bytecode->code_count; ++pc) {
        uint32_t word = bytecode->code[pc], op = word & 255;
        encoded += op == XR_RBC_FLEK;
        ordinary += op == XR_RBC_FLE;
        last_encodable |= op == XR_RBC_FLEK && (word >> 24) == 255;
        first_fallback |= op == XR_RBC_LOADK && (word >> 16) == 256;
    }
    CHECK(encoded == 256 && ordinary == 2 && last_encodable && first_fallback);
    uint64_t argument = 0;
    CHECK(run(program, &argument, 1) == 1);
    argument = UINT64_C(0x7ff0000000000000);
    CHECK(run(program, &argument, 1) == 0);
    argument = UINT64_C(0x7ff8000000000000);
    CHECK(run(program, &argument, 1) == 0);
    xr_rbc_program_destroy(program);
}

static void test_f64_constants_and_refusals(void) {
    const uint64_t bits[] = {0, UINT64_C(0x8000000000000000),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8000000000000)};
    for (size_t n = 0; n < sizeof(bits) / sizeof(bits[0]); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunction f = test_function(m, "constant", NULL, 0, XR_LIR_F64);
        XrLirBlock b = test_block(m, f, NULL, 0);
        OK(xr_lir_set_entry(m, f, b));
        test_return(m, b, test_constant(m, b, XR_LIR_F64, bits[n]));
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = compile(m, f);
        xr_lir_module_destroy(m);
        CHECK(run(program, NULL, 0) == bits[n]);
        xr_rbc_program_destroy(program);
    }
    const struct { XrLirOp op; XrLirType input, output; uint32_t arity; } cases[] = {
        {XR_LIR_FADD, XR_LIR_F32, XR_LIR_F32, 2},
        {XR_LIR_FSUB, XR_LIR_F32, XR_LIR_F32, 2},
        {XR_LIR_FMUL, XR_LIR_F32, XR_LIR_F32, 2},
        {XR_LIR_FDIV, XR_LIR_F32, XR_LIR_F32, 2},
        {XR_LIR_FNEG, XR_LIR_F32, XR_LIR_F32, 1},
        {XR_LIR_FCMP, XR_LIR_F32, XR_LIR_I1, 2},
        {XR_LIR_FCAST, XR_LIR_F64, XR_LIR_F64, 1},
        {XR_LIR_INTCAST, XR_LIR_I8, XR_LIR_I64, 1},
        {XR_LIR_INTCAST, XR_LIR_I64, XR_LIR_I8, 1},
        {XR_LIR_DIV, XR_LIR_U64, XR_LIR_U64, 2},
        {XR_LIR_REM, XR_LIR_U64, XR_LIR_U64, 2}
    };
    for (size_t n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunction f = test_function(m, "refused", &cases[n].input, 1, cases[n].output);
        XrLirBlock b = test_block(m, f, NULL, 0);
        OK(xr_lir_set_entry(m, f, b));
        XrLirValue a = parameter(m, f, 0), arguments[] = {a, a}, result;
        XrLirInstruction i = {.op = cases[n].op, .type = cases[n].output,
            .operands = arguments, .operand_count = cases[n].arity,
            .predicate = cases[n].op == XR_LIR_FCMP ? XR_LIR_EQ :
                cases[n].op == XR_LIR_ICMP ? XR_LIR_LT : XR_LIR_PREDICATE_NONE,
            .source = {"refused.xr", 10, 3, 5, 3, 9}};
        OK(xr_lir_emit_value(m, b, &i, &result));
        test_return(m, b, result);
        OK(xr_lir_module_finish(m, NULL));
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic diagnostic;
        CHECK(xr_rbc_compile(m, f, &program, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED);
        CHECK(!program);
        xr_lir_module_destroy(m);
        CHECK(!strcmp(diagnostic.path, "refused.xr") && diagnostic.line == 3);
    }
    // Unsupported value admission still owns the available defining location.
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirFunction f = test_function(m, "narrow_value", NULL, 0, XR_LIR_I64);
    XrLirBlock b = test_block(m, f, NULL, 0);
    OK(xr_lir_set_entry(m, f, b));
    XrLirValue unused;
    XrLirInstruction i = {.op = XR_LIR_CONST, .type = XR_LIR_U16, .bits = 0,
        .source = {"unsupported-value.xr", 20, 7, 11, 7, 14}};
    OK(xr_lir_emit_value(m, b, &i, &unused));
    test_return(m, b, test_constant(m, b, XR_LIR_I64, 0));
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, f, &program, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED);
    CHECK(!program);
    xr_lir_module_destroy(m);
    CHECK(!strcmp(diagnostic.path, "unsupported-value.xr") && diagnostic.line == 7 &&
        diagnostic.column == 11 && diagnostic.instruction == 1);
}

static XrRbcProgram *panic_program(XrLirOp op, bool unknown, const char *path, unsigned located) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType flag_type = XR_LIR_I1, types[] = {XR_LIR_I64, XR_LIR_I64};
    XrLirFunction root = test_function(m, "choose", &flag_type, 1, XR_LIR_I64);
    XrLirFunction helpers[] = {test_function(m, "left", types, 2, XR_LIR_I64),
                               test_function(m, "right", types, 2, XR_LIR_I64)};
    XrLirBlock entry = test_block(m, root, NULL, 0);
    XrLirBlock yes = test_block(m, root, NULL, 0), no = test_block(m, root, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    XrLirValue arguments[] = {test_constant(m, entry, XR_LIR_I64, 17),
                              test_constant(m, entry, XR_LIR_I64, 0)};
    XrLirValue zero = test_constant(m, entry, flag_type, 0);
    XrLirValue operands[] = {parameter(m, root, 0), zero}, condition;
    XrLirInstruction predicate = {
        .op = XR_LIR_ICMP,
        .type = XR_LIR_I1,
        .operands = operands,
        .operand_count = 2,
        .predicate = XR_LIR_NE,
        .source = located & 1 ? (XrLirSource) {"choose.xr", 9, 11, 1, 11, 4} : (XrLirSource) {0}};
    OK(xr_lir_emit_value(m, entry, &predicate, &condition));
    XrLirInstruction branch = {.op = XR_LIR_CONDBR,
                               .type = XR_LIR_UNIT,
                               .operands = &condition,
                               .operand_count = 1,
                               .edge_count = 2,
                               .edges = {{yes, NULL, 0}, {no, NULL, 0}},
                               .source = located & 2 ? (XrLirSource) {"choose.xr", 9, 12, 1, 12, 4}
                                                     : (XrLirSource) {0}};
    OK(xr_lir_set_terminator(m, entry, &branch));
    test_return(m, yes, call(m, yes, helpers[0], XR_LIR_I64, arguments, 2));
    test_return(m, no, call(m, no, helpers[1], XR_LIR_I64, arguments, 2));
    for (unsigned h = 0; h < 2; ++h) {
        XrLirBlock b = test_block(m, helpers[h], NULL, 0);
        OK(xr_lir_set_entry(m, helpers[h], b));
        XrLirValue one, result;
        XrLirInstruction instruction = {.op = XR_LIR_CONST,
                                        .type = XR_LIR_I64,
                                        .bits = 1,
                                        .source = {path, strlen(path), 3, 2, 3, 4}};
        OK(xr_lir_emit_value(m, b, &instruction, &one));
        XrLirValue lhs =
            test_binary(m, b, XR_LIR_ADD, parameter(m, helpers[h], 0), one, XR_LIR_PREDICATE_NONE);
        XrLirValue operands[] = {lhs, parameter(m, helpers[h], 1)};
        XrLirSource source =
            unknown && h ? (XrLirSource) {0}
                         : (XrLirSource) {path, strlen(path), h ? 29u : 17u, 5, h ? 30u : 18u, 9};
        instruction = (XrLirInstruction) {.op = op,
                                          .type = XR_LIR_I64,
                                          .operands = operands,
                                          .operand_count = 2,
                                          .source = source};
        OK(xr_lir_emit_value(m, b, &instruction, &result));
        test_return(m, b, result);
    }
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    xr_lir_module_destroy(m);
    return program;
}

static void test_owned_arithmetic_panics(void) {
    for (unsigned op = 0; op < 2; ++op) {
        for (unsigned located = 0; located < 4; ++located) {
            unsigned prefix = (located & 1 ? 1u : 0u) + (located & 2 ? 1u : 0u);
            for (unsigned unknown = 0; unknown < 2; ++unknown) {
                for (unsigned program_first = 0; program_first < 2; ++program_first) {
                    char *path = xr_malloc(401), expected[401];
                    CHECK(path);
                    memset(path, 'p', 400);
                    memcpy(path + 380, "\xc3\xa9\n\"\\.xr", 8);
                    path[400] = 0;
                    memcpy(expected, path, 401);
                    XrRbcProgram *program =
                        panic_program(op ? XR_LIR_REM : XR_LIR_DIV, unknown != 0, path, located);
                    memset(path, 'x', 400);
                    xr_free(path);
                    XrRbcOutcome *outcomes[2];
                    for (unsigned h = 0; h < 2; ++h) {
                        uint64_t argument = h ? 0 : 1;
                        XrRbcExecution *execution = NULL;
                        CHECK(xr_rbc_program_prepare(program, &argument, 1, &execution) ==
                              XR_RBC_OK);
                        outcomes[h] = xr_rbc_execution_run(execution);
                        CHECK(outcomes[h] && !xr_rbc_execution_run(execution));
                        xr_rbc_execution_free(&execution);
                    }
                    if (program_first)
                        xr_rbc_program_destroy(program);
                    for (unsigned h = 0; h < 2; ++h) {
                        XrRbcOutcome *outcome = outcomes[h];
                        CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_TYPED_PANIC);
                        uint64_t bits = 919;
                        CHECK(!xr_rbc_outcome_bits(outcome, &bits) && bits == 919);
                        XrtArithmeticPanic facts = {77, 88};
                        const XrtLocationOwner *borrowed = NULL;
                        CHECK(!xr_rbc_outcome_arithmetic_panic(outcome, NULL, &borrowed) &&
                              !borrowed);
                        CHECK(!xr_rbc_outcome_arithmetic_panic(outcome, &facts, NULL));
                        CHECK(facts.code == 77 && facts.source_id == 88);
                        CHECK(xr_rbc_outcome_arithmetic_panic(outcome, &facts, &borrowed));
                        CHECK(facts.code == (op ? 421u : 420u));
                        bool known = !unknown || !h;
                        CHECK(facts.source_id == (known ? prefix + (h ? 4u : 2u) : 0u));
                        XrtOutputFailure output = {.source_id = 99};
                        unsigned char before[sizeof(output)];
                        memcpy(before, &output, sizeof(output));
                        const XrtLocationOwner *same = borrowed;
                        CHECK(!xr_rbc_outcome_output_failure(outcome, &output, &borrowed));
                        CHECK(!memcmp(before, &output, sizeof(output)) && borrowed == same);
                        XrtLocationOwner *retained = (XrtLocationOwner *) borrowed;
                        CHECK(xrt_locations_retain(retained));
                        xr_rbc_outcome_free(&outcome);
                        CHECK(xrt_locations_count(retained) == prefix + (unknown ? 3u : 4u));
                        const XrtSourceLocation *location =
                            xrt_locations_get(retained, facts.source_id);
                        if (known) {
                            CHECK(location && location->path_length == 400);
                            CHECK(!memcmp(location->path, expected, 400));
                            CHECK(location->line == (h ? 29u : 17u) && location->column == 5);
                            CHECK(location->end_line == (h ? 30u : 18u) &&
                                  location->end_column == 9);
                        } else
                            CHECK(!location);
                        xrt_locations_release(retained);
                    }
                    if (!program_first)
                        xr_rbc_program_destroy(program);
                }
            }
        }
    }
}

static XrLirValue cell_value(XrLirModule *m, XrLirBlock block, XrLirOp op, XrLirValue operand) {
    XrLirInstruction instruction = {.op = op,
        .type = op == XR_LIR_CELL_NEW ? XR_LIR_I64_CELL : XR_LIR_I64,
        .operands = &operand, .operand_count = 1};
    XrLirValue result;
    OK(xr_lir_emit_value(m, block, &instruction, &result));
    return result;
}

static void test_cell_transport(void) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    XrLirType flag = XR_LIR_I1, cells[] = {XR_LIR_I64_CELL, XR_LIR_I64_CELL};
    XrLirFunction root = test_function(m, "root", &flag, 1, XR_LIR_I64);
    XrLirFunction helper = test_function(m, "alias", cells, 2, XR_LIR_I64);
    XrLirBlock entry = test_block(m, root, NULL, 0), join = test_block(m, root, cells, 2);
    XrLirBlock body = test_block(m, helper, NULL, 0);
    OK(xr_lir_set_entry(m, root, entry));
    OK(xr_lir_set_entry(m, helper, body));
    XrLirValue a = cell_value(m, entry, XR_LIR_CELL_NEW, test_constant(m, entry, XR_LIR_I64, 7));
    XrLirValue b = cell_value(m, entry, XR_LIR_CELL_NEW, test_constant(m, entry, XR_LIR_I64, 20));
    XrLirValue yes[] = {a, b}, no[] = {b, a};
    conditional(m, entry, parameter(m, root, 0), (XrLirEdge){join, yes, 2}, (XrLirEdge){join, no, 2});
    XrLirValue selected = block_parameter(m, join, 0), other = block_parameter(m, join, 1);
    XrLirValue arguments[] = {selected, selected};
    XrLirValue first = call(m, join, helper, XR_LIR_I64, arguments, 2);
    arguments[0] = arguments[1] = other;
    XrLirValue second = call(m, join, helper, XR_LIR_I64, arguments, 2);
    first = test_binary(m, join, XR_LIR_MUL, first, test_constant(m, join, XR_LIR_I64, 100),
        XR_LIR_PREDICATE_NONE);
    test_return(m, join, test_binary(m, join, XR_LIR_ADD, first, second, XR_LIR_PREDICATE_NONE));
    XrLirValue value = cell_value(m, body, XR_LIR_CELL_READ, parameter(m, helper, 0));
    value = test_binary(m, body, XR_LIR_ADD, value, test_constant(m, body, XR_LIR_I64, 1),
        XR_LIR_PREDICATE_NONE);
    XrLirValue write[] = {parameter(m, helper, 1), value};
    XrLirInstruction instruction = {.op = XR_LIR_CELL_WRITE, .type = XR_LIR_UNIT,
        .operands = write, .operand_count = 2};
    OK(xr_lir_emit_effect(m, body, &instruction));
    test_return(m, body, cell_value(m, body, XR_LIR_CELL_READ, parameter(m, helper, 0)));
    OK(xr_lir_module_finish(m, NULL));
    XrRbcProgram *program = compile(m, root);
    CHECK(xr_rbc_program_view(program)->functions[0].cell_count == 2);
    XrRbcProgram *rejected = NULL;
    XrRbcCompileDiagnostic diagnostic;
    CHECK(xr_rbc_compile(m, helper, &rejected, &diagnostic) == XR_RBC_COMPILE_UNSUPPORTED && !rejected);
    CHECK(strstr(diagnostic.message, "cell argument"));
    xr_lir_module_destroy(m);
    uint64_t choose = 1;
    XrRbcExecution *first_execution = NULL, *second_execution = NULL;
    CHECK(xr_rbc_program_prepare(program, &choose, 1, &first_execution) == XR_RBC_OK);
    CHECK(xr_rbc_program_prepare(program, &choose, 1, &second_execution) == XR_RBC_OK);
    XrRbcOutcome *first_outcome = xr_rbc_execution_run(first_execution);
    CHECK(!xr_rbc_execution_run(first_execution));
    XrRbcOutcome *second_outcome = xr_rbc_execution_run(second_execution);
    xr_rbc_execution_free(&first_execution);
    xr_rbc_execution_free(&second_execution);
    choose = 0;
    CHECK(run(program, &choose, 1) == 2108);
    xr_rbc_program_destroy(program);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_bits(first_outcome, &bits) && bits == 821);
    CHECK(xr_rbc_outcome_bits(second_outcome, &bits) && bits == 821);
    xr_rbc_outcome_free(&first_outcome);
    xr_rbc_outcome_free(&second_outcome);
}


static void test_cell_escape_rejections(void) {
    for (unsigned kind = 0; kind < 3; ++kind) {
        XrLirModule *m = NULL;
        OK(xr_lir_module_create(NULL, &m));
        XrLirFunction root = {0};
        if (kind == 1) {
            XrLirFunctionSpec spec = {.name = "invalid_cell", .name_length = 12,
                .result = XR_LIR_I64_CELL};
            CHECK(xr_lir_declare_function(m, &spec, &root) == XR_LIR_INVALID_ARGUMENT && !root.id);
        } else {
            root = test_function(m, "invalid_cell", NULL, 0, XR_LIR_I64);
            XrLirBlock body = test_block(m, root, NULL, 0);
            OK(xr_lir_set_entry(m, root, body));
            XrLirInstruction instruction = {.op = XR_LIR_CONST, .type = XR_LIR_I64_CELL, .bits = 1};
            XrLirValue rejected = {0};
            if (kind == 0) {
                CHECK(xr_lir_emit_value(m, body, &instruction, &rejected) == XR_LIR_VERIFY_ERROR);
                CHECK(!rejected.id);
            } else {
                XrLirValue value = test_constant(m, body, XR_LIR_I64, 7);
                XrLirValue cell = cell_value(m, body, XR_LIR_CELL_NEW, value);
                instruction = (XrLirInstruction) {.op = XR_LIR_CALL, .type = XR_LIR_UNIT,
                    .callee = test_print_declaration(m), .operands = &cell, .operand_count = 1};
                CHECK(xr_lir_emit_effect(m, body, &instruction) == XR_LIR_VERIFY_ERROR);
            }
        }
        XrRbcProgram *program = NULL;
        CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_INVALID && !program);
        xr_lir_module_destroy(m);
    }
}


static XrLirModule *literal_module(XrLirFunction *root, unsigned unsigned_use) {
    XrLirModule *m = NULL;
    OK(xr_lir_module_create(NULL, &m));
    unsigned char bytes[] = {'A', 0, 0xc3, 0xa9};
    XrLirLiteral literal = {0}, unused = {0};
    OK(xr_lir_add_literal(m, bytes, sizeof(bytes), &literal));
    OK(xr_lir_add_literal(m, "unused", 6, &unused));
    memset(bytes, 0xa5, sizeof(bytes));
    XrLirType flag = XR_LIR_I1, types[] = {XR_LIR_PTR, XR_LIR_U64};
    *root = test_function(m, "literal", &flag, 1, XR_LIR_UNIT);
    XrLirFunction print;
    XrLirFunctionSpec spec = {.name="xrt_print_string", .name_length=16,
        .parameters=types, .parameter_count=2, .result=XR_LIR_UNIT,
        .kind=XR_LIR_FUNCTION_EXTERNAL, .extern_kind=XR_LIR_EXTERN_PRINT_STRING};
    OK(xr_lir_declare_function(m, &spec, &print));
    XrLirBlock entry = test_block(m, *root, NULL, 0), output = test_block(m, *root, NULL, 0);
    XrLirBlock finish = test_block(m, *root, NULL, 0);
    OK(xr_lir_set_entry(m, *root, entry));
    conditional(m, entry, parameter(m, *root, 0), (XrLirEdge){output, NULL, 0},
        (XrLirEdge){finish, NULL, 0});
    XrLirValue length = test_constant(m, output, XR_LIR_U64, sizeof(bytes));
    for (unsigned i = 0; i < 2; ++i) {
        XrLirValue args[2] = {{0}, length};
        OK(xr_lir_literal_address(m, output, literal, &args[0]));
        call(m, output, print, XR_LIR_UNIT, args, 2);
    }
    if (unsigned_use == 1 || unsigned_use == 2) {
        XrLirValue args[] = {length, length}, result;
        XrLirInstruction i = {.op=unsigned_use == 1 ? XR_LIR_ICMP : XR_LIR_ADD,
            .type=unsigned_use == 1 ? XR_LIR_I1 : XR_LIR_U64,
            .predicate=unsigned_use == 1 ? XR_LIR_LT : XR_LIR_PREDICATE_NONE,
            .operands=args, .operand_count=2};
        OK(xr_lir_emit_value(m, output, &i, &result));
    }
    if (unsigned_use == 3) {
        XrLirType type = XR_LIR_U64;
        XrLirBlock transport = test_block(m, *root, &type, 1);
        test_branch(m, output, transport, &length, 1);
        test_branch(m, transport, finish, NULL, 0);
    } else test_branch(m, output, finish, NULL, 0);
    test_return(m, finish, (XrLirValue){0});
    OK(xr_lir_module_finish(m, NULL));
    return m;
}

static void test_literal_ownership(void) {
    XrLirFunction root;
    XrLirModule *m = literal_module(&root, false);
    XrRbcProgram *program = compile(m, root);
    const XrRbcModule *module = xr_rbc_program_view(program);
    CHECK(module->literal_count == 1 && module->literals[0].length == 4);
    const unsigned char expected[] = {'A', 0, 0xc3, 0xa9};
    CHECK(!memcmp(module->literals[0].bytes, expected, sizeof(expected)));
    uint32_t prints = 0;
    for (size_t pc = 0; pc < module->functions[0].code_count; ++pc)
        if ((module->functions[0].code[pc] & 255) == XR_RBC_PRINT_LITERAL) {
            CHECK((module->functions[0].code[pc] >> 8) == 0);++prints;
        }
    CHECK(prints == 2);
    XrLirLiteral input;
    const uint8_t *bytes = NULL;size_t length = 0;
    OK(xr_lir_literal_at(m, 0, &input));OK(xr_lir_literal_view(m, input, &bytes, &length));
    CHECK((const void *)bytes != (const void *)module->literals[0].bytes);
    xr_lir_module_destroy(m);
    CHECK(!memcmp(module->literals[0].bytes, expected, sizeof(expected)));
    XrRbcOutcome *outcomes[2];
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t flag = 0;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_program_prepare(program, &flag, 1, &execution) == XR_RBC_OK);
        outcomes[i] = xr_rbc_execution_run(execution);
        CHECK(outcomes[i] && !xr_rbc_execution_run(execution));
        xr_rbc_execution_free(&execution);
    }
    xr_rbc_program_destroy(program);
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t bits = 99;
        CHECK(xr_rbc_outcome_bits(outcomes[i], &bits) && !bits);
        xr_rbc_outcome_free(&outcomes[i]);
    }
    program = NULL;
    for (unsigned i = 1; i <= 3; ++i) {
        m = literal_module(&root, i);
        CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED && !program);
        xr_lir_module_destroy(m);
    }
    XrLirType types[] = {XR_LIR_PTR, XR_LIR_U64};
    for (unsigned i = 0; i < 2; ++i) {
        m = NULL;OK(xr_lir_module_create(NULL, &m));
        root = test_function(m, "opaque_argument", &types[i], 1, XR_LIR_UNIT);
        XrLirBlock b = test_block(m, root, NULL, 0);OK(xr_lir_set_entry(m, root, b));
        test_return(m, b, (XrLirValue){0});OK(xr_lir_module_finish(m, NULL));
        if (types[i] == XR_LIR_U64) {
            program = compile(m, root);
            xr_lir_module_destroy(m);
            const uint64_t arguments[] = {0, UINT64_C(0x8000000000000000), UINT64_MAX};
            for (size_t n = 0; n < sizeof(arguments) / sizeof(*arguments); ++n)
                CHECK(run(program, &arguments[n], 1) == 0);
            xr_rbc_program_destroy(program);
            program = NULL;
        } else {
            CHECK(xr_rbc_compile(m, root, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED && !program);
            xr_lir_module_destroy(m);
        }
    }
}

static void test_string_edge_swap(void) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL,&m));
    XrLirType flag = XR_LIR_I1, types[] = {XR_LIR_STRING,XR_LIR_STRING};
    XrLirFunction f = test_function(m,"string_swap",&flag,1,XR_LIR_STRING);
    XrLirBlock entry=test_block(m,f,NULL,0), left=test_block(m,f,types,2),
        right=test_block(m,f,types,2), join=test_block(m,f,types,2);
    OK(xr_lir_set_entry(m,f,entry));
    XrLirValue values[2];
    for (unsigned i=0;i<2;++i) {
        XrLirLiteral literal = {0}; const char *text=i?"B":"A"; OK(xr_lir_add_literal(m,text,1,&literal));
        XrLirInstruction v={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
        OK(xr_lir_emit_value(m,entry,&v,&values[i]));
    }
    conditional(m,entry,parameter(m,f,0),(XrLirEdge){left,values,2},(XrLirEdge){right,values,2});
    XrLirValue order[]={block_parameter(m,left,0),block_parameter(m,left,1)};
    test_branch(m,left,join,order,2);
    order[0]=block_parameter(m,right,1);order[1]=block_parameter(m,right,0);test_branch(m,right,join,order,2);
    order[0]=block_parameter(m,join,0);order[1]=block_parameter(m,join,1);
    XrLirInstruction concat={.op=XR_LIR_STRING_CONCAT,.type=XR_LIR_STRING,.operands=order,.operand_count=2};
    XrLirValue result; OK(xr_lir_emit_value(m,join,&concat,&result));
    for (unsigned i=0;i<2;++i) {
        XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&order[i],.operand_count=1};
        OK(xr_lir_emit_effect(m,join,&drop));
    }
    test_return(m,join,result);OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,f);xr_lir_module_destroy(m);
    for (uint64_t flag_value=0;flag_value<2;++flag_value) {
        XrRbcExecution *execution=NULL;CHECK(xr_rbc_program_prepare(program,&flag_value,1,&execution)==XR_RBC_OK);
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);CHECK(outcome&&xr_rbc_outcome_status(outcome)==XR_RBC_OK);
        xr_rbc_execution_free(&execution);
        if(flag_value){xr_rbc_program_destroy(program);program=NULL;}
        const XrtManagedHeader *text=NULL;CHECK(xr_rbc_outcome_string(outcome,&text));
        const XrtStringBacking *backing=(const XrtStringBacking *)text;size_t size=backing->length;const char *bytes=backing->bytes;
        CHECK(size==2&&!memcmp(bytes,flag_value?"AB":"BA",2));xr_rbc_outcome_free(&outcome);
    }
}
static void test_loop_string_temporaries(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));XrLirType type=XR_LIR_I64;
    XrLirFunction f=test_function(m,"temporary_loop",&type,1,XR_LIR_I64),helper=test_function(m,"next",&type,1,XR_LIR_I64);
    XrLirBlock increment=test_block(m,helper,NULL,0);OK(xr_lir_set_entry(m,helper,increment));
    XrLirValue argument=parameter(m,helper,0),one=test_constant(m,increment,XR_LIR_I64,1);
    test_return(m,increment,test_binary(m,increment,XR_LIR_ADD,argument,one,XR_LIR_PREDICATE_NONE));
    XrLirBlock entry=test_block(m,f,NULL,0),header=test_block(m,f,&type,1),body=test_block(m,f,NULL,0),done=test_block(m,f,NULL,0);
    OK(xr_lir_set_entry(m,f,entry));XrLirValue zero=test_constant(m,entry,XR_LIR_I64,0);test_branch(m,entry,header,&zero,1);
    XrLirValue iteration=block_parameter(m,header,0),limit=parameter(m,f,0);
    XrLirValue condition=test_binary(m,header,XR_LIR_ICMP,iteration,limit,XR_LIR_LT);
    conditional(m,header,condition,(XrLirEdge){body,NULL,0},(XrLirEdge){done,NULL,0});
    XrLirLiteral literal={0};const unsigned char bytes[]={0xc3,0xa9,0};OK(xr_lir_add_literal(m,bytes,sizeof(bytes),&literal));
    XrLirValue pieces[3];XrLirInstruction string={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    OK(xr_lir_emit_value(m,body,&string,&pieces[0]));string=(XrLirInstruction){.op=XR_LIR_STRING_I64,.type=XR_LIR_STRING,.operands=&iteration,.operand_count=1};
    OK(xr_lir_emit_value(m,body,&string,&pieces[1]));string=(XrLirInstruction){.op=XR_LIR_STRING_CONCAT,.type=XR_LIR_STRING,.operands=pieces,.operand_count=2};
    OK(xr_lir_emit_value(m,body,&string,&pieces[2]));
    for(unsigned i=0;i<3;++i){XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&pieces[i],.operand_count=1};OK(xr_lir_emit_effect(m,body,&drop));}
    XrLirValue next=call(m,body,helper,XR_LIR_I64,&iteration,1);test_branch(m,body,header,&next,1);test_return(m,done,iteration);
    OK(xr_lir_module_finish(m,NULL));XrRbcProgram *program=compile(m,f);xr_lir_module_destroy(m);
    const uint64_t counts[]={1000,3,0,1000};for(unsigned i=0;i<4;++i)CHECK(run(program,&counts[i],1)==counts[i]);
    xr_rbc_program_destroy(program);
}
static void test_sequential_string_owners(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));XrLirType flags[]={XR_LIR_I1,XR_LIR_I1},owned=XR_LIR_STRING;
    XrLirFunction f=test_function(m,"three_owned_branches",flags,2,XR_LIR_STRING);
    XrLirBlock current=test_block(m,f,NULL,0);OK(xr_lir_set_entry(m,f,current));
    const unsigned char prefix[]={'A',0};XrLirLiteral literal={0};OK(xr_lir_add_literal(m,prefix,sizeof(prefix),&literal));
    XrLirValue value;XrLirInstruction string={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};OK(xr_lir_emit_value(m,current,&string,&value));
    for(unsigned stage=0;stage<3;++stage) {
        XrLirBlock arms[]={test_block(m,f,&owned,1),test_block(m,f,&owned,1)},join=test_block(m,f,&owned,1);
        conditional(m,current,parameter(m,f,stage%2),(XrLirEdge){arms[0],&value,1},(XrLirEdge){arms[1],&value,1});
        for(unsigned arm=0;arm<2;++arm) {
            XrLirValue pieces[]={block_parameter(m,arms[arm],0),{0}};XrLirLiteral suffix={0};OK(xr_lir_add_literal(m,arm?"R":"L",1,&suffix));
            string=(XrLirInstruction){.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=suffix.id};OK(xr_lir_emit_value(m,arms[arm],&string,&pieces[1]));
            XrLirValue next;XrLirInstruction concat={.op=XR_LIR_STRING_CONCAT,.type=XR_LIR_STRING,.operands=pieces,.operand_count=2};OK(xr_lir_emit_value(m,arms[arm],&concat,&next));
            for(unsigned part=0;part<2;++part){XrLirInstruction drop={.op=XR_LIR_STRING_DROP,.type=XR_LIR_UNIT,.operands=&pieces[part],.operand_count=1};OK(xr_lir_emit_effect(m,arms[arm],&drop));}
            test_branch(m,arms[arm],join,&next,1);
        }
        current=join;value=block_parameter(m,join,0);
    }
    test_return(m,current,value);OK(xr_lir_module_finish(m,NULL));XrRbcProgram *program=compile(m,f);xr_lir_module_destroy(m);
    XrRbcOutcome *outcomes[4]={0};
    for(unsigned choice=0;choice<4;++choice){uint64_t arguments[]={choice&1u,(choice>>1)&1u};XrRbcExecution *execution=NULL;
        CHECK(xr_rbc_program_prepare(program,arguments,2,&execution)==XR_RBC_OK);outcomes[choice]=xr_rbc_execution_run(execution);
        CHECK(outcomes[choice]&&xr_rbc_outcome_status(outcomes[choice])==XR_RBC_OK);xr_rbc_execution_free(&execution);}
    xr_rbc_program_destroy(program);
    for(unsigned choice=0;choice<4;++choice){const XrtManagedHeader *text=NULL;CHECK(xr_rbc_outcome_string(outcomes[choice],&text));const XrtStringBacking *backing=(const XrtStringBacking *)text;size_t size=backing->length;const char *bytes=backing->bytes;
        const unsigned char expected[]={'A',0,(choice&1u)?'L':'R',(choice&2u)?'L':'R',(choice&1u)?'L':'R'};
        CHECK(size==sizeof(expected)&&!memcmp(bytes,expected,size));xr_rbc_outcome_free(&outcomes[choice]);}
}
static void test_scalar_spills(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirFunction root=test_function(m,"virtual_values",NULL,0,XR_LIR_I64);
    XrLirBlock body=test_block(m,root,NULL,0);OK(xr_lir_set_entry(m,root,body));
    XrLirValue value={0};
    for(uint32_t n=0;n<260;++n)value=test_constant(m,body,XR_LIR_I64,n);
    test_return(m,body,value);OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,root);
    const XrRbcFunction *physical=&xr_rbc_program_view(program)->functions[0];
    CHECK(physical->register_count==255&&physical->spill_count&&!physical->reference_spill_count);
    xr_lir_module_destroy(m);CHECK(run(program,NULL,0)==259);xr_rbc_program_destroy(program);
}
static void test_scalar_spill_branches(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirType integer=XR_LIR_I64;
    XrLirFunction root=test_function(m,"virtual_branch_values",&integer,1,integer);
    XrLirBlock entry=test_block(m,root,NULL,0),yes=test_block(m,root,NULL,0);
    XrLirBlock no=test_block(m,root,NULL,0),join=test_block(m,root,&integer,1);
    OK(xr_lir_set_entry(m,root,entry));
    XrLirValue zero=test_constant(m,entry,integer,0);
    XrLirValue condition=test_binary(m,entry,XR_LIR_ICMP,parameter(m,root,0),zero,XR_LIR_NE);
    conditional(m,entry,condition,(XrLirEdge){yes,NULL,0},(XrLirEdge){no,NULL,0});
    XrLirValue value={0};
    for (uint32_t n=0; n<260; ++n) value=test_constant(m,yes,integer,n);
    test_branch(m,yes,join,&value,1);
    value=test_constant(m,no,integer,42);test_branch(m,no,join,&value,1);
    test_return(m,join,block_parameter(m,join,0));OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,root);
    const XrRbcFunction *physical=&xr_rbc_program_view(program)->functions[0];
    CHECK(physical->register_count<=255 && physical->spill_count && !physical->reference_spill_count);
    xr_lir_module_destroy(m);
    uint64_t flag=0;CHECK(run(program,&flag,1)==42);
    flag=1;CHECK(run(program,&flag,1)==259);xr_rbc_program_destroy(program);
}
static void test_many_spilled_conditions(void) {
    XrLirModule *module=NULL;
    OK(xr_lir_module_create(NULL,&module));
    XrLirType integer=XR_LIR_I64;
    XrLirFunction function=test_function(module,"many_conditions",&integer,1,integer);
    XrLirBlock current=test_block(module,function,NULL,0);
    OK(xr_lir_set_entry(module,function,current));
    XrLirValue argument=parameter(module,function,0);
    for(uint32_t n=0;n<300;++n) {
        XrLirValue value=test_constant(module,current,integer,n);
        XrLirValue condition=test_binary(module,current,XR_LIR_ICMP,argument,value,XR_LIR_EQ);
        XrLirBlock selected=test_block(module,function,NULL,0);
        XrLirBlock next=test_block(module,function,NULL,0);
        conditional(module,current,condition,(XrLirEdge){selected,NULL,0},(XrLirEdge){next,NULL,0});
        test_return(module,selected,value);
        current=next;
    }
    test_return(module,current,test_constant(module,current,integer,UINT64_MAX));
    OK(xr_lir_module_finish(module,NULL));
    XrRbcProgram *program=compile(module,function);
    CHECK(xr_rbc_program_view(program)->functions[0].spill_count);
    xr_lir_module_destroy(module);
    const uint64_t arguments[]={0,150,299,300};
    for(unsigned n=0;n<4;++n)
        CHECK(run(program,&arguments[n],1)==(arguments[n]<300 ? arguments[n] : UINT64_MAX));
    xr_rbc_program_destroy(program);
}

static XrLirValue frame_text(XrLirModule *m,XrLirBlock block,const char *bytes,size_t length) {
    XrLirLiteral literal={0};XrLirValue value;
    OK(xr_lir_add_literal(m,bytes,length,&literal));
    XrLirInstruction op={.op=XR_LIR_STRING_LITERAL,.type=XR_LIR_STRING,.bits=literal.id};
    OK(xr_lir_emit_value(m,block,&op,&value));return value;
}
static void frame_drop(XrLirModule *m,XrLirBlock block,XrLirValue value,bool cell) {
    XrLirInstruction op={.op=cell?XR_LIR_STRING_CELL_DROP:XR_LIR_STRING_DROP,
        .type=XR_LIR_UNIT,.operands=&value,.operand_count=1};
    OK(xr_lir_emit_effect(m,block,&op));
}
static void test_string_binding_aliases(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirType bit=XR_LIR_I1,cells[]={XR_LIR_STRING_CELL,XR_LIR_STRING_CELL};
    XrLirFunction root=test_function(m,"frame_alias_caller",&bit,1,XR_LIR_I64);
    XrLirFunction helper=test_function(m,"explicit_and_hidden_borrows",cells,2,XR_LIR_I64);
    XrLirBlock entry=test_block(m,root,NULL,0),yes=test_block(m,root,NULL,0),no=test_block(m,root,NULL,0);
    OK(xr_lir_set_entry(m,root,entry));XrLirValue handles[2];
    for(unsigned i=0;i<2;++i) {
        XrLirValue text=frame_text(m,entry,i?"other":"old",i?5u:3u);
        XrLirInstruction op={.op=XR_LIR_CELL_NEW,.type=XR_LIR_STRING_CELL,.operands=&text,.operand_count=1};
        OK(xr_lir_emit_value(m,entry,&op,&handles[i]));frame_drop(m,entry,text,false);
    }
    XrLirValue condition=parameter(m,root,0);
    conditional(m,entry,condition,(XrLirEdge){yes,NULL,0},(XrLirEdge){no,NULL,0});
    for(unsigned same=0;same<2;++same) {
        XrLirBlock block=same?yes:no;XrLirValue arguments[]={handles[0],handles[same?0:1]},result;
        XrLirInstruction call={.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=helper,.operands=arguments,.operand_count=2};
        OK(xr_lir_emit_value(m,block,&call,&result));
        frame_drop(m,block,handles[0],true);frame_drop(m,block,handles[1],true);test_return(m,block,result);
    }
    XrLirBlock body=test_block(m,helper,NULL,0);OK(xr_lir_set_entry(m,helper,body));
    XrLirValue replacement=frame_text(m,body,"alias",5),write[]={parameter(m,helper,1),replacement};
    XrLirInstruction op={.op=XR_LIR_CELL_WRITE,.type=XR_LIR_UNIT,.operands=write,.operand_count=2};
    OK(xr_lir_emit_effect(m,body,&op));frame_drop(m,body,replacement,false);
    XrLirValue first=parameter(m,helper,0),read,length;
    op=(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_STRING,.operands=&first,.operand_count=1};
    OK(xr_lir_emit_value(m,body,&op,&read));
    op=(XrLirInstruction){.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,.operands=&read,.operand_count=1};
    OK(xr_lir_emit_value(m,body,&op,&length));frame_drop(m,body,read,false);test_return(m,body,length);
    OK(xr_lir_module_finish(m,NULL));XrRbcProgram *program=compile(m,root);
    xr_lir_module_destroy(m);
    uint64_t flag=0;
    CHECK(run(program,&flag,1)==3);
    flag=1;
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,&flag,1,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    uint64_t bits=0;
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK &&
        xr_rbc_outcome_bits(outcome,&bits) && bits==5);
    xr_rbc_outcome_free(&outcome);
}

static void test_scalar_edge_in_typed_function(bool spill) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));XrLirType integer=XR_LIR_I64;
    XrLirFunction f=test_function(m,"mixed_typed_copy_cycle",&integer,1,integer);
    XrLirType carried_types[]={integer,integer,integer};
    XrLirBlock entry=test_block(m,f,NULL,0),loop=test_block(m,f,carried_types,3);
    XrLirBlock body=test_block(m,f,NULL,0),done=test_block(m,f,NULL,0);
    OK(xr_lir_set_entry(m,f,entry));
    if (spill) for (uint32_t n=0;n<260;++n) (void)test_constant(m,entry,integer,n);
    // The function owns a String, but no owner crosses the scalar edge.
    XrLirValue text=frame_text(m,entry,"temporary",9);frame_drop(m,entry,text,false);
    XrLirValue initial[]={test_constant(m,entry,integer,1),test_constant(m,entry,integer,2),parameter(m,f,0)};
    test_branch(m,entry,loop,initial,3);
    XrLirValue carried[]={block_parameter(m,loop,0),block_parameter(m,loop,1),block_parameter(m,loop,2)};
    XrLirValue zero=test_constant(m,loop,integer,0);
    XrLirValue condition=test_binary(m,loop,XR_LIR_ICMP,carried[2],zero,XR_LIR_GT);
    conditional(m,loop,condition,(XrLirEdge){body,NULL,0},(XrLirEdge){done,NULL,0});
    XrLirValue next=test_binary(m,body,XR_LIR_SUB,carried[2],test_constant(m,body,integer,1),XR_LIR_PREDICATE_NONE);
    XrLirValue swapped[]={carried[1],carried[0],next};test_branch(m,body,loop,swapped,3);
    test_return(m,done,carried[0]);OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,f);
    const XrRbcModule *physical=xr_rbc_program_view(program);
    const XrRbcValueCode *code=xr_rbc_module_value_code(physical,0);
    CHECK(code && code->homes && (physical->functions[0].spill_count > 0)==spill);
    CHECK(code->homes->values[text.id].owner_count==1 &&
        code->locations[text.id].kind==XR_RBC_VALUE_NOWHERE);
    for (size_t index=0;index<code->access_count;++index)
        CHECK(code->accesses[index].operation!=XR_RBC_VALUE_EDGE);
    xr_lir_module_destroy(m);
    for(uint64_t n=0;n<4;++n)CHECK(run(program,&n,1)==(n%2?2u:1u));
    xr_rbc_program_destroy(program);
}

/* Locally produced Strings forwarded by MOVE share one stable home, so the
 * edge carries no owner transfer however the String was made. */
static void test_fresh_string_stationary_edges(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirType texts[]={XR_LIR_STRING,XR_LIR_STRING};
    XrLirFunction f=test_function(m,"fresh_string_edges",NULL,0,XR_LIR_I64);
    XrLirBlock entry=test_block(m,f,NULL,0),join=test_block(m,f,texts,2);
    OK(xr_lir_set_entry(m,f,entry));
    XrLirValue number=test_constant(m,entry,XR_LIR_I64,123),digits={0},joined={0};
    XrLirInstruction create={.op=XR_LIR_STRING_I64,.type=XR_LIR_STRING,.operands=&number,.operand_count=1};
    OK(xr_lir_emit_value(m,entry,&create,&digits));
    XrLirValue pair[]={digits,digits};
    XrLirInstruction concat={.op=XR_LIR_STRING_CONCAT,.type=XR_LIR_STRING,.operands=pair,.operand_count=2};
    OK(xr_lir_emit_value(m,entry,&concat,&joined));
    XrLirValue values[]={digits,joined};test_branch(m,entry,join,values,2);
    XrLirValue parameters[2],lengths[2];
    for(unsigned n=0;n<2;++n) {
        OK(xr_lir_block_parameter(m,join,n,&parameters[n]));
        XrLirInstruction len={.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,.operands=&parameters[n],.operand_count=1};
        OK(xr_lir_emit_value(m,join,&len,&lengths[n]));
        frame_drop(m,join,parameters[n],false);
    }
    test_return(m,join,test_binary(m,join,XR_LIR_ADD,lengths[0],lengths[1],XR_LIR_PREDICATE_NONE));
    OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,f);
    const XrLirValueHomes *homes=xr_rbc_module_value_homes(xr_rbc_program_view(program),0);
    CHECK(homes && !homes->edge_count);
    for(unsigned n=0;n<2;++n) CHECK(xr_lir_home_stationary_edge(homes,values[n].id,parameters[n].id));
    xr_lir_module_destroy(m);
    CHECK(run(program,NULL,0)==9);
    xr_rbc_program_destroy(program);
}

/* A loop carries a String owner beside two scalars that swap on every turn.
 * The typed edge operations move only the owner; the scalars travel as
 * ordinary parallel copies, including the swap cycle. */
static void test_mixed_edge_scalar_copies(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirType carried[]={XR_LIR_STRING,XR_LIR_I64,XR_LIR_I64,XR_LIR_I64};
    XrLirFunction f=test_function(m,"mixed_edge_scalars",NULL,0,XR_LIR_I64);
    XrLirFunction make=test_text_function(m,"mixed_edge_text",12345);
    XrLirBlock entry=test_block(m,f,NULL,0),loop=test_block(m,f,carried,4),body=test_block(m,f,carried,4);
    XrLirBlock done=test_block(m,f,carried,3);
    OK(xr_lir_set_entry(m,f,entry));
    XrLirValue text=test_text_call(m,entry,make);
    XrLirValue initial[]={text,test_constant(m,entry,XR_LIR_I64,3),test_constant(m,entry,XR_LIR_I64,10),
        test_constant(m,entry,XR_LIR_I64,0)};
    test_branch(m,entry,loop,initial,4);
    XrLirValue at_loop[4],at_body[4],at_done[3];
    for(unsigned n=0;n<4;++n){at_loop[n]=block_parameter(m,loop,n);at_body[n]=block_parameter(m,body,n);}
    for(unsigned n=0;n<3;++n)at_done[n]=block_parameter(m,done,n);
    XrLirValue more=test_binary(m,loop,XR_LIR_ICMP,at_loop[3],test_constant(m,loop,XR_LIR_I64,5),XR_LIR_LT);
    OK(xr_lir_set_terminator(m,loop,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&more,.operand_count=1,.edges={{body,at_loop,4},{done,at_loop,3}},.edge_count=2}));
    XrLirValue next=test_binary(m,body,XR_LIR_ADD,at_body[3],test_constant(m,body,XR_LIR_I64,1),XR_LIR_PREDICATE_NONE);
    XrLirValue swapped[]={at_body[0],at_body[2],at_body[1],next};
    test_branch(m,body,loop,swapped,4);
    XrLirValue length={0};
    XrLirInstruction len={.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,.operands=&at_done[0],.operand_count=1};
    OK(xr_lir_emit_value(m,done,&len,&length));frame_drop(m,done,at_done[0],false);
    XrLirValue high=test_binary(m,done,XR_LIR_MUL,at_done[1],test_constant(m,done,XR_LIR_I64,1000),XR_LIR_PREDICATE_NONE);
    XrLirValue low=test_binary(m,done,XR_LIR_MUL,at_done[2],test_constant(m,done,XR_LIR_I64,10),XR_LIR_PREDICATE_NONE);
    XrLirValue sum=test_binary(m,done,XR_LIR_ADD,high,low,XR_LIR_PREDICATE_NONE);
    test_return(m,done,test_binary(m,done,XR_LIR_ADD,sum,length,XR_LIR_PREDICATE_NONE));
    OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,f);
    const XrRbcValueCode *code=xr_rbc_module_value_code(xr_rbc_program_view(program),0);
    CHECK(code && code->homes && code->homes->edge_count);
    size_t owners=0;
    for(size_t n=0;n<code->access_count;++n) {
        XrRbcValueAccess access=code->accesses[n];
        if(access.operation!=XR_RBC_VALUE_EDGE) continue;
        CHECK(access.index!=UINT32_MAX);
        for(uint32_t pair=0;pair<access.count;++pair) {
            uint32_t target=code->arguments[access.first+(size_t)pair*2+1];
            CHECK(code->homes->values[target].type);
            ++owners;
        }
    }
    CHECK(owners>=3);
    xr_lir_module_destroy(m);
    CHECK(run(program,NULL,0)==10035);
    xr_rbc_program_destroy(program);
}

/* Plain local owner operations compile to their own words: a String carried
 * around a loop, retained, released and passed to a formal that retains its
 * argument. A retain from a formal keeps the generic word. Admission accepts
 * only the word the shared selector names for each access. */
static size_t owner_words(const XrRbcModule *module, uint32_t function, uint32_t op) {
    size_t count=0;
    for (size_t pc=0;pc<module->functions[function].code_count;++pc)
        count+=(module->functions[function].code[pc]&255u)==op;
    return count;
}
static void owner_word_reject(XrRbcProgram *program, uint32_t function, size_t pc, uint32_t op) {
    uint32_t *words=(uint32_t *)xr_rbc_program_view(program)->functions[function].code, saved=words[pc];
    words[pc]=(saved&~255u)|op;
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(xr_rbc_program_view(program),0,NULL,0,xr_rbc_program_locations(program),
        &execution)==XR_RBC_INVALID && !execution);
    words[pc]=saved;
}
static void test_owner_words(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));
    XrLirType text_type=XR_LIR_STRING, carried[]={XR_LIR_STRING,XR_LIR_I64};
    XrLirFunction f=test_function(m,"owner_words",NULL,0,XR_LIR_I64);
    XrLirFunction make=test_text_function(m,"owner_words_text",12345);
    XrLirFunction g=test_function(m,"owner_words_formal",&text_type,1,XR_LIR_I64);
    XrLirBlock g_body=test_block(m,g,NULL,0);OK(xr_lir_set_entry(m,g,g_body));
    XrLirValue formal={0};OK(xr_lir_function_parameter(m,g,0,&formal));
    XrLirValue kept={0},kept_length={0};
    OK(xr_lir_emit_value(m,g_body,&(XrLirInstruction){.op=XR_LIR_STRING_RETAIN,.type=XR_LIR_STRING,
        .operands=&formal,.operand_count=1},&kept));
    OK(xr_lir_emit_value(m,g_body,&(XrLirInstruction){.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,
        .operands=&kept,.operand_count=1},&kept_length));
    frame_drop(m,g_body,kept,false);test_return(m,g_body,kept_length);
    XrLirBlock entry=test_block(m,f,NULL,0),loop=test_block(m,f,carried,2),body=test_block(m,f,carried,2);
    XrLirBlock done=test_block(m,f,carried,2);OK(xr_lir_set_entry(m,f,entry));
    XrLirValue initial[]={test_text_call(m,entry,make),test_constant(m,entry,XR_LIR_I64,0)};
    test_branch(m,entry,loop,initial,2);
    XrLirValue at_loop[2],at_body[2],at_done[2];
    for(unsigned n=0;n<2;++n){at_loop[n]=block_parameter(m,loop,n);at_body[n]=block_parameter(m,body,n);
        at_done[n]=block_parameter(m,done,n);}
    XrLirValue more=test_binary(m,loop,XR_LIR_ICMP,at_loop[1],test_constant(m,loop,XR_LIR_I64,400),XR_LIR_LT);
    OK(xr_lir_set_terminator(m,loop,&(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&more,.operand_count=1,.edges={{body,at_loop,2},{done,at_loop,2}},.edge_count=2}));
    XrLirValue copy={0},copy_length={0};
    OK(xr_lir_emit_value(m,body,&(XrLirInstruction){.op=XR_LIR_STRING_RETAIN,.type=XR_LIR_STRING,
        .operands=&at_body[0],.operand_count=1},&copy));
    OK(xr_lir_emit_value(m,body,&(XrLirInstruction){.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,
        .operands=&copy,.operand_count=1},&copy_length));
    frame_drop(m,body,copy,false);
    XrLirValue borrowed=test_call(m,body,g,at_body[0]);
    XrLirValue step=test_binary(m,body,XR_LIR_ADD,copy_length,borrowed,XR_LIR_PREDICATE_NONE);
    XrLirValue next=test_binary(m,body,XR_LIR_ADD,at_body[1],
        test_binary(m,body,XR_LIR_ADD,step,test_constant(m,body,XR_LIR_I64,90),XR_LIR_PREDICATE_NONE),XR_LIR_PREDICATE_NONE);
    XrLirValue back[]={at_body[0],next};test_branch(m,body,loop,back,2);
    XrLirValue final_length={0};
    OK(xr_lir_emit_value(m,done,&(XrLirInstruction){.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,
        .operands=&at_done[0],.operand_count=1},&final_length));
    frame_drop(m,done,at_done[0],false);
    test_return(m,done,test_binary(m,done,XR_LIR_ADD,at_done[1],final_length,XR_LIR_PREDICATE_NONE));
    OK(xr_lir_module_finish(m,NULL));
    XrRbcProgram *program=compile(m,f);
    const XrRbcModule *module=xr_rbc_program_view(program);
    uint32_t root=UINT32_MAX,formal_function=UINT32_MAX;
    for(uint32_t n=0;n<module->function_count;++n) {
        const XrRbcValueCode *code=xr_rbc_module_value_code(module,n);
        if(!code) continue;
        bool parameter_copy=false;
        for(size_t a=0;a<code->access_count;++a)
            parameter_copy|=code->accesses[a].operation==XR_RBC_VALUE_COPY &&
                code->homes->values[code->accesses[a].source].parameter;
        if(parameter_copy) formal_function=n; else if(owner_words(module,n,XR_RBC_OWNER_EDGE)) root=n;
    }
    CHECK(root!=UINT32_MAX && formal_function!=UINT32_MAX);
    CHECK(owner_words(module,root,XR_RBC_OWNER_COPY)==1 && owner_words(module,root,XR_RBC_OWNER_DROP)==2 &&
        owner_words(module,root,XR_RBC_OWNER_EDGE)>=1);
    CHECK(!owner_words(module,formal_function,XR_RBC_OWNER_COPY) && owner_words(module,formal_function,XR_RBC_OWNER_DROP)==1);
    xr_lir_module_destroy(m);
    // 400 / (5 + 5 + 90) = 4 turns; the final text adds its length once more.
    CHECK(run(program,NULL,0)==405);
    XrRbcExecution *accepted=NULL;
    CHECK(root==0 && xr_rbc_module_prepare(module,0,NULL,0,xr_rbc_program_locations(program),&accepted)==XR_RBC_OK &&
        accepted);
    xr_rbc_execution_free(&accepted);
    const XrRbcFunction *physical=&module->functions[root];
    for(size_t pc=0;pc<physical->code_count;++pc) {
        uint32_t op=physical->code[pc]&255u;
        if(op==XR_RBC_OWNER_DROP||op==XR_RBC_OWNER_COPY||op==XR_RBC_OWNER_EDGE)
            owner_word_reject(program,root,pc,XR_RBC_VALUE_ACCESS);
        if(op==XR_RBC_OWNER_EDGE) owner_word_reject(program,root,pc,XR_RBC_OWNER_DROP);
        if(op==XR_RBC_OWNER_DROP) owner_word_reject(program,root,pc,XR_RBC_OWNER_COPY);
    }
    const XrRbcFunction *formal_code=&module->functions[formal_function];
    const XrRbcValueCode *code=xr_rbc_module_value_code(module,formal_function);
    for(size_t pc=0;pc<formal_code->code_count;++pc) {
        uint32_t word=formal_code->code[pc];
        if((word&255u)==XR_RBC_VALUE_ACCESS && code->accesses[word>>16].operation==XR_RBC_VALUE_COPY)
            owner_word_reject(program,formal_function,pc,XR_RBC_OWNER_COPY);
    }
    CHECK(run(program,NULL,0)==405);
    xr_rbc_program_destroy(program);
}

static void test_backward_owned_dag(void) {
    XrLirModule *m=NULL;OK(xr_lir_module_create(NULL,&m));XrLirType text_type=XR_LIR_STRING;
    XrLirFunction f=test_function(m,"backward_owned_dag",NULL,0,XR_LIR_I64);
    XrLirFunction make=test_text_function(m,"backward_owned_text",12345);
    XrLirBlock entry=test_block(m,f,NULL,0),join=test_block(m,f,&text_type,1),producer=test_block(m,f,NULL,0);
    OK(xr_lir_set_entry(m,f,entry));test_branch(m,entry,producer,NULL,0);
    for(uint32_t n=0;n<260;++n)(void)test_constant(m,producer,XR_LIR_I64,n);
    // A call result keeps a real transfer; a local String producer would stay in one home.
    XrLirValue text=test_text_call(m,producer,make);test_branch(m,producer,join,&text,1);
    XrLirValue value=block_parameter(m,join,0),length;
    XrLirInstruction op={.op=XR_LIR_STRING_LEN,.type=XR_LIR_I64,.operands=&value,.operand_count=1};
    OK(xr_lir_emit_value(m,join,&op,&length));frame_drop(m,join,value,false);test_return(m,join,length);
    OK(xr_lir_module_finish(m,NULL));
    // Physical instruction order can run backward through an acyclic graph.
    XrRbcProgram *program=NULL;XrRbcCompileDiagnostic diagnostic={0};
    CHECK(xr_rbc_compile(m,f,&program,&diagnostic)==XR_RBC_COMPILE_OK);
    CHECK(program && diagnostic.status==XR_RBC_COMPILE_OK);
    const XrRbcModule *physical=xr_rbc_program_view(program);
    const XrLirValueHomes *homes=xr_rbc_module_value_homes(physical,0);
    const XrRbcValueCode *code=xr_rbc_module_value_code(physical,0);
    CHECK(homes && code && homes->values[text.id].owner_count==1 && homes->values[value.id].owner_count==1);
    CHECK(code->locations[text.id].kind==XR_RBC_VALUE_NOWHERE && code->locations[value.id].kind==XR_RBC_VALUE_NOWHERE);
    bool transfer=false;
    for(size_t n=0;n<homes->transfer_count;++n)
        transfer|=homes->transfers[n].source==text.id && homes->transfers[n].target==value.id;
    CHECK(transfer);xr_lir_module_destroy(m);
    CHECK(run(program,NULL,0)==5);
    xr_rbc_program_destroy(program);
}

#include "lir_bytecode_state_cases.h"
#include "lir_nullable_owner_cases.h"

int main(void) {
    test_nullable_outcome_owner();
    test_state_reference_transport();
    test_f64_constant_comparison();
    test_f64_constant_spills_and_phi();
    test_many_spilled_conditions();
    test_scalar_edge_in_typed_function(false);
    test_scalar_edge_in_typed_function(true);
    test_fresh_string_stationary_edges();
    test_mixed_edge_scalar_copies();
    test_owner_words();
    test_backward_owned_dag();
    test_string_binding_aliases();
    test_scalar_spill_branches();
    test_scalar_spills();
    test_sequential_string_owners();
    test_loop_string_temporaries();
    test_string_edge_swap();
    test_literal_ownership();
    test_cell_escape_rejections();
    test_cell_transport();
    test_edge_moves();
    test_owned_arithmetic_panics();
    test_raw_typed_error_rejected();
    test_directory_frame_after_entry_jump();
    test_cleanup_root_authority(false);
    test_cleanup_root_authority(true);
    test_owned_try_assert();
    test_nullable_invoke_result();
    test_unsupported_nullable_invoke_result(false);
    test_unsupported_nullable_invoke_result(true);
    test_owned_array_invoke(false);
    test_owned_array_invoke(true);
    test_owned_string_invoke_drop();
    test_owned_startup_error();
    test_integer_identity_cast(XR_LIR_I64, XR_LIR_I64);
    test_integer_identity_cast(XR_LIR_U64, XR_LIR_I64);
    test_integer_identity_cast(XR_LIR_I64, XR_LIR_U64);
    test_u64_arithmetic_and_edges();
    test_u64_ordered_comparisons();
    test_recursive_and_loop();
    test_immediate_selection();
    test_u8_immediate_selection();
    test_location_ownership();
    test_comparisons();
    test_arithmetic();
    test_constants_and_bool_calls();
    test_parallel_cycle();
    test_same_target_edges();
    test_call_windows();
    test_capacity(64, false);
    test_capacity(65, false);
    test_capacity(255, false);
    test_capacity(256, false);
    test_capacity(300, false);
    test_capacity(254, true);
    test_capacity(300, true);
    test_call_capacity(253);
    test_call_capacity(254);
    test_call_capacity(300);
    test_unused_and_invalid();
    test_owned_diagnostic_and_rejection();
    test_i64_to_f64_admission();
    test_f64_constants_and_refusals();
    puts("lir_bytecode: passed");
    return 0;
}
