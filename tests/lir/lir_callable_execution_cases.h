/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "lir_callable_bytecode_cases.h"

static void callable_c_rejected(const XrLirModule *module, XrLirFunction root, XrLirCStatus expected) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=module, .entry=root, .resources=resources,
        .translation_unit="callable-frontier.c"};
    XrLirCOutput output = {0};
    XrLirCDiagnostic diagnostic = {0};
    XrLirCStatus status = xr_lir_emit_c(&request, &output, &diagnostic);
    if (status != expected || output.bytes || output.length)
        fprintf(stderr, "callable C f%u status=%u: %s\n", root.id, (unsigned)status, diagnostic.message);
    CHECK(status == expected && !output.bytes && !output.length);
    xr_compile_resources_release(resources);
}
static void callable_rbc_unavailable(const XrLirModule *module, XrLirFunction root) {
    OK(xr_lir_verify(module, NULL));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED && !program);
}
static void callable_c_supported(const XrLirModule *module, XrLirFunction root, bool owned) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY,0,false}; XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=module,.entry=root,.resources=resources,.translation_unit="callable-values.c"};
    XrLirCOutput output = {0}; XrLirCDiagnostic diagnostic = {0};
    XrLirCStatus status = xr_lir_emit_c(&request,&output,&diagnostic);
    if (status != XR_LIR_C_OK) fprintf(stderr,"callable C: %s\n",diagnostic.message);
    CHECK(status == XR_LIR_C_OK && output.bytes && output.length);
    CHECK((strstr(output.bytes,"xrt_native_environment_create") != NULL) == owned);
    xr_lir_c_output_free(&output); xr_compile_resources_release(resources);
}
static void callable_c_frame(const XrLirModule *module, XrLirFunction root) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {.module=module, .entry=root, .resources=resources,
        .translation_unit="callable-frame.c"};
    XrLirCOutput output = {0}; XrLirCDiagnostic diagnostic = {0};
    XrLirCStatus status = xr_lir_emit_c(&request,&output,&diagnostic);
    if (status != XR_LIR_C_OK) fprintf(stderr,"Frame C: %s\n",diagnostic.message);
    CHECK(status == XR_LIR_C_OK && output.bytes && output.length);
    CHECK(strstr(output.bytes,"XrtFunction") && strstr(output.bytes,"XrlFnEntry"));
    CHECK(!strstr(output.bytes,"xrt_native_environment_create"));
    xr_lir_c_output_free(&output); xr_compile_resources_release(resources);
}
static void callable_c_borrowed_result_fixture(XrLirModule **output, XrLirFunction *root) {
    CArrayFixture c = {0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirType leaf_type = 0, factory_type = 0;
    OK(xr_lir_callable_type(c.module,&(XrLirCallableType){.result=XR_LIR_I64},&leaf_type));
    XrLirCallableParameter argument = {.type=leaf_type,.escaping=true};
    OK(xr_lir_callable_type(c.module,&(XrLirCallableType){.parameters=&argument,.parameter_count=1,.result=leaf_type},&factory_type));
    XrLirFunction leaf = {0}, identity = {0};
    XrLirFunctionSpec spec = {.name="leaf",.name_length=4,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&leaf));
    OK(xr_lir_set_closure_target(c.module,leaf,(XrLirClosureTarget){.callable=leaf_type}));
    OK(xr_lir_define_function(c.module,leaf)); XrLirBlock block = {0};
    OK(xr_lir_add_block(c.module,leaf,NULL,0,&block)); OK(xr_lir_set_entry(c.module,leaf,block));
    ca_return(&c,block,ca_constant(&c,block,42));
    XrLirFunctionCallableParameter contract = {.contract=argument};
    spec = (XrLirFunctionSpec){.name="identity",.name_length=8,.parameters=&leaf_type,.parameter_count=1,.result=leaf_type,
        .callable_parameters=&contract,.callable_parameter_count=1};
    OK(xr_lir_declare_function(c.module,&spec,&identity));
    OK(xr_lir_set_closure_target(c.module,identity,(XrLirClosureTarget){.callable=factory_type}));
    OK(xr_lir_define_function(c.module,identity));
    OK(xr_lir_add_block(c.module,identity,NULL,0,&block)); OK(xr_lir_set_entry(c.module,identity,block));
    XrLirValue borrowed = {0}; OK(xr_lir_function_parameter(c.module,identity,0,&borrowed));
    XrLirValue scope = callable_fixture_scope(&c,block,(XrLirValue){0}), operands[] = {scope,borrowed};
    XrLirValue bound = ca_value(&c,block,XR_LIR_FUNCTION_BIND,leaf_type,operands,2);
    XrLirValue kept = callable_fixture_leave(&c,block,scope,bound,leaf_type); ca_return(&c,block,kept);
    spec = (XrLirFunctionSpec){.name="root",.name_length=4,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,root)); OK(xr_lir_define_function(c.module,*root));
    OK(xr_lir_add_block(c.module,*root,NULL,0,&block)); OK(xr_lir_set_entry(c.module,*root,block));
    XrLirValue first = {0}, factory = {0};
    XrLirInstruction reference = {.op=XR_LIR_FUNCTION_REF,.type=leaf_type,.callee=leaf};
    OK(xr_lir_emit_value(c.module,block,&reference,&first));
    reference.type=factory_type; reference.callee=identity; OK(xr_lir_emit_value(c.module,block,&reference,&factory));
    XrLirValue arguments[] = {factory,first};
    XrLirValue returned = ca_value(&c,block,XR_LIR_CALL_INDIRECT,leaf_type,arguments,2);
    XrLirValue copied_answer = ca_value(&c,block,XR_LIR_CALL_INDIRECT,XR_LIR_I64,&returned,1);
    XrLirValue original_answer = ca_value(&c,block,XR_LIR_CALL_INDIRECT,XR_LIR_I64,&first,1);
    ca_return(&c,block,ca_binary(&c,block,XR_LIR_ADD,copied_answer,original_answer));
    OK(xr_lir_module_finish(c.module,NULL)); *output=c.module;
}
/* Reusing one constructor's stack cell would change the previous iteration's
 * immutable capture before it is invoked. Keep this valid LIR rejected until
 * independent snapshots exist, rather than emitting a program that returns 2
 * instead of 1. The entry constructor itself is outside the cycle. */
static void callable_reentry_snapshot_frontier(void) {
    CArrayFixture c = {0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirType callable = 0;
    XrLirCallableType signature = {.result=XR_LIR_I64};
    OK(xr_lir_callable_type(c.module,&signature,&callable));
    XrLirFunction leaf = {0}, root = {0}; XrLirType scalar = XR_LIR_I64;
    XrLirFunctionSpec spec = {.name="capture",.name_length=7,.parameters=&scalar,.parameter_count=1,.result=scalar};
    OK(xr_lir_declare_function(c.module,&spec,&leaf));
    OK(xr_lir_set_closure_target(c.module,leaf,(XrLirClosureTarget){.callable=callable,.capture_count=1}));
    OK(xr_lir_define_function(c.module,leaf)); XrLirBlock body = {0};
    OK(xr_lir_add_block(c.module,leaf,NULL,0,&body)); OK(xr_lir_set_entry(c.module,leaf,body));
    XrLirValue captured = {0}; OK(xr_lir_function_parameter(c.module,leaf,0,&captured)); ca_return(&c,body,captured);
    spec = (XrLirFunctionSpec){.name="root",.name_length=4,.result=scalar};
    OK(xr_lir_declare_function(c.module,&spec,&root)); OK(xr_lir_define_function(c.module,root));
    XrLirBlock entry = {0}, loop = {0}, done = {0}; XrLirType types[] = {scalar,callable};
    OK(xr_lir_add_block(c.module,root,NULL,0,&entry)); OK(xr_lir_add_block(c.module,root,types,2,&loop));
    OK(xr_lir_add_block(c.module,root,&scalar,1,&done)); OK(xr_lir_set_entry(c.module,root,entry));
    XrLirValue zero = ca_constant(&c,entry,0), initial = {0};
    XrLirInstruction reference = {.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=leaf,.operands=&zero,.operand_count=1};
    OK(xr_lir_emit_value(c.module,entry,&reference,&initial));
    XrLirValue first[] = {zero,initial};
    XrLirInstruction branch = {.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,.edges={{loop,first,2}}};
    OK(xr_lir_set_terminator(c.module,entry,&branch));
    XrLirValue index = {0}, previous = {0};
    OK(xr_lir_block_parameter(c.module,loop,0,&index)); OK(xr_lir_block_parameter(c.module,loop,1,&previous));
    XrLirValue one = ca_constant(&c,loop,1), next = ca_binary(&c,loop,XR_LIR_ADD,index,one), value = {0};
    reference.operands = &next; OK(xr_lir_emit_value(c.module,loop,&reference,&value));
    XrLirValue observed = ca_value(&c,loop,XR_LIR_CALL_INDIRECT,scalar,&previous,1), condition = {0};
    XrLirValue compare[] = {index,one};
    XrLirInstruction less = {.op=XR_LIR_ICMP,.type=XR_LIR_I1,.predicate=XR_LIR_LT,.operands=compare,.operand_count=2};
    OK(xr_lir_emit_value(c.module,loop,&less,&condition));
    XrLirValue again[] = {next,value};
    branch = (XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
        .edge_count=2,.edges={{loop,again,2},{done,&observed,1}}};
    OK(xr_lir_set_terminator(c.module,loop,&branch));
    OK(xr_lir_block_parameter(c.module,done,0,&observed)); ca_return(&c,done,observed);
    OK(xr_lir_module_finish(c.module,NULL));
    callable_c_rejected(c.module,root,XR_LIR_C_UNSUPPORTED);
    xr_lir_module_destroy(c.module);
}
static void callable_entry_signature_frontiers(void) {
    for (unsigned shape = 0; shape < 5; ++shape) {
        CArrayFixture c = {0}; OK(xr_lir_module_create(NULL,&c.module));
        XrLirType callable = 0, child = 0;
        XrLirCallableParameter parameter = {0};
        XrLirCallableType signature = {.result=XR_LIR_STRING};
        if (shape == 1) {
            XrLirCallableType leaf = {.result=XR_LIR_F32};
            OK(xr_lir_callable_type(c.module,&leaf,&child));
            parameter.type = child;
            signature = (XrLirCallableType){.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
        } else if (shape >= 2) {
            child = shape == 2 ? XR_LIR_STRING : shape == 3 ? XR_LIR_ARRAY : XR_LIR_MAP_STRING_I64;
            parameter = (XrLirCallableParameter){.type=child,.mode=XR_PARAM_MOVE};
            if (shape == 3) parameter.array_element = XR_LIR_I64;
            signature = (XrLirCallableType){.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
        }
        OK(xr_lir_callable_type(c.module,&signature,&callable));
        XrLirFunction target = {0}, root = {0};
        XrLirFunctionSpec spec = {.name="target",.name_length=6,.parameters=shape ? &child : NULL,
            .parameter_count=shape ? 1u : 0u,.result=signature.result};
        OK(xr_lir_declare_function(c.module,&spec,&target));
        if (shape == 3) {
            XrLirValue argument = {0}; XrLirArrayShape element = {XR_LIR_I64};
            OK(xr_lir_function_parameter(c.module,target,0,&argument));
            OK(xr_lir_set_array_shape(c.module,argument,&element));
        }
        OK(xr_lir_set_closure_target(c.module,target,(XrLirClosureTarget){.callable=callable}));
        OK(xr_lir_define_function(c.module,target)); XrLirBlock block = {0};
        OK(xr_lir_add_block(c.module,target,NULL,0,&block)); OK(xr_lir_set_entry(c.module,target,block));
        ca_return(&c,block,shape ? ca_constant(&c,block,42) : ca_text(&c,block,"x",1));
        spec = (XrLirFunctionSpec){.name="root",.name_length=4,.result=XR_LIR_I64};
        OK(xr_lir_declare_function(c.module,&spec,&root)); OK(xr_lir_define_function(c.module,root));
        OK(xr_lir_add_block(c.module,root,NULL,0,&block)); OK(xr_lir_set_entry(c.module,root,block));
        XrLirInstruction reference = {.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=target};
        XrLirValue unused = {0}; OK(xr_lir_emit_value(c.module,block,&reference,&unused));
        ca_return(&c,block,ca_constant(&c,block,42)); OK(xr_lir_module_finish(c.module,NULL));
        if (!shape) callable_c_supported(c.module,root,false);
        else callable_c_rejected(c.module,root,XR_LIR_C_UNSUPPORTED);
        xr_lir_module_destroy(c.module);
    }
}
static void callable_direct_weak_fixture(XrLirModule **output, XrLirFunction *root) {
    CArrayFixture c = {0}; OK(xr_lir_module_create(NULL,&c.module));
    XrLirCallableParameter parameter = {.type=XR_LIR_I64};
    XrLirCallableType signature = {.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64,
        .flags=XR_LIR_CALLABLE_NO_SUSPEND};
    XrLirType strong = 0, weak = 0;
    OK(xr_lir_callable_type(c.module,&signature,&strong));
    signature.flags = 0; OK(xr_lir_callable_type(c.module,&signature,&weak));
    XrLirType scalar = XR_LIR_I64; XrLirFunction target = {0};
    XrLirFunctionSpec spec = {.name="target",.name_length=6,.parameters=&scalar,.parameter_count=1,.result=scalar};
    OK(xr_lir_declare_function(c.module,&spec,&target));
    OK(xr_lir_set_closure_target(c.module,target,(XrLirClosureTarget){.callable=strong}));
    OK(xr_lir_define_function(c.module,target)); XrLirBlock block = {0};
    OK(xr_lir_add_block(c.module,target,NULL,0,&block)); OK(xr_lir_set_entry(c.module,target,block));
    XrLirValue input = {0}; OK(xr_lir_function_parameter(c.module,target,0,&input));
    ca_return(&c,block,ca_binary(&c,block,XR_LIR_ADD,input,ca_constant(&c,block,1)));
    spec = (XrLirFunctionSpec){.name="root",.name_length=4,.result=scalar};
    OK(xr_lir_declare_function(c.module,&spec,root)); OK(xr_lir_define_function(c.module,*root));
    OK(xr_lir_add_block(c.module,*root,NULL,0,&block)); OK(xr_lir_set_entry(c.module,*root,block));
    XrLirInstruction reference = {.op=XR_LIR_FUNCTION_REF,.type=weak,.callee=target};
    XrLirValue function = {0}; OK(xr_lir_emit_value(c.module,block,&reference,&function));
    XrLirValue arguments[] = {function,ca_constant(&c,block,41)};
    ca_return(&c,block,ca_value(&c,block,XR_LIR_CALL_INDIRECT,scalar,arguments,2));
    OK(xr_lir_module_finish(c.module,NULL)); *output = c.module;
}

typedef struct CallableCodeLeaseProbe { uint32_t retained, released; bool reject; } CallableCodeLeaseProbe;

static bool callable_code_lease_retain(void *owner) {
    CallableCodeLeaseProbe *probe = owner;
    if (probe->reject) return false;
    ++probe->retained;
    return true;
}

static void callable_code_lease_release(void *owner) {
    ++((CallableCodeLeaseProbe *)owner)->released;
}

static void callable_code_lease_cases(const XrRbcProgram *program) {
    XrRbcModule module = *xr_rbc_program_view(program);
    CallableCodeLeaseProbe probe = {0};
    XrRbcCodeLease lease = {&probe, callable_code_lease_retain, callable_code_lease_release};
    XrtLocationOwner *locations = xr_rbc_program_locations(program);
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
        module.code_lease = lease;
        if (invalid == 0) module.code_lease.owner = NULL;
        if (invalid == 1) module.code_lease.retain = NULL;
        if (invalid == 2) module.code_lease.release = NULL;
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_INVALID);
        CHECK(!execution && !probe.retained && !probe.released);
    }
    module.code_lease = lease;
    probe.reject = true;
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_VALUE_FAILURE);
    CHECK(!execution && !probe.retained && !probe.released);
    probe.reject = false;
    CHECK(xr_rbc_module_prepare(&module, 0, NULL, 0, locations, &execution) == XR_RBC_OK);
    uint32_t leases = 1;
    CHECK(probe.retained == leases && !probe.released);
    xr_rbc_execution_free(&execution);
    CHECK(probe.released == leases);
}

static void callable_transforms(XrLirModule *module, XrLirFunction root) {
    XrLirModule *rebuilt = NULL, *inlined = NULL, *folded = NULL;
    OK(xr_lir_rebuild(module, &rebuilt));
    OK(xr_lir_inline(rebuilt, &inlined));
    OK(xr_lir_sccp(inlined, &folded));
    xr_lir_module_destroy(rebuilt);
    xr_lir_module_destroy(inlined);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(folded, (XrLirFunction){folded, root.id}, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(folded);
    callable_code_lease_cases(program);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    xr_rbc_program_destroy(program);
    program = NULL;
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
    xr_rbc_outcome_free(&outcome);
}

static void callable_frame_frontiers(const XrLirModule *module, XrLirFunction root) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        XrLirModule *copy = NULL;
        OK(xr_lir_rebuild(module, &copy));
        XrLirFunctionData *function = &copy->functions[root.id - 1];
        XrLirBlockData *entry = &function->blocks[function->entry - 1];
        XrLirInstructionView *reference = NULL;
        for (uint32_t n = 0; n < entry->instruction_count; ++n)
            if (entry->instructions[n].view.op == XR_LIR_FUNCTION_REF) reference = &entry->instructions[n].view;
        CHECK(reference);
        if (!scenario) {
            reference->bits = XR_LIR_CLOSURE_OWNED;
            XrLirModule *received = NULL; OK(xr_lir_rebuild(copy,&received));
            xr_lir_module_destroy(copy); copy = received;
        }
        else if (scenario == 1) {
            function->result = reference->type;
            XrLirInstructionView *ret = &entry->instructions[entry->instruction_count - 1].view;
            CHECK(ret->op == XR_LIR_RET && ret->operand_count == 1);
            ((uint32_t *)ret->operands)[0] = reference->result;
        } else {
            XrLirFunctionData *helper = &copy->functions[1];
            CHECK(helper->callable_parameter_count == 1 && !helper->closure.callable);
            helper->callable_parameters[0].contract.escaping = true;
            CHECK(xr_lir_verify(copy, NULL) != XR_LIR_OK);
            reference->bits = XR_LIR_CLOSURE_OWNED;
            XrLirModule *received = NULL;
            OK(xr_lir_rebuild(copy, &received));
            CHECK(received->functions[1].callable_parameters != helper->callable_parameters);
            xr_lir_module_destroy(copy);
            copy = received;
            CHECK(copy->functions[1].callable_parameters[0].contract.escaping);
        }
        callable_rbc_unavailable(copy, (XrLirFunction){copy, root.id});
        if (scenario == 1) callable_c_rejected(copy,(XrLirFunction){copy,root.id},XR_LIR_C_UNSUPPORTED);
        else callable_c_supported(copy,(XrLirFunction){copy,root.id},true);
        xr_lir_module_destroy(copy);
    }
}

static void callable_vm_execution(void) {
    callable_reentry_snapshot_frontier();
    callable_entry_signature_frontiers();
    XrLirModule *captured = NULL;
    XrLirFunction captured_root = {0};
    callable_direct_weak_fixture(&captured,&captured_root);
    callable_c_frame(captured,captured_root); callable_transforms(captured,captured_root);
    xr_lir_module_destroy(captured); captured = NULL;
    callable_fixture_capture(&captured, &captured_root, true);
    callable_c_frame(captured,captured_root);
    callable_transforms(captured, captured_root);
    callable_frame_frontiers(captured, captured_root);
    XrRbcProgram *captured_program = NULL;
    CHECK(xr_rbc_compile(captured, captured_root, &captured_program, NULL) == XR_RBC_COMPILE_OK);
    callable_frame_receiving(captured_program);
    xr_rbc_program_destroy(captured_program);
    captured_program = NULL;
    xr_lir_module_destroy(captured);
    callable_fixture_capture(&captured, &captured_root, 2);
    callable_c_frame(captured, captured_root);
    callable_transforms(captured, captured_root);
    CHECK(xr_rbc_compile(captured, captured_root, &captured_program, NULL) == XR_RBC_COMPILE_OK);
    callable_frame_receiving(captured_program);
    xr_rbc_program_destroy(captured_program);
    xr_lir_module_destroy(captured);
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_fixture(&module, &root);
    callable_transforms(module, root);
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    XrRbcCompileStatus status = xr_rbc_compile(module, root, &program, &diagnostic);
    if (status != XR_RBC_COMPILE_OK)
        fprintf(stderr, "Callable compile=%u %s\n", (unsigned)status, diagnostic.message);
    CHECK(status == XR_RBC_COMPILE_OK && program);
    callable_raw_receiving(program);
    callable_narrow_argument(program);
    XrLirFunction scalar = {0};
    OK(xr_lir_function_at(module, 0, &scalar));
    XrRbcProgram *direct = NULL;
    CHECK(xr_rbc_compile(module, scalar, &direct, &diagnostic) == XR_RBC_COMPILE_OK && direct);
    xr_lir_module_destroy(module);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(program);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
    xr_rbc_outcome_free(&outcome);
    uint64_t argument = 41;
    CHECK(xr_rbc_program_prepare(direct, &argument, 1, &execution) == XR_RBC_OK);
    outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    xr_rbc_program_destroy(direct);
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome, &bits) && bits == 42);
    xr_rbc_outcome_free(&outcome);
}
