/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "lir_callable_bytecode_cases.h"

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
    CHECK(probe.retained == 1 && !probe.released);
    xr_rbc_execution_free(&execution);
    CHECK(probe.released == 1);
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
        if (!scenario) reference->bits = XR_LIR_CLOSURE_OWNED;
        else if (scenario == 1) {
            function->result = reference->type;
            XrLirInstructionView *ret = &entry->instructions[entry->instruction_count - 1].view;
            CHECK(ret->op == XR_LIR_RET && ret->operand_count == 1);
            ((uint32_t *)ret->operands)[0] = reference->result;
        } else {
            XrLirFunctionData *helper = &copy->functions[1];
            CHECK(helper->callable_parameter_count == 1 && !helper->closure.callable);
            helper->callable_parameters[0].contract.escaping = true;
            XrLirModule *received = NULL;
            OK(xr_lir_rebuild(copy, &received));
            CHECK(received->functions[1].callable_parameters != helper->callable_parameters);
            xr_lir_module_destroy(copy);
            copy = received;
            CHECK(copy->functions[1].callable_parameters[0].contract.escaping);
        }
        OK(xr_lir_verify(copy, NULL));
        XrRbcProgram *program = NULL;
        CHECK(xr_rbc_compile(copy, (XrLirFunction){copy, root.id}, &program, NULL) == XR_RBC_COMPILE_UNSUPPORTED);
        CHECK(!program);
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
        XrLirCRequest request = {.module=copy, .entry={copy, root.id}, .resources=resources,
            .translation_unit="frame-frontier.c"};
        XrLirCOutput output = {0};
        CHECK(xr_lir_emit_c(&request, &output, NULL) == XR_LIR_C_UNSUPPORTED);
        CHECK(!output.bytes && !output.length);
        xr_compile_resources_release(resources);
        xr_lir_module_destroy(copy);
    }
}

static void callable_vm_execution(void) {
    XrLirModule *captured = NULL;
    XrLirFunction captured_root = {0};
    callable_fixture_capture(&captured, &captured_root, true);
    callable_transforms(captured, captured_root);
    callable_frame_frontiers(captured, captured_root);
    XrRbcProgram *captured_program = NULL;
    CHECK(xr_rbc_compile(captured, captured_root, &captured_program, NULL) == XR_RBC_COMPILE_OK);
    callable_frame_receiving(captured_program);
    xr_rbc_program_destroy(captured_program);
    captured_program = NULL;
    xr_lir_module_destroy(captured);
    callable_fixture_capture(&captured, &captured_root, 2);
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
