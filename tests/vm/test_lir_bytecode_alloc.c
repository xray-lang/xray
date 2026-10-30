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
#include "vm/register/xrbc.c"
// Scope fault injection to the actual common admission called before publish.
static XrRbcStatus compile_admission(const XrRbcModule *module,const XrtLocationOwner *locations) {
    in_compile_admission=true;admission_attempts=0;
    XrRbcStatus status=xr_rbc_module_admit(module,locations);
    in_compile_admission=false;return status;
}
#define xr_rbc_module_admit compile_admission
#include "vm/register/xrbc_lir.c"
#undef xr_rbc_module_admit

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
        CHECK(function->source_ids[0] == 0);
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

int main(void) {
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
        CHECK(function->source_ids[0] == 0);
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
    compiled_source_maps();
    frame_cfg_compiler_failures();
    CHECK(!live_allocations);
    puts("lir_bytecode_alloc: passed (representative compiler/owner failures, relocating buffers)");
    return 0;
}
