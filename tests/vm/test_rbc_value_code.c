/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_rbc_value_code.c - Typed receiving descriptions and their actual code lease
 */
#include "lir/xlir_builder.h"
#include "vm/register/xrbc_lir.h"
#include "vm/register/xrbc_value_code.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); abort(); } } while (0)

static size_t attempts, failure = SIZE_MAX, live;
static struct { void *pointer; size_t bytes; } allocations[4096];
static size_t allocation_index(void *pointer) {
    for (size_t n = 0; n < 4096; ++n) if (allocations[n].pointer == pointer) return n;
    CHECK(false); return 0;
}
static void *allocate(size_t bytes) {
    if (++attempts == failure) return NULL;
    void *memory = xr_malloc(bytes);
    if (memory) {
        size_t n = allocation_index(NULL); allocations[n].pointer = memory; allocations[n].bytes = bytes; ++live;
    }
    return memory;
}
static void *allocate_zero(size_t count, size_t bytes) {
    CHECK(!bytes || count <= SIZE_MAX / bytes);
    void *memory = allocate(count * bytes); if (memory) memset(memory,0,count * bytes); return memory;
}
static void release(void *memory) {
    if (!memory) return;
    size_t n = allocation_index(memory); allocations[n].pointer = NULL; CHECK(live); --live; xr_free(memory);
}
static void *resize(void *old, size_t bytes) {
    if (++attempts == failure) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    size_t n = allocation_index(old);
    if (old) { memcpy(memory,old,bytes < allocations[n].bytes ? bytes : allocations[n].bytes); xr_free(old); }
    else ++live;
    allocations[n].pointer = memory; allocations[n].bytes = bytes; return memory;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) allocate(bytes)
#define xr_calloc(count, bytes) allocate_zero(count, bytes)
#define xr_realloc(memory, bytes) resize(memory, bytes)
#define xr_free(memory) release(memory)
#include "vm/register/xrbc_lir.c"
#include "../lir/lir_c_managed_state_fixture.h"

static XrRbcProgram *compile(XrLirModule *module, XrLirFunction root) {
    XrRbcProgram *program = NULL; XrRbcCompileDiagnostic diagnostic = {0};
    XrRbcCompileStatus status = xr_rbc_compile(module,root,&program,&diagnostic);
    if (status != XR_RBC_COMPILE_OK) fprintf(stderr,"compile f%u b%u i%u: %s\n",
        diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.message);
    CHECK(status == XR_RBC_COMPILE_OK && program); return program;
}
static void run_after_input_retirement(XrRbcProgram *program, uint64_t expected) {
    XrRbcModule snapshot = *xr_rbc_program_view(program);
    const XrRbcValueCode *description = xr_rbc_module_value_code(&snapshot,0);
    XrRbcExecution *execution = NULL;
    CHECK(snapshot.code_lease.retain(snapshot.code_lease.owner));
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution) == XR_RBC_OK);
    xr_rbc_program_destroy(program);
    CHECK(xr_rbc_module_value_code(&snapshot,0) == description);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution); uint64_t bits = 0;
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK && xr_rbc_outcome_bits(outcome,&bits) && bits == expected);
    xr_rbc_execution_free(&execution); xr_rbc_outcome_free(&outcome);
    CHECK(xr_rbc_module_value_code(&snapshot,0) == description);
    snapshot.code_lease.release(snapshot.code_lease.owner); CHECK(!live);
}
static XrLirModule *string_call(XrLirFunction *root) {
    XrLirModule *module = NULL; CHECK(xr_lir_module_create(NULL,&module) == XR_LIR_OK);
    XrLirType string = XR_LIR_STRING; XrLirFunction echo = {0};
    XrLirFunctionSpec spec = {.name="echo",.name_length=4,.parameters=&string,.parameter_count=1,.result=string};
    CHECK(xr_lir_declare_function(module,&spec,&echo) == XR_LIR_OK && xr_lir_define_function(module,echo) == XR_LIR_OK);
    XrLirBlock block = {0}; CHECK(xr_lir_add_block(module,echo,NULL,0,&block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module,echo,block) == XR_LIR_OK);
    XrLirValue parameter = {0}; CHECK(xr_lir_function_parameter(module,echo,0,&parameter) == XR_LIR_OK);
    XrLirValue copied = managed_state_value(module,block,XR_LIR_STRING_RETAIN,string,&parameter,1,0);
    CHECK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&copied,.operand_count=1}) == XR_LIR_OK);
    spec = (XrLirFunctionSpec){.name="root",.name_length=4,.result=XR_LIR_I64};
    CHECK(xr_lir_declare_function(module,&spec,root) == XR_LIR_OK && xr_lir_define_function(module,*root) == XR_LIR_OK);
    block = (XrLirBlock){0}; CHECK(xr_lir_add_block(module,*root,NULL,0,&block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module,*root,block) == XR_LIR_OK);
    XrLirLiteral literal = {0}; CHECK(xr_lir_add_literal(module,(const uint8_t *)"ok",2,&literal) == XR_LIR_OK);
    XrLirValue text = managed_state_value(module,block,XR_LIR_STRING_LITERAL,string,NULL,0,literal.id), result = {0};
    CHECK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL,.type=string,.callee=echo,
        .operands=&text,.operand_count=1},&result) == XR_LIR_OK);
    XrLirValue joined[] = {result,text};
    XrLirValue twice = managed_state_value(module,block,XR_LIR_STRING_CONCAT,string,joined,2,0);
    XrLirValue length = managed_state_value(module,block,XR_LIR_STRING_LEN,XR_LIR_I64,&twice,1,0);
    XrLirValue owners[] = {text,result,twice};
    for (unsigned n = 0; n < 3; ++n) managed_state_effect(module,block,XR_LIR_STRING_DROP,&owners[n],1,0);
    CHECK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&length,.operand_count=1}) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module,NULL) == XR_LIR_OK); return module;
}
static void check_receiving(const XrRbcProgram *program) {
    const XrRbcModule *module = xr_rbc_program_view(program);
    CHECK(!xr_rbc_module_value_code(NULL,0) && !xr_rbc_module_value_code(module,UINT32_MAX));
    XrRbcModule raw = *module; raw.code_lease = (XrRbcCodeLease){0};
    CHECK(!xr_rbc_module_value_code(&raw,0));
    XrRbcExecution *invalid = NULL;
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&invalid) == XR_RBC_INVALID && !invalid);
    raw = *module; raw.functions = NULL; CHECK(!xr_rbc_module_value_code(&raw,0));
    raw = *module; --raw.function_count; CHECK(!xr_rbc_module_value_code(&raw,0));
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrRbcValueCode *code = xr_rbc_module_value_code(module,f);
        CHECK(code && code->homes == xr_rbc_module_value_homes(module,f));
        for (uint32_t v = 1; v <= code->value_count; ++v)
            if (code->homes->values[v].owner_count) CHECK(code->locations[v].kind == XR_RBC_VALUE_NOWHERE);
        for (size_t pc = 0; pc < module->functions[f].code_count; ++pc) {
            uint32_t word = module->functions[f].code[pc];
            if ((word & 255) == XR_RBC_VALUE_ACCESS) CHECK((word >> 16) < code->access_count);
        }
    }
}
static void allocation_failures(void) {
    XrLirFunction root = {0}; XrLirModule *module = managed_state_fixture(&root);
    attempts = 0; XrRbcProgram *program = compile(module,root); size_t count = attempts;
    xr_rbc_program_destroy(program); CHECK(!live && count);
    for (size_t n = 1; n <= count; ++n) {
        attempts = 0; failure = n; program = NULL;
        CHECK(xr_rbc_compile(module,root,&program,NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY && !program);
        CHECK(!live);
    }
    failure = SIZE_MAX;
    program = compile(module,root); check_receiving(program);
    const XrRbcValueCode *code = xr_rbc_module_value_code(xr_rbc_program_view(program),0);
    size_t mixed = 0;
    for (size_t n = 0; n < code->access_count; ++n) {
        XrRbcValueAccess access = code->accesses[n];
        if (access.operation != XR_RBC_VALUE_EDGE) continue;
        CHECK(access.index != UINT32_MAX && access.count == 4 && access.first + 2 * access.count <= code->argument_count);
        ++mixed;
    }
    CHECK(mixed == 3);
    xr_lir_module_destroy(module); run_after_input_retirement(program,3311);
    printf("typed code: %zu compiler allocation failures preserved empty output and retired all metadata\n",count);
}
static void scalar_state(void) {
    XrLirModule *module = NULL; CHECK(xr_lir_module_create(NULL,&module) == XR_LIR_OK);
    XrLirStateModule owner = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_I64,.mutable=true,.size=8,.alignment=8};
    XrLirStateLayout layout = {&owner,1,NULL,0,&slot,1,8,8};
    CHECK(xr_lir_set_state_layout(module,&layout) == XR_LIR_OK);
    XrLirFunction root = {0}; XrLirType state_type = XR_LIR_STATE;
    XrLirFunctionSpec spec = {.name="scalar_state",.name_length=12,.parameters=&state_type,.parameter_count=1,.result=XR_LIR_I64};
    CHECK(xr_lir_declare_function(module,&spec,&root) == XR_LIR_OK && xr_lir_define_function(module,root) == XR_LIR_OK);
    CHECK(xr_lir_set_state_entry(module,root) == XR_LIR_OK);
    XrLirBlock block = {0}; CHECK(xr_lir_add_block(module,root,NULL,0,&block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module,root,block) == XR_LIR_OK);
    XrLirValue state = {0}; CHECK(xr_lir_function_parameter(module,root,0,&state) == XR_LIR_OK);
    managed_state_effect(module,block,XR_LIR_MODULE_BEGIN,&state,1,0);
    XrLirValue value = managed_state_value(module,block,XR_LIR_CONST,XR_LIR_I64,NULL,0,42), operands[] = {state,value};
    XrLirValue reference = managed_state_value(module,block,XR_LIR_STATE_CONSTRUCT,XR_LIR_STATE_REF,operands,2,0);
    managed_state_effect(module,block,XR_LIR_STATE_PUBLISH,&reference,1,0);
    managed_state_effect(module,block,XR_LIR_MODULE_READY,&state,1,0);
    value = managed_state_value(module,block,XR_LIR_STATE_READ,XR_LIR_I64,&reference,1,0);
    CHECK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&value,.operand_count=1}) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module,NULL) == XR_LIR_OK);
    XrRbcProgram *program = compile(module,root);
    CHECK(!xr_rbc_module_value_code(xr_rbc_program_view(program),0));
    xr_lir_module_destroy(module); run_after_input_retirement(program,42);
}
static void scalar_body_calls(void) {
    for (unsigned typed = 0; typed < 2; ++typed) {
        XrLirModule *module = NULL; CHECK(xr_lir_module_create(NULL,&module) == XR_LIR_OK);
        XrLirType integer = XR_LIR_I64; XrLirFunction callee = {0}, root = {0};
        XrLirFunctionSpec spec = {.name="body_length",.name_length=11,.parameters=&integer,
            .parameter_count=1,.result=integer};
        CHECK(xr_lir_declare_function(module,&spec,&callee) == XR_LIR_OK && xr_lir_define_function(module,callee) == XR_LIR_OK);
        XrLirBlock block = {0}; CHECK(xr_lir_add_block(module,callee,NULL,0,&block) == XR_LIR_OK);
        CHECK(xr_lir_set_entry(module,callee,block) == XR_LIR_OK);
        XrLirValue parameter = {0}; CHECK(xr_lir_function_parameter(module,callee,0,&parameter) == XR_LIR_OK);
        XrLirLiteral literal = {0}; CHECK(xr_lir_add_literal(module,(const uint8_t *)"body",4,&literal) == XR_LIR_OK);
        XrLirValue text = managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,literal.id);
        XrLirValue length = managed_state_value(module,block,XR_LIR_STRING_LEN,integer,&text,1,0), add[] = {parameter,length};
        XrLirValue result = managed_state_value(module,block,XR_LIR_ADD,integer,add,2,0);
        managed_state_effect(module,block,XR_LIR_STRING_DROP,&text,1,0);
        CHECK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
            .operands=&result,.operand_count=1}) == XR_LIR_OK);
        const char *name = typed ? "typed_caller" : "scalar_caller";
        spec = (XrLirFunctionSpec){.name=name,.name_length=strlen(name),.result=integer};
        CHECK(xr_lir_declare_function(module,&spec,&root) == XR_LIR_OK && xr_lir_define_function(module,root) == XR_LIR_OK);
        block = (XrLirBlock){0}; CHECK(xr_lir_add_block(module,root,NULL,0,&block) == XR_LIR_OK);
        CHECK(xr_lir_set_entry(module,root,block) == XR_LIR_OK);
        XrLirValue caller_text = {0};
        if (typed) {
            literal = (XrLirLiteral){0}; CHECK(xr_lir_add_literal(module,(const uint8_t *)"caller",6,&literal) == XR_LIR_OK);
            caller_text = managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,literal.id);
        }
        XrLirValue results[2] = {{0},{0}};
        for (unsigned n = 0; n < 2; ++n) {
            XrLirValue argument = managed_state_value(module,block,XR_LIR_CONST,integer,NULL,0,38 + n);
            CHECK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL,.type=integer,.callee=callee,
                .operands=&argument,.operand_count=1},&results[n]) == XR_LIR_OK);
        }
        result = managed_state_value(module,block,XR_LIR_ADD,integer,results,2,0);
        if (typed) {
            length = managed_state_value(module,block,XR_LIR_STRING_LEN,integer,&caller_text,1,0);
            add[0] = result; add[1] = length;
            result = managed_state_value(module,block,XR_LIR_ADD,integer,add,2,0);
            managed_state_effect(module,block,XR_LIR_STRING_DROP,&caller_text,1,0);
        }
        CHECK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
            .operands=&result,.operand_count=1}) == XR_LIR_OK);
        CHECK(xr_lir_module_finish(module,NULL) == XR_LIR_OK);
        XrRbcProgram *program = compile(module,root);
        const XrRbcModule *code = xr_rbc_program_view(program);
        CHECK(code->function_count == 2);
        const XrRbcValueCode *body = xr_rbc_module_value_code(code,1), *caller = xr_rbc_module_value_code(code,0);
        CHECK(body && body->homes->owned_frame_needed && body->parameter_count == 1);
        CHECK(typed ? caller != NULL : caller == NULL);
        unsigned ordinary_calls = 0;
        for (size_t pc = 0; pc < code->functions[0].code_count; ++pc)
            ordinary_calls += (code->functions[0].code[pc] & 255) == XR_RBC_CALL;
        CHECK(ordinary_calls == 2);
        for (size_t n = 0; caller && n < caller->access_count; ++n)
            CHECK(caller->accesses[n].operation != XR_RBC_VALUE_CALL);
        bool typed_return = false;
        for (size_t n = 0; n < body->access_count; ++n)
            typed_return |= body->accesses[n].operation == XR_RBC_VALUE_RETURN;
        CHECK(typed_return);
        xr_lir_module_destroy(module); run_after_input_retirement(program,typed ? 91 : 85);
    }
    puts("typed scalar bodies: ordinary calls lazily enter and return to scalar and typed callers passed");
}
int main(void) {
    XrLirFunction root = {0}; XrLirModule *module = string_call(&root);
    XrRbcProgram *program = compile(module,root); check_receiving(program);
    const XrRbcValueCode *callee = xr_rbc_module_value_code(xr_rbc_program_view(program),1);
    CHECK(callee->parameter_count == 1 && callee->homes->values[callee->parameters[0]].parameter == 1);
    xr_lir_module_destroy(module); run_after_input_retirement(program,4);
    allocation_failures(); scalar_state(); scalar_body_calls(); CHECK(!live);
    puts("typed code: real String call/return, mixed State edges, source retirement and code lease passed");
    return 0;
}
