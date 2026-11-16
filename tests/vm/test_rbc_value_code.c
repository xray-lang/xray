/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_rbc_value_code.c - Typed receiving descriptions and their actual code lease
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_string.h"
#include "vm/register/xrbc_lir.h"
#include "vm/register/xrbc_value_code.h"
#include "xray_native_string_value.h"
#include "runtime/native/xnative_array_backing.h"
#include "runtime/native/xnative_map_backing.h"
#include "xray_native_array_slice.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); abort(); } } while (0)

#define OK(c) CHECK((c)==XR_LIR_OK)
#include "../lir/lir_array_ref_fixture.h"

static size_t attempts, failure = SIZE_MAX, compiler_live_allocations;
static struct { void *pointer; size_t bytes; } allocations[4096];
static size_t allocation_index(void *pointer) {
    for (size_t n = 0; n < 4096; ++n) if (allocations[n].pointer == pointer) return n;
    CHECK(false); return 0;
}
static void *allocate(size_t bytes) {
    if (++attempts == failure) return NULL;
    void *memory = xr_malloc(bytes);
    if (memory) {
        size_t n = allocation_index(NULL); allocations[n].pointer = memory; allocations[n].bytes = bytes; ++compiler_live_allocations;
    }
    return memory;
}
static void *allocate_zero(size_t count, size_t bytes) {
    CHECK(!bytes || count <= SIZE_MAX / bytes);
    void *memory = allocate(count * bytes); if (memory) memset(memory,0,count * bytes); return memory;
}
static void release(void *memory) {
    if (!memory) return;
    size_t n = allocation_index(memory); allocations[n].pointer = NULL; CHECK(compiler_live_allocations); --compiler_live_allocations; xr_free(memory);
}
static void *resize(void *old, size_t bytes) {
    if (++attempts == failure) return NULL;
    void *memory = xr_malloc(bytes); if (!memory) return NULL;
    size_t n = allocation_index(old);
    if (old) { memcpy(memory,old,bytes < allocations[n].bytes ? bytes : allocations[n].bytes); xr_free(old); }
    else ++compiler_live_allocations;
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
    snapshot.code_lease.release(snapshot.code_lease.owner); CHECK(!compiler_live_allocations);
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
    /* Explicit raw descriptions are borrowed data. Seeing the real metadata
     * cannot authorize the owning slots required by this managed program. */
    const XrRbcValueCode *borrowed=xr_rbc_module_value_code(&raw,0);
    CHECK(borrowed==xr_rbc_module_value_code(module,0) && borrowed &&
        borrowed->homes && borrowed->homes->owned_frame_needed);
    XrRbcExecution *invalid = NULL;
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&invalid) == XR_RBC_INVALID && !invalid);
    raw.borrowed_value_codes=NULL; raw.borrowed_value_code_count=0;
    CHECK(!xr_rbc_module_value_code(&raw,0));
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&invalid) == XR_RBC_INVALID && !invalid);
    raw = *module; raw.functions = NULL; CHECK(!xr_rbc_module_value_code(&raw,0));
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&invalid) == XR_RBC_INVALID && !invalid);
    raw = *module; --raw.function_count; CHECK(!xr_rbc_module_value_code(&raw,0));
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&invalid) == XR_RBC_INVALID && !invalid);
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
    xr_rbc_program_destroy(program); CHECK(!compiler_live_allocations && count);
    for (size_t n = 1; n <= count; ++n) {
        attempts = 0; failure = n; program = NULL;
        CHECK(xr_rbc_compile(module,root,&program,NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY && !program);
        CHECK(!compiler_live_allocations);
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
static void string_outcome_lifetime(void) {
    static const uint8_t bytes[] = {'A',0xc3,0xa9,0,0xe4,0xb8,0xad};
    XrLirModule *module = NULL; CHECK(xr_lir_module_create(NULL,&module) == XR_LIR_OK);
    XrLirFunction root = {0};
    XrLirFunctionSpec spec = {.name="string_result",.name_length=13,.result=XR_LIR_STRING};
    CHECK(xr_lir_declare_function(module,&spec,&root) == XR_LIR_OK && xr_lir_define_function(module,root) == XR_LIR_OK);
    XrLirBlock block = {0}; CHECK(xr_lir_add_block(module,root,NULL,0,&block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module,root,block) == XR_LIR_OK);
    XrLirLiteral literal = {0}; CHECK(xr_lir_add_literal(module,bytes,sizeof(bytes),&literal) == XR_LIR_OK);
    XrLirValue value = managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,literal.id);
    CHECK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&value,.operand_count=1}) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module,NULL) == XR_LIR_OK);
    XrRbcProgram *program = compile(module,root);
    xr_lir_module_destroy(module);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution) == XR_RBC_OK);
    xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome) == XR_RBC_OK);
    xr_rbc_execution_free(&execution); CHECK(!execution);
    const XrtManagedHeader *borrowed = NULL;
    CHECK(xr_rbc_outcome_string(outcome,&borrowed) && borrowed);
    const XrtStringBacking *text = (const XrtStringBacking *)borrowed;
    CHECK(text->length == sizeof(bytes) && text->runes == 4 && text->capacity >= text->length);
    CHECK(!memcmp(text->bytes,bytes,sizeof(bytes)) && !text->bytes[text->length]);
    const XrtManagedHeader *again = NULL;
    CHECK(xr_rbc_outcome_string(outcome,&again) && again == borrowed);
    xr_rbc_outcome_free(&outcome); CHECK(!outcome && !compiler_live_allocations);
    puts("String outcome: fixed UTF-8 backing survives compiler, program and execution retirement passed");
}
static void array_binding_calls(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_STRING,XR_LIR_U8,XR_LIR_U64};
    for (size_t e=0;e<4;++e) {
        XrLirModule *module=NULL; OK(array_ref_fixture(elements[e],&module));
        XrLirFunction root={0}; OK(xr_lir_function_at(module,0,&root));
        attempts=0; XrRbcProgram *program=compile(module,root); size_t sites=attempts;
        if (!e) {
            xr_rbc_program_destroy(program); program=NULL; CHECK(!compiler_live_allocations);
            for (size_t n=1;n<=sites;++n) {
                attempts=0; failure=n;
                CHECK(xr_rbc_compile(module,root,&program,NULL)==XR_RBC_COMPILE_OUT_OF_MEMORY && !program && !compiler_live_allocations);
            }
            failure=SIZE_MAX; program=compile(module,root);
            printf("Array typed code allocation rollback sites: %zu\n",sites);
        }
        check_receiving(program);
        const XrRbcModule *image=xr_rbc_program_view(program);
        for (uint32_t f=0;f<image->function_count;++f) {
            const XrRbcValueCode *code=xr_rbc_module_value_code(image,f);
            CHECK(code && !image->functions[f].cell_count);
            for (uint32_t v=1;v<=code->value_count;++v)
                if (code->homes->bindings && code->homes->bindings[v].type)
                    CHECK(code->locations[v].kind==XR_RBC_VALUE_NOWHERE);
        }
        xr_lir_module_destroy(module); run_after_input_retirement(program,0);
    }
    puts("Array binding calls: four families use stable local and State owners after compiler input retirement passed");
}
static void array_outcome_lifetime(void) {
    static const uint8_t bytes[]={'A',0xc3,0xa9,0,0xe4,0xb8,0xad};
    XrLirModule *module=NULL; OK(xr_lir_array_module_create(NULL,&module));
    XrLirFunction root={0}; XrLirFunctionSpec spec={.name="array_result",.name_length=12,.result=XR_LIR_ARRAY};
    OK(xr_lir_declare_function(module,&spec,&root));
    OK(xr_lir_set_array_result(module,root,&(XrLirArrayShape){XR_LIR_STRING}));
    OK(xr_lir_define_function(module,root)); XrLirBlock block={0};
    OK(xr_lir_add_block(module,root,NULL,0,&block)); OK(xr_lir_set_entry(module,root,block));
    XrLirLiteral literal={0}; OK(xr_lir_add_literal(module,bytes,sizeof(bytes),&literal));
    XrLirValue text=managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,literal.id),args[]={text,text},array={0};
    uint32_t constructor=0; OK(xr_lir_array_operation(module,XR_LIR_STRING,XR_LIR_ARRAY_OP_NEW,&constructor));
    OK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,
        .intrinsic=constructor,.operands=args,.operand_count=2},&array));
    managed_state_effect(module,block,XR_LIR_STRING_DROP,&text,1,0);
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&array,.operand_count=1}));
    OK(xr_lir_module_finish(module,NULL)); XrRbcProgram *program=compile(module,root); check_receiving(program);
    const XrRbcModule *view=xr_rbc_program_view(program); XrRbcModule raw=*view; raw.code_lease=(XrRbcCodeLease){0};
    XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
    uint32_t word=XR_RBC_RET; XrRbcRegisterKind kind=XR_RBC_ARRAY_STRING;
    XrRbcFunction function={.code=&word,.code_count=1,.register_count=1,.register_kinds=&kind,.register_kind_count=1,.result_kind=kind};
    raw=(XrRbcModule){.functions=&function,.function_count=1};
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
    xr_lir_module_destroy(module); CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(program); XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK); xr_rbc_execution_free(&execution);
    const XrtManagedHeader *header=NULL; CHECK(xr_rbc_outcome_array(outcome,&header) && header);
    const XrtArrayBacking *result=(const XrtArrayBacking *)header; CHECK(result->length==2 && result->capacity>=2);
    const XrtStringBacking *first=NULL,*second=NULL;
    CHECK(xrt_array_backing_string_at(result,0,&first)==XRT_MANAGED_OK && xrt_array_backing_string_at(result,1,&second)==XRT_MANAGED_OK);
    CHECK(first==second && first->length==sizeof(bytes) && first->runes==4 && !memcmp(first->bytes,bytes,sizeof(bytes)));
    XrtManagedHeader *owned=NULL; CHECK(xr_rbc_outcome_take_array(outcome,&owned) && owned==header);
    xr_rbc_outcome_free(&outcome); CHECK(!compiler_live_allocations);
    CHECK(first->length==sizeof(bytes) && !memcmp(first->bytes,bytes,sizeof(bytes))); xrt_managed_drop(owned);
    puts("Array outcome: real aliased String tail survives code and outcome retirement; raw Array returns reject passed");
}
static void map_outcome_lifetime(void) {
    static const uint8_t bytes[]={'A',0xc3,0xa9,0,0xe4,0xb8,0xad};
    XrLirModule *module=NULL; OK(xr_lir_module_create(NULL,&module));
    OK(xr_lir_map_catalog_install(module,false,false,false,false,false));
    XrLirType optional=0; OK(xr_lir_nullable_type(module,XR_LIR_I64,&optional));
    XrLirFunction root={0},echo={0}; XrLirType map_type=XR_LIR_MAP_STRING_I64;
    XrLirFunctionSpec spec={.name="map_result",.name_length=10,.result=map_type};
    OK(xr_lir_declare_function(module,&spec,&root)); OK(xr_lir_define_function(module,root));
    spec=(XrLirFunctionSpec){.name="map_echo",.name_length=8,.parameters=&map_type,.parameter_count=1,.result=map_type};
    OK(xr_lir_declare_function(module,&spec,&echo)); OK(xr_lir_define_function(module,echo));
    XrLirBlock body={0}; OK(xr_lir_add_block(module,echo,NULL,0,&body)); OK(xr_lir_set_entry(module,echo,body));
    XrLirValue parameter={0}; OK(xr_lir_function_parameter(module,echo,0,&parameter));
    XrLirValue copy=managed_state_value(module,body,XR_LIR_MAP_RETAIN,map_type,&parameter,1,0);
    OK(xr_lir_set_terminator(module,body,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&copy,.operand_count=1}));
    XrLirType lookup_parameters[]={map_type,XR_LIR_STRING}; XrLirFunction lookup={0};
    spec=(XrLirFunctionSpec){.name="map_lookup",.name_length=10,.parameters=lookup_parameters,.parameter_count=2,.result=optional};
    OK(xr_lir_declare_function(module,&spec,&lookup)); OK(xr_lir_define_function(module,lookup));
    body=(XrLirBlock){0}; OK(xr_lir_add_block(module,lookup,NULL,0,&body)); OK(xr_lir_set_entry(module,lookup,body));
    XrLirValue lookup_args[2]; OK(xr_lir_function_parameter(module,lookup,0,&lookup_args[0]));
    OK(xr_lir_function_parameter(module,lookup,1,&lookup_args[1])); uint32_t getter=0;
    OK(xr_lir_map_operation(module,XR_LIR_MAP_OP_GET,map_type,&getter)); XrLirValue selected={0};
    OK(xr_lir_emit_value(module,body,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=optional,.intrinsic=getter,
        .operands=lookup_args,.operand_count=2},&selected));
    OK(xr_lir_set_terminator(module,body,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&selected,.operand_count=1}));
    XrLirBlock block={0}; OK(xr_lir_add_block(module,root,NULL,0,&block)); OK(xr_lir_set_entry(module,root,block));
    XrLirLiteral literal={0}; OK(xr_lir_add_literal(module,bytes,sizeof(bytes),&literal));
    XrLirValue text=managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,literal.id);
    XrLirValue value=managed_state_value(module,block,XR_LIR_CONST,XR_LIR_I64,NULL,0,42),args[]={text,value},map={0};
    uint32_t constructor=0; OK(xr_lir_map_operation(module,XR_LIR_MAP_OP_NEW,map_type,&constructor));
    OK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=map_type,
        .intrinsic=constructor,.operands=args,.operand_count=2},&map));
    XrLirValue from_lookup={0},actuals[]={map,text};
    OK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL,.type=optional,.callee=lookup,
        .operands=actuals,.operand_count=2},&from_lookup));
    args[1]=managed_state_value(module,block,XR_LIR_NULLABLE_UNWRAP,XR_LIR_I64,&from_lookup,1,0);
    managed_state_effect(module,block,XR_LIR_MAP_DROP,&map,1,0);
    OK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=map_type,
        .intrinsic=constructor,.operands=args,.operand_count=2},&map));
    XrLirValue result={0}; OK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CALL,.type=map_type,
        .callee=echo,.operands=&map,.operand_count=1},&result));
    managed_state_effect(module,block,XR_LIR_MAP_DROP,&map,1,0);
    managed_state_effect(module,block,XR_LIR_STRING_DROP,&text,1,0);
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&result,.operand_count=1}));
    OK(xr_lir_module_finish(module,NULL));
    attempts=0; XrRbcProgram *program=compile(module,root); size_t sites=attempts;
    xr_rbc_program_destroy(program); program=NULL; CHECK(!compiler_live_allocations && sites);
    for (size_t n=1;n<=sites;++n) {
        attempts=0; failure=n;
        CHECK(xr_rbc_compile(module,root,&program,NULL)==XR_RBC_COMPILE_OUT_OF_MEMORY && !program && !compiler_live_allocations);
    }
    failure=SIZE_MAX; program=compile(module,root); check_receiving(program);
    XrRbcModule raw=*xr_rbc_program_view(program); raw.code_lease=(XrRbcCodeLease){0}; XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
    uint32_t word=XR_RBC_RET; XrRbcRegisterKind kind=XR_RBC_MAP_STRING_I64;
    XrRbcFunction function={.code=&word,.code_count=1,.register_count=1,.register_kinds=&kind,.register_kind_count=1,.result_kind=kind};
    raw=(XrRbcModule){.functions=&function,.function_count=1};
    CHECK(xr_rbc_module_prepare(&raw,0,NULL,0,xr_rbc_program_locations(program),&execution)==XR_RBC_INVALID && !execution);
    xr_lir_module_destroy(module); CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK);
    xr_rbc_program_destroy(program); XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
    CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK); xr_rbc_execution_free(&execution);
    const XrtManagedHeader *header=NULL; CHECK(xr_rbc_outcome_map(outcome,&header) && header);
    const XrtMapBacking *answer=(const XrtMapBacking *)header;
    CHECK(answer->length==1 && answer->capacity>=1 && answer->rows[0].key);
    const XrtStringBacking *key=answer->rows[0].key;
    CHECK(key->length==sizeof(bytes) && key->runes==4 && !memcmp(key->bytes,bytes,sizeof(bytes)));
    int64_t found_value=0; bool found=false;
    CHECK(xrt_map_backing_get(answer,key,&found_value,&found)==XRT_MANAGED_OK && found && found_value==42);
    XrtManagedHeader *owned=NULL; CHECK(xr_rbc_outcome_take_map(outcome,&owned) && owned==header);
    xr_rbc_outcome_free(&outcome); CHECK(!compiler_live_allocations);
    CHECK(key->length==sizeof(bytes) && !memcmp(key->bytes,bytes,sizeof(bytes))); xrt_managed_drop(owned);
    printf("Map typed code: %zu allocation rollback sites; real UTF-8 key and call/result owner survive code retirement\n",sites);
}
static void pod_outcome_lifetime(void) {
    XrLirModule *module=NULL; OK(xr_lir_module_create(NULL,&module));
    XrLirType optional=0; OK(xr_lir_nullable_type(module,XR_LIR_I64,&optional));
    XrLirFunction root={0}; XrLirFunctionSpec spec={.name="pod_result",.name_length=10,.result=optional};
    OK(xr_lir_declare_function(module,&spec,&root)); OK(xr_lir_define_function(module,root));
    XrLirBlock block={0}; OK(xr_lir_add_block(module,root,NULL,0,&block)); OK(xr_lir_set_entry(module,root,block));
    XrLirLiteral literal={0}; OK(xr_lir_add_literal(module,(const uint8_t *)"temporary",9,&literal));
    XrLirValue text=managed_state_value(module,block,XR_LIR_STRING_LITERAL,XR_LIR_STRING,NULL,0,literal.id);
    managed_state_effect(module,block,XR_LIR_STRING_DROP,&text,1,0);
    XrLirValue value=managed_state_value(module,block,XR_LIR_CONST,XR_LIR_I64,NULL,0,42);
    value=managed_state_value(module,block,XR_LIR_NULLABLE_SOME,optional,&value,1,0);
    OK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&value,.operand_count=1}));
    OK(xr_lir_module_finish(module,NULL)); XrRbcProgram *program=compile(module,root); check_receiving(program);
    xr_lir_module_destroy(module); XrRbcExecution *execution=NULL;
    CHECK(xr_rbc_program_prepare(program,NULL,0,&execution)==XR_RBC_OK); xr_rbc_program_destroy(program);
    XrRbcOutcome *outcome=xr_rbc_execution_run(execution); CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK);
    xr_rbc_execution_free(&execution); XrPodType type={0}; const uint8_t *payload=NULL; int64_t result=0;
    CHECK(xr_rbc_outcome_aggregate(outcome,&type,&payload) && type.kind==XR_POD_NULLABLE && type.element==XR_POD_I64);
    CHECK(type.physical.size==16 && payload[0]==1); memcpy(&result,payload+8,8); CHECK(result==42);
    xr_rbc_outcome_free(&outcome); CHECK(!compiler_live_allocations);
    puts("POD outcome: typed body publishes inline Nullable bytes after code retirement passed");
}
#include "../lir/lir_record_fixture.h"
static void inline_code_retirement(void) {
    XrLirFunction root={0}; XrLirModule *module=record_fixture(&root);
    attempts=0; XrRbcProgram *program=compile(module,root); size_t sites=attempts;
    xr_rbc_program_destroy(program); program=NULL; CHECK(!compiler_live_allocations);
    for (size_t n=1;n<=sites;++n) {
        attempts=0; failure=n;
        CHECK(xr_rbc_compile(module,root,&program,NULL)==XR_RBC_COMPILE_OUT_OF_MEMORY && !program && !compiler_live_allocations);
    }
    failure=SIZE_MAX; program=compile(module,root); check_receiving(program);
    const XrRbcModule *code=xr_rbc_program_view(program); bool field=false,record=false;
    for (uint32_t f=0;f<code->function_count;++f) {
        const XrRbcValueCode *values=xr_rbc_module_value_code(code,f); CHECK(values);
        for (size_t n=0;n<values->access_count;++n) {
            field|=values->accesses[n].operation==XR_RBC_VALUE_RECORD_FIELD;
            record|=values->accesses[n].operation==XR_RBC_VALUE_RECORD_NEW;
        }
    }
    CHECK(field && record); xr_lir_module_destroy(module); run_after_input_retirement(program,100);
    printf("inline Record/Nullable code: %zu allocation failures preserved empty output and retired metadata\n",sites);
}
#include "lir_slice_bytecode_cases.h"
#include "lir_string_append_cases.h"
int main(void) {
    XrLirFunction root = {0}; XrLirModule *module = string_call(&root);
    XrRbcProgram *program = compile(module,root); check_receiving(program);
    const XrRbcValueCode *callee = xr_rbc_module_value_code(xr_rbc_program_view(program),1);
    CHECK(callee->parameter_count == 1 && callee->homes->values[callee->parameters[0]].parameter == 1);
    xr_lir_module_destroy(module); run_after_input_retirement(program,4);
    allocation_failures(); scalar_state(); scalar_body_calls(); string_outcome_lifetime();
    array_binding_calls(); array_outcome_lifetime(); map_outcome_lifetime(); pod_outcome_lifetime(); inline_code_retirement();
    slice_bytecode_retirement(); string_append_retirement(); CHECK(!compiler_live_allocations);
    puts("typed code: real String call/return, mixed State edges, source retirement and code lease passed");
    return 0;
}
