/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_callable_bytecode.c - Pure code tokens and borrowed stack captures
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_internal.h"
#include "lir/xlir_verify.h"
#include "vm/register/xrbc_lir.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

typedef struct CArrayFixture { XrLirModule *module; } CArrayFixture;
static XrLirValue ca_value(CArrayFixture *c, XrLirBlock block, XrLirOp op,
    XrLirType type, const XrLirValue *arguments, uint32_t count) {
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op=op, .type=type, .operands=arguments, .operand_count=count};
    OK(xr_lir_emit_value(c->module, block, &instruction, &result));
    return result;
}
static XrLirValue ca_constant(CArrayFixture *c, XrLirBlock block, int64_t bits) {
    XrLirValue result = {0};
    XrLirInstruction instruction = {.op=XR_LIR_CONST, .type=XR_LIR_I64, .bits=(uint64_t)bits};
    OK(xr_lir_emit_value(c->module, block, &instruction, &result));
    return result;
}
static XrLirValue ca_binary(CArrayFixture *c, XrLirBlock block, XrLirOp op,
    XrLirValue left, XrLirValue right) {
    XrLirValue arguments[] = {left,right};
    return ca_value(c,block,op,XR_LIR_I64,arguments,2);
}
static void ca_return(CArrayFixture *c, XrLirBlock block, XrLirValue value) {
    XrLirInstruction instruction = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&value, .operand_count=1};
    OK(xr_lir_set_terminator(c->module,block,&instruction));
}
#include "lir_callable_fixture.h"
#include "lir_callable_bytecode_cases.h"

static XrLirInstructionView *reference(XrLirModule *module, XrLirFunction root) {
    XrLirFunctionData *function = &module->functions[root.id - 1];
    for (uint32_t b = 0; b < function->block_count; ++b)
        for (uint32_t n = 0; n < function->blocks[b].instruction_count; ++n) {
            XrLirInstructionView *instruction = &function->blocks[b].instructions[n].view;
            if (instruction->op == XR_LIR_FUNCTION_REF) return instruction;
        }
    CHECK(false);
    return NULL;
}
static XrRbcProgram *compile(XrLirModule *module, XrLirFunction root) {
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    XrRbcCompileStatus status = xr_rbc_compile(module,root,&program,&diagnostic);
    if (status != XR_RBC_COMPILE_OK) fprintf(stderr,"compile %u: %s\n",(unsigned)status,diagnostic.message);
    CHECK(status == XR_RBC_COMPILE_OK && program);
    return program;
}
static void execute(XrRbcProgram *program, uint64_t expected) {
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution) == XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    uint64_t bits = 0;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome,&bits) && bits == expected);
    xr_rbc_outcome_free(&outcome);
}
static void pure_captures(void) {
    for (unsigned capture = 0; capture < 3; ++capture) {
        XrLirModule *module = NULL;
        XrLirFunction root = {0};
        if (capture) callable_fixture_capture(&module,&root,capture);
        else callable_fixture(&module,&root);
        XrRbcProgram *program = compile(module,root);
        xr_lir_module_destroy(module);
        if (capture) callable_frame_receiving(program);
        else {
            callable_raw_receiving(program);
            callable_narrow_argument(program);
        }
        execute(program,42);
    }
}
static void code_only_owned(void) {
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_fixture(&module,&root);
    reference(module,root)->bits = XR_LIR_CLOSURE_OWNED;
    OK(xr_lir_verify(module,NULL));
    XrRbcProgram *program = compile(module,root);
    const XrRbcModule *view = xr_rbc_program_view(program);
    const XrRbcFunction *function = &view->functions[0];
    bool found = false;
    for (size_t n = 0; n < function->aggregate_access_count; ++n) {
        XrRbcAggregateAccess access = function->aggregate_accesses[n];
        if (access.operation != XR_RBC_FUNCTION_REFERENCE) continue;
        CHECK(!access.destination && !access.count && access.slot == 1 && access.callee < view->function_count);
        CHECK(!view->functions[access.callee].capture_count && !view->functions[access.callee].state_capture);
        found = true;
    }
    CHECK(found && !function->environment_count && !function->environment_size);
    xr_lir_module_destroy(module);
    callable_raw_receiving(program);
    execute(program,42);
}
static void unsupported_owners(void) {
    for (unsigned capture = 3; capture <= 4; ++capture) {
        XrLirModule *module = NULL;
        XrLirFunction root = {0};
        callable_fixture_capture(&module,&root,capture);
        XrRbcProgram *program = NULL;
        XrRbcCompileDiagnostic diagnostic = {0};
        CHECK(xr_rbc_compile(module,root,&program,&diagnostic) == XR_RBC_COMPILE_UNSUPPORTED);
        CHECK(!program && diagnostic.message[0]);
        xr_lir_module_destroy(module);
    }
    XrLirModule *module = NULL;
    XrLirFunction root = {0};
    callable_fixture(&module,&root);
    XrLirInstructionView *value = reference(module,root);
    value->bits = XR_LIR_CLOSURE_OWNED;
    XrLirFunctionData *function = &module->functions[root.id - 1];
    function->result = value->type;
    XrLirBlockData *entry = &function->blocks[function->entry - 1];
    XrLirInstructionView *ret = &entry->instructions[entry->instruction_count - 1].view;
    CHECK(ret->op == XR_LIR_RET);
    ((uint32_t *)ret->operands)[0] = value->result;
    OK(xr_lir_verify(module,NULL));
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(module,root,&program,NULL) == XR_RBC_COMPILE_UNSUPPORTED && !program);
    xr_lir_module_destroy(module);
}

static XrLirValue scope_enter(CArrayFixture *c, XrLirBlock block, XrLirValue parent) {
    return ca_value(c,block,XR_LIR_FUNCTION_SCOPE_ENTER,XR_LIR_FUNCTION_SCOPE,
        parent.id ? &parent : NULL,parent.id ? 1u : 0u);
}
static void scope_leave(CArrayFixture *c, XrLirBlock block, XrLirValue scope) {
    XrLirInstruction instruction = {.op=XR_LIR_FUNCTION_SCOPE_LEAVE,.type=XR_LIR_UNIT,
        .operands=&scope,.operand_count=1};
    OK(xr_lir_emit_effect(c->module,block,&instruction));
}
static XrRbcProgram *scoped_code(void) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL,&c.module));
    XrLirCallableParameter parameter = {.type=XR_LIR_I64};
    XrLirCallableType signature = {.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    XrLirType callable = 0, integer = XR_LIR_I64;
    OK(xr_lir_callable_type(c.module,&signature,&callable));
    XrLirFunction target = {0}, root = {0};
    XrLirFunctionSpec spec = {.name="plus_one",.name_length=8,.parameters=&integer,
        .parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&target));
    OK(xr_lir_set_closure_target(c.module,target,(XrLirClosureTarget){.callable=callable}));
    OK(xr_lir_define_function(c.module,target));
    XrLirBlock block = {0};
    OK(xr_lir_add_block(c.module,target,NULL,0,&block));
    OK(xr_lir_set_entry(c.module,target,block));
    XrLirValue input = {0};
    OK(xr_lir_function_parameter(c.module,target,0,&input));
    ca_return(&c,block,ca_binary(&c,block,XR_LIR_ADD,input,ca_constant(&c,block,1)));
    spec = (XrLirFunctionSpec){.name="root",.name_length=4,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&root));
    OK(xr_lir_define_function(c.module,root));
    XrLirBlock entry = {0}, left = {0}, right = {0}, join = {0};
    OK(xr_lir_add_block(c.module,root,NULL,0,&entry));
    OK(xr_lir_add_block(c.module,root,NULL,0,&left));
    OK(xr_lir_add_block(c.module,root,NULL,0,&right));
    OK(xr_lir_add_block(c.module,root,NULL,0,&join));
    OK(xr_lir_set_entry(c.module,root,entry));
    XrLirValue outer = scope_enter(&c,entry,(XrLirValue){0});
    XrLirValue inner = scope_enter(&c,entry,outer), code = {0};
    XrLirInstruction instruction = {.op=XR_LIR_FUNCTION_REF,.type=callable,.callee=target,
        .bits=XR_LIR_CLOSURE_OWNED};
    OK(xr_lir_emit_value(c.module,entry,&instruction,&code));
    XrLirValue bind[] = {outer,code};
    code = ca_value(&c,entry,XR_LIR_FUNCTION_BIND,callable,bind,2);
    XrLirValue condition = {0};
    instruction = (XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1};
    OK(xr_lir_emit_value(c.module,entry,&instruction,&condition));
    instruction = (XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,
        .operand_count=1,.edges={{left,NULL,0},{right,NULL,0}},.edge_count=2};
    OK(xr_lir_set_terminator(c.module,entry,&instruction));
    XrLirBlock branches[] = {left,right};
    for (unsigned n = 0; n < 2; ++n) {
        scope_leave(&c,branches[n],inner);
        instruction = (XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
            .edges={{join,NULL,0}},.edge_count=1};
        OK(xr_lir_set_terminator(c.module,branches[n],&instruction));
    }
    XrLirValue arguments[] = {code,ca_constant(&c,join,41)};
    XrLirValue result = ca_value(&c,join,XR_LIR_CALL_INDIRECT,XR_LIR_I64,arguments,2);
    scope_leave(&c,join,outer);
    ca_return(&c,join,result);
    OK(xr_lir_module_finish(c.module,NULL));
    XrRbcProgram *program = compile(c.module,root);
    xr_lir_module_destroy(c.module);
    return program;
}
static void scope_receiving(void) {
    XrRbcProgram *program = scoped_code();
    const XrRbcModule *original = xr_rbc_program_view(program);
    CHECK(original->function_count == 2 && original->function_scope_count == 2);
    const XrRbcFunction *root = &original->functions[0];
    CHECK(original->function_scopes[0].count == 2);
    XrRbcAggregateAccess *accesses = xr_malloc(root->aggregate_access_count * sizeof(*accesses));
    uint32_t *code = xr_malloc(root->code_count * sizeof(*code));
    CHECK(accesses && code);
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        XrRbcModule module = *original;
        XrRbcFunction functions[2];
        XrRbcFunctionScopes scopes[2];
        uint32_t parents[2];
        memcpy(functions,original->functions,sizeof(functions));
        memcpy(scopes,original->function_scopes,sizeof(scopes));
        memcpy(parents,scopes[0].parents,sizeof(parents));
        memcpy(accesses,root->aggregate_accesses,root->aggregate_access_count * sizeof(*accesses));
        memcpy(code,root->code,root->code_count * sizeof(*code));
        module.functions = functions;
        module.function_scopes = scopes;
        scopes[0].parents = parents;
        functions[0].aggregate_accesses = accesses;
        functions[0].code = code;
        if (mutation == 0) module.function_scopes = NULL;
        if (mutation == 1) --module.function_scope_count;
        if (mutation == 2) scopes[0].parents = NULL;
        if (mutation == 3) parents[0] = 1;
        if (mutation == 4) parents[1] = 3;
        bool changed = mutation < 5;
        for (size_t n = 0; n < root->aggregate_access_count; ++n) {
            if (mutation == 5 && accesses[n].operation == XR_RBC_FUNCTION_BIND) {
                accesses[n].destination = 2;
                changed = true;
            }
            if (mutation == 6 && accesses[n].operation == XR_RBC_FUNCTION_SCOPE_ENTER && accesses[n].destination == 2) {
                accesses[n].source = 0;
                changed = true;
            }
        }
        if (mutation >= 7)
            for (size_t pc = 0; pc + 1 < root->code_count; ++pc) {
                if ((code[pc] & 255) != XR_RBC_AGGREGATE_ACCESS) continue;
                XrRbcAggregateAccess access = accesses[code[pc] >> 16];
                if (access.operation != XR_RBC_FUNCTION_SCOPE_LEAVE ||
                    access.destination != (mutation == 7 ? 2u : 1u)) continue;
                CHECK(xr_rbc_jump(&code[pc],pc,pc + 1,root->code_count));
                changed = true;
                break;
            }
        CHECK(changed);
        XrRbcExecution *execution = NULL;
        CHECK(xr_rbc_module_prepare(&module,0,NULL,0,xr_rbc_program_locations(program),&execution) == XR_RBC_INVALID);
        CHECK(!execution);
    }
    xr_free(code);
    xr_free(accesses);
    execute(program,42);
}

int main(void) {
    pure_captures();
    code_only_owned();
    unsupported_owners();
    scope_receiving();
    puts("pure callable bytecode: stack captures, code-only owners, scope CFG and raw receiving PASS");
    return 0;
}
