/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LIR_CALLABLE_CONSTRUCTION_FIXTURE_H
#define LIR_CALLABLE_CONSTRUCTION_FIXTURE_H

/* Shapes: a nonentry constructor, an unrelated preceding loop, a self-cycle
 * containing the constructor, and a two-block constructor cycle. */
static void callable_construction_fixture(unsigned shape, XrLirModule **output,
    XrLirFunction *root, XrLirValue *reference) {
    XrLirModule *m = NULL; OK(xr_lir_module_create(NULL, &m));
    XrLirType callable = 0, scalar = XR_LIR_I64;
    OK(xr_lir_callable_type(m, &(XrLirCallableType){.result=scalar}, &callable));
    XrLirFunction leaf = {0};
    XrLirFunctionSpec spec = {.name="capture", .name_length=7, .parameters=&scalar,
        .parameter_count=1, .result=scalar};
    OK(xr_lir_declare_function(m, &spec, &leaf)); OK(xr_lir_define_function(m, leaf));
    OK(xr_lir_set_closure_target(m, leaf, (XrLirClosureTarget){.callable=callable, .capture_count=1}));
    XrLirBlock body = {0}; XrLirValue captured = {0};
    OK(xr_lir_add_block(m, leaf, NULL, 0, &body)); OK(xr_lir_set_entry(m, leaf, body));
    OK(xr_lir_function_parameter(m, leaf, 0, &captured));
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&captured, .operand_count=1};
    OK(xr_lir_set_terminator(m, body, &ret));
    spec = (XrLirFunctionSpec){.name="construction", .name_length=12, .result=scalar};
    OK(xr_lir_declare_function(m, &spec, root)); OK(xr_lir_define_function(m, *root));
    XrLirBlock entry = {0}, construct = {0}, loop = {0}, exit = {0};
    OK(xr_lir_add_block(m, *root, NULL, 0, &entry)); OK(xr_lir_set_entry(m, *root, entry));
    OK(xr_lir_add_block(m, *root, NULL, 0, &construct));
    if (shape == 1 || shape == 3) OK(xr_lir_add_block(m, *root, NULL, 0, &loop));
    OK(xr_lir_add_block(m, *root, NULL, 0, &exit));
    XrLirValue seed = {0}, condition = {0};
    XrLirInstruction instruction = {.op=XR_LIR_CONST, .type=scalar, .bits=42};
    OK(xr_lir_emit_value(m, entry, &instruction, &seed));
    instruction = (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_I1};
    OK(xr_lir_emit_value(m, entry, &instruction, &condition));
    XrLirInstruction branch = {.op=XR_LIR_BR, .type=XR_LIR_UNIT, .edge_count=1,
        .edges={{.target=shape == 1 ? loop : construct}}};
    OK(xr_lir_set_terminator(m, entry, &branch));
    if (shape == 1) {
        branch = (XrLirInstruction){.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT, .operands=&condition,
            .operand_count=1, .edge_count=2, .edges={{.target=loop}, {.target=construct}}};
        OK(xr_lir_set_terminator(m, loop, &branch));
    }
    instruction = (XrLirInstruction){.op=XR_LIR_FUNCTION_REF, .type=callable, .callee=leaf,
        .bits=XR_LIR_CLOSURE_FRAME, .operands=&seed, .operand_count=1};
    OK(xr_lir_emit_value(m, construct, &instruction, reference));
    XrLirValue result = {0};
    instruction = (XrLirInstruction){.op=XR_LIR_CALL_INDIRECT, .type=scalar,
        .operands=reference, .operand_count=1};
    OK(xr_lir_emit_value(m, construct, &instruction, &result));
    branch = (XrLirInstruction){.op=XR_LIR_BR, .type=XR_LIR_UNIT, .edge_count=1,
        .edges={{.target=shape == 3 ? loop : exit}}};
    if (shape == 2) branch = (XrLirInstruction){.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT,
        .operands=&condition, .operand_count=1, .edge_count=2,
        .edges={{.target=construct}, {.target=exit}}};
    OK(xr_lir_set_terminator(m, construct, &branch));
    if (shape == 3) {
        branch = (XrLirInstruction){.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT, .operands=&condition,
            .operand_count=1, .edge_count=2, .edges={{.target=construct}, {.target=exit}}};
        OK(xr_lir_set_terminator(m, loop, &branch));
    }
    ret.operands = &result; OK(xr_lir_set_terminator(m, exit, &ret));
    OK(xr_lir_module_finish(m, NULL)); *output = m;
}
#endif // LIR_CALLABLE_CONSTRUCTION_FIXTURE_H
