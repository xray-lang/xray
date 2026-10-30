/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_register_vm_alloc.c - Moving stacks and representative execution OOMs
 *
 * KEY CONCEPT:
 *   Real stack growth always relocates. Preparation failures publish nothing;
 *   a failure after invocation still transfers the already owned outcome.
 */
#include "register_vm_programs.h"
#include "base/xmalloc.h"

static struct { void *pointer; size_t size; } blocks[4];
static enum { NO_FAILURE, OUTCOME_FAILURE, INITIAL_STACK_FAILURE, GROWTH_FAILURE,
    CELL_INITIAL_FAILURE, CELL_GROWTH_FAILURE, STRING_INDEX_FAILURE, STRING_JOIN_FAILURE,
    SPILL_STATE_FAILURE, SPILL_INITIAL_FAILURE, SPILL_GROWTH_FAILURE } fault;
static size_t spill_state_failure_bytes=2;
static bool observe_frame_drops;
static unsigned observed_cell_drops,observed_cell_writes;
static void observed_string_drop(XrtString **value,const XrRbcFunction *function,size_t pc) {
    if(observe_frame_drops) {
        XrRbcOp op=(XrRbcOp)(function->code[pc]&255);
        observed_cell_drops+=op==XR_RBC_STRING_CELL_DROP;
        observed_cell_writes+=op==XR_RBC_CELL_WRITE;
    }
    xrt_string_drop(value);
}

static void *real_malloc(size_t size) { return xr_malloc(size); }
static void *execution_malloc(size_t size);
static void *real_calloc(size_t count, size_t size) { return xr_calloc(count, size); }
static void *execution_calloc(size_t count, size_t size);

static void *moving_realloc(void *old, size_t size) {
    size_t index = 0;
    while (index < 4 && blocks[index].pointer != old) ++index;
    XR_CHECK(index < 4, "unknown stack allocation");
    if ((fault == INITIAL_STACK_FAILURE && !old) || (fault == GROWTH_FAILURE && old) ||
        (fault == CELL_INITIAL_FAILURE && !old && index == 2) ||
        (fault == CELL_GROWTH_FAILURE && old && index == 2) ||
        (fault == SPILL_INITIAL_FAILURE && !old && index == 3) ||
        (fault == SPILL_GROWTH_FAILURE && old && index == 3)) {
        fault = NO_FAILURE;
        return NULL;
    }
    void *fresh = xr_malloc(size);
    if (!fresh) return NULL;
    if (old) {
        XR_CHECK(size >= blocks[index].size, "interpreter only grows buffers");
        memcpy(fresh, old, blocks[index].size);
        xr_free(old);
    }
    blocks[index].pointer = fresh;
    blocks[index].size = size;
    return fresh;
}

static void moving_free(void *pointer) {
    if (!pointer) return;
    for (size_t i = 0; i < 4; ++i) {
        if (blocks[i].pointer != pointer) continue;
        blocks[i].pointer = NULL;
        blocks[i].size = 0;
    }
    xr_free(pointer);
}

// Only this VM instance's allocator is interposed. The common execution
// boundary and location owner remain the actual linked production runtime.
#undef xr_malloc
#define xr_malloc(size) execution_malloc(size)
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_calloc(count, size) execution_calloc(count, size)
#define xr_realloc(pointer, size) moving_realloc(pointer, size)
#define xr_free(pointer) moving_free(pointer)
#define xrt_string_drop(value) observed_string_drop((value),function,pc-1)
#include "vm/register/xrbc.c"
#undef xrt_string_drop

static void *execution_malloc(size_t size) {
    if (fault == SPILL_STATE_FAILURE && size == spill_state_failure_bytes) { fault=NO_FAILURE;return NULL; }
    if (fault == STRING_INDEX_FAILURE) { fault = NO_FAILURE; return NULL; }
    return real_malloc(size);
}
static void *execution_calloc(size_t count, size_t size) {
    if (fault == STRING_JOIN_FAILURE) { fault = NO_FAILURE; return NULL; }
    if (fault == OUTCOME_FAILURE && size == sizeof(XrRbcOutcome)) {
        fault = NO_FAILURE;
        return NULL;
    }
    return real_calloc(count, size);
}

static void invoke(const XrRbcModule *module, XrtLocationOwner *owner, bool prepare_fails,
    XrRbcStatus expected) {
    uint64_t argument = 128, result = 999;
    XrRbcExecution *execution = NULL;
    XrRbcStatus status = xr_rbc_module_prepare(module, 0, &argument, 1, owner, &execution);
    if (prepare_fails) {
        XR_CHECK(fault == NO_FAILURE, "preparation fault was reached");
        XR_CHECK(status == XR_RBC_OUT_OF_MEMORY && !execution, "failed prepare publishes nothing");
        return;
    }
    XR_CHECK(status == XR_RBC_OK && execution, "prepared execution is owned");
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    XR_CHECK(fault == NO_FAILURE, "growth fault was reached during invocation");
    XR_CHECK(outcome && xr_rbc_outcome_status(outcome) == expected, "one owned guest result");
    bool value = xr_rbc_outcome_bits(outcome, &result);
    XR_CHECK(value == (expected == XR_RBC_OK), "bits require success");
    XR_CHECK(result == (value ? argument : 999), "success or preserved failure output");
    xr_rbc_execution_free(&execution);
    xr_rbc_outcome_free(&outcome);
}

static void panic_after_growth(void) {
    uint32_t code[12], ids[12] = {0};
    XrRbcFunction function = make_countdown(code);
    // The base case faults only after both indexed arrays have repeatedly relocated.
    emit(code, 10, XR_RBC_DIV, 0, 0, 1);
    emit(code, 11, XR_RBC_RET0, 0, 0, 0);
    function.code_count = 12;
    ids[10] = 1;
    function.source_ids = ids;
    function.source_id_count = 12;
    XrRbcModule module = {&function, 1, NULL, 0, NULL, 0};
    XrtSourceLocation location = {"grown.xr", 8, 19, 2, 19, 7};
    XrtLocationOwner *owner = NULL;
    XR_CHECK(xrt_locations_create(&location, 1, &owner) == XRT_LOCATION_OK, "owned fault site");
    for (unsigned kind = 0; kind < 2; ++kind) {
        emit(code, 10, kind ? XR_RBC_REM : XR_RBC_DIV, 0, 0, 1);
        uint64_t argument = 128;
        XrRbcExecution *execution = NULL;
        XR_CHECK(xr_rbc_module_prepare(&module, 0, &argument, 1, owner, &execution) == XR_RBC_OK,
            "prepared growing execution");
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        XR_CHECK(outcome, "owned outcome after transfer");
        xr_rbc_execution_free(&execution);
        XrtArithmeticPanic facts;
        const XrtLocationOwner *borrowed = NULL;
        XR_CHECK(xr_rbc_outcome_arithmetic_panic(outcome, &facts, &borrowed), "owned panic");
        XR_CHECK(facts.code == (kind ? 421u : 420u) && facts.source_id == 1,
            "actual deepest instruction site");
        XR_CHECK(xrt_locations_get(borrowed, facts.source_id)->line == 19, "retained source");
        xr_rbc_outcome_free(&outcome);
    }
    xrt_locations_release(owner);
}


static void cell_growth(bool panic) {
    uint32_t root[7], body[17], ids[17] = {0};
    immediate(root, 0, XR_RBC_LOADI, 1, 7);
    immediate(root, 1, XR_RBC_CELL_NEW, 1, 0);
    emit(root, 2, XR_RBC_MOV, 4, 0, 0);
    emit(root, 3, XR_RBC_MOV, 5, 1, 0);
    immediate(root, 4, XR_RBC_CALL, 3, 1);
    emit(root, 5, XR_RBC_CELL_READ, 0, 1, 0);
    emit(root, 6, XR_RBC_RET, 0, 0, 0);
    immediate(body, 0, XR_RBC_LOADI, 2, 1);
    immediate(body, 1, XR_RBC_CELL_NEW, 2, 0);
    emit(body, 2, XR_RBC_CELL_READ, 3, 1, 0);
    emit(body, 3, XR_RBC_CELL_READ, 2, 2, 0);
    emit(body, 4, XR_RBC_ADD, 3, 3, 2);
    emit(body, 5, XR_RBC_CELL_WRITE, 1, 3, 0);
    immediate(body, 6, XR_RBC_LOADI, 2, 0);
    emit(body, 7, XR_RBC_EQ, 0, 2, 0);
    XR_CHECK(xr_rbc_jump(body + 8, 8, 15, 17), "deepest cell activation");
    immediate(body, 9, XR_RBC_LOADI, 2, 1);
    emit(body, 10, XR_RBC_SUB, 4, 0, 2);
    emit(body, 11, XR_RBC_MOV, 5, 1, 0);
    immediate(body, 12, XR_RBC_CALL, 3, 1);
    emit(body, 13, XR_RBC_RET, 3, 0, 0);
    emit(body, 14, XR_RBC_RET0, 0, 0, 0);
    emit(body, 15, panic ? XR_RBC_DIV : XR_RBC_CELL_READ, 0, panic ? 0 : 1, panic ? 2 : 0);
    emit(body, 16, XR_RBC_RET, 0, 0, 0);
    ids[15] = 1;
    XrRbcFunction functions[] = {{root, 7, NULL, 0, 6, 1, NULL, 0, 1},
        {body, 17, NULL, 0, 6, 2, ids, 17, 1}};
    XrRbcModule module = {functions, 2, NULL, 0, NULL, 0};
    XrtSourceLocation location = {"cells.xr", 8, 19, 2, 19, 7};
    XrtLocationOwner *owner = NULL;
    XR_CHECK(xrt_locations_create(&location, 1, &owner) == XRT_LOCATION_OK, "cell fault site");
    uint64_t argument = 128, bits = 999;
    XrRbcExecution *execution = NULL;
    bool prepare_fails = fault == CELL_INITIAL_FAILURE;
    bool growth_fails = fault == CELL_GROWTH_FAILURE;
    XrRbcStatus status = xr_rbc_module_prepare(&module, 0, &argument, 1, owner, &execution);
    if (prepare_fails) {
        XR_CHECK(status == XR_RBC_OUT_OF_MEMORY && !execution && fault == NO_FAILURE,
            "real initial cell allocation failed atomically");
    } else {
        XR_CHECK(status == XR_RBC_OK && execution, "cell execution is ready");
        XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
        XR_CHECK(outcome && !xr_rbc_execution_run(execution), "one owned cell outcome");
        XR_CHECK(xr_rbc_outcome_status(outcome) == (growth_fails ? XR_RBC_OUT_OF_MEMORY :
            panic ? XR_RBC_ARITHMETIC_PANIC : XR_RBC_OK), "cell outcome classification");
        xr_rbc_execution_free(&execution);
        if (!panic && !growth_fails)
            XR_CHECK(xr_rbc_outcome_bits(outcome, &bits) && bits == 136,
                "ancestor cell survives every relocated recursive bank");
        else XR_CHECK(!xr_rbc_outcome_bits(outcome, &bits) && bits == 999, "failure preserves bits");
        if (panic && !growth_fails) {
            XrtArithmeticPanic facts;
            const XrtLocationOwner *borrowed = NULL;
            XR_CHECK(xr_rbc_outcome_arithmetic_panic(outcome, &facts, &borrowed), "owned cell panic");
            XR_CHECK(facts.code == 420 && xrt_locations_get(borrowed, facts.source_id)->line == 19,
                "panic locations survive cell stack retirement");
        }
        xr_rbc_outcome_free(&outcome);
    }
    xrt_locations_release(owner);
    XR_CHECK(fault == NO_FAILURE, "requested cell fault reached");
    for (size_t i = 0; i < 4; ++i)
        XR_CHECK(!blocks[i].pointer && !blocks[i].size, "all moving buffers reclaimed");
}

static void string_admission_oom(void) {
    uint32_t code[5];
    XR_CHECK(xr_rbc_encode(&code[0],(XrRbcOperands){XR_RBC_STRING_LITERAL,0,0,0,0}),"String producer");
    XR_CHECK(xr_rbc_jump(&code[1],1,3,5),"forward String edge");
    XR_CHECK(xr_rbc_encode(&code[2],(XrRbcOperands){XR_RBC_RET0,0,0,0,0}),"unreachable return");
    XR_CHECK(xr_rbc_encode(&code[3],(XrRbcOperands){XR_RBC_STRING_DROP,0,0,0,0}),"owned String retirement");
    code[4]=code[2];
    XrRbcRegisterKind kind=XR_RBC_STRING;
    XrRbcFunction function={.code=code,.code_count=5,.register_count=1,.register_kinds=&kind,.register_kind_count=1};
    XrRbcLiteral literal={"s",1}; XrRbcModule module={.functions=&function,.function_count=1,.literals=&literal,.literal_count=1};
    XrtLocationOwner *owner=NULL; XR_CHECK(xrt_locations_create(NULL,0,&owner)==XRT_LOCATION_OK,"String image");
    for (unsigned i=0;i<3;++i) {
        fault=i==0?STRING_INDEX_FAILURE:i==1?STRING_JOIN_FAILURE:NO_FAILURE;
        XrRbcExecution *execution=NULL; XrRbcStatus status=xr_rbc_module_prepare(&module,0,NULL,0,owner,&execution);
        XR_CHECK(fault==NO_FAILURE,"representative String admission allocation reached");
        if(i<2) XR_CHECK(status==XR_RBC_OUT_OF_MEMORY&&!execution,"String verifier OOM publishes nothing");
        else {
            XR_CHECK(status==XR_RBC_OK&&execution,"String admission recovers after allocation failures");
            XrRbcOutcome *outcome=xr_rbc_execution_run(execution);XR_CHECK(outcome&&xr_rbc_outcome_status(outcome)==XR_RBC_OK,"same String graph executes");
            xr_rbc_execution_free(&execution);xr_rbc_outcome_free(&outcome);
        }
    }
    xrt_locations_release(owner);
    for(size_t i=0;i<4;++i)XR_CHECK(!blocks[i].pointer&&!blocks[i].size,"String execution buffers reclaimed");
}
static void spill_branch_admission_oom(void) {
    uint32_t code[7];
    immediate(code,0,XR_RBC_STRING_LITERAL,0,0);immediate(code,1,XR_RBC_SPILL,0,0);
    XR_CHECK(xr_rbc_jump(code+2,2,4,7),"forward spilled String edge");
    emit(code,3,XR_RBC_RET0,0,0,0);immediate(code,4,XR_RBC_RELOAD,0,0);
    emit(code,5,XR_RBC_STRING_DROP,0,0,0);emit(code,6,XR_RBC_RET0,0,0,0);
    XrRbcRegisterKind kind=XR_RBC_STRING;
    XrRbcFunction function={.code=code,.code_count=7,.register_count=1,.register_kinds=&kind,
        .register_kind_count=1,.spill_count=1,.reference_spill_count=1};
    XrRbcLiteral literal={"\xc3\xa9\0",3};
    XrRbcModule module={.functions=&function,.function_count=1,.literals=&literal,.literal_count=1};
    XrtLocationOwner *owner=NULL;
    XR_CHECK(xrt_locations_create(NULL,0,&owner)==XRT_LOCATION_OK,"spilled branch location owner");
    /* Two saved entries and the current state have distinct spill storage. */
    spill_state_failure_bytes=3*function.spill_count;
    for (unsigned i=0; i<2; ++i) {
        fault=i?NO_FAILURE:SPILL_STATE_FAILURE;
        XrRbcExecution *execution=NULL;
        XrRbcStatus status=xr_rbc_module_prepare(&module,0,NULL,0,owner,&execution);
        XR_CHECK(fault==NO_FAILURE,"actual spilled branch admission allocation reached");
        if (!i) XR_CHECK(status==XR_RBC_OUT_OF_MEMORY && !execution,"failed branch admission publishes nothing");
        else {
            XR_CHECK(status==XR_RBC_OK && execution,"identical spilled branch request recovers");
            XrRbcOutcome *outcome=xr_rbc_execution_run(execution);
            XR_CHECK(outcome && xr_rbc_outcome_status(outcome)==XR_RBC_OK,"spilled branch String retires");
            xr_rbc_execution_free(&execution);xr_rbc_outcome_free(&outcome);
        }
    }
    spill_state_failure_bytes=2;
    xrt_locations_release(owner);
    for (size_t i=0; i<4; ++i) XR_CHECK(!blocks[i].pointer && !blocks[i].size,"branch execution buffers reclaimed");
}
static void spill_growth(bool panic) {
    uint32_t caller[11],callee[5];
    immediate(caller,0,XR_RBC_LOADI,3,5);immediate(caller,1,XR_RBC_CELL_NEW,3,0);
    immediate(caller,2,XR_RBC_STRING_LITERAL,0,0);immediate(caller,3,XR_RBC_SPILL,0,0);
    immediate(caller,4,XR_RBC_CALL,4,1);emit(caller,5,XR_RBC_MOV,2,4,0);
    emit(caller,6,XR_RBC_CELL_READ,1,3,0);emit(caller,7,XR_RBC_ADD,2,2,1);
    immediate(caller,8,XR_RBC_RELOAD,0,0);emit(caller,9,XR_RBC_STRING_DROP,0,0,0);
    emit(caller,10,XR_RBC_RET,2,0,0);
    immediate(callee,0,XR_RBC_LOADI,0,panic?0:42);immediate(callee,1,XR_RBC_SPILL,0,31);
    immediate(callee,2,XR_RBC_RELOAD,0,31);
    emit(callee,3,panic?XR_RBC_DIV:XR_RBC_RET,0,0,0);emit(callee,4,XR_RBC_RET,0,0,0);
    XrRbcRegisterKind kinds[]={XR_RBC_STRING,XR_RBC_BITS,XR_RBC_BITS,XR_RBC_BITS,XR_RBC_CALL_WINDOW};
    XrRbcFunction functions[]={
        {.code=caller,.code_count=11,.register_count=5,.cell_count=1,.register_kinds=kinds,
         .register_kind_count=5,.spill_count=1,.reference_spill_count=1},
        {.code=callee,.code_count=panic?5u:4u,.register_count=1,.spill_count=32}};
    XrRbcLiteral literal={"live across spill relocation",28};
    XrRbcModule module={.functions=functions,.function_count=2,.literals=&literal,.literal_count=1};
    XrtLocationOwner *owner=NULL;XR_CHECK(xrt_locations_create(NULL,0,&owner)==XRT_LOCATION_OK,"spill location owner");
    unsigned expected_fault=fault;XrRbcExecution *execution=NULL;
    XrRbcStatus status=xr_rbc_module_prepare(&module,0,NULL,0,owner,&execution);
    if(expected_fault==SPILL_STATE_FAILURE || expected_fault==SPILL_INITIAL_FAILURE)
        XR_CHECK(status==XR_RBC_OUT_OF_MEMORY&&!execution&&fault==NO_FAILURE,"spill prepare first OOM preserved");
    else {
        if(status!=XR_RBC_OK)fprintf(stderr,"spill prepare fault=%u panic=%u status=%u\n",
            expected_fault,(unsigned)panic,(unsigned)status);
        XR_CHECK(status==XR_RBC_OK&&execution,"spill caller prepared");
        XrRbcOutcome *outcome=xr_rbc_execution_run(execution);uint64_t bits=99;
        XrRbcStatus expected=expected_fault==SPILL_GROWTH_FAILURE?XR_RBC_OUT_OF_MEMORY:
            panic?XR_RBC_ARITHMETIC_PANIC:XR_RBC_OK;
        XR_CHECK(outcome&&xr_rbc_outcome_status(outcome)==expected&&fault==NO_FAILURE,"spill result first cause");
        XR_CHECK(xr_rbc_outcome_bits(outcome,&bits)==(expected==XR_RBC_OK),"spill result status gates bits");
        XR_CHECK(bits==(expected==XR_RBC_OK?47u:99u),"caller cell and String survive moving spill storage");
        xr_rbc_execution_free(&execution);xr_rbc_outcome_free(&outcome);
    }
    xrt_locations_release(owner);
    for(size_t i=0;i<4;++i)XR_CHECK(!blocks[i].pointer&&!blocks[i].size,"all four moving buffers reclaimed");
}
typedef struct FrameObservation {
    XrRbcExecution *execution;
    XrtStringRoot strings;
    XrtExecutionFailure failure;
    XrRbcRegister result;
    XrRbcStatus status;
    bool before_drain;
} FrameObservation;
static void observe_frame_root(void *data) {
    FrameObservation *o=data;
    o->status=xr_rbc_execute(o->execution->module,&o->execution->stack,
        o->execution->pending->locations,&o->result);
    if(o->status!=XR_RBC_OK)return;
    if(o->execution->pending->result_kind==XR_RBC_STRING)
        o->result.string=xrt_string_root_take(o->result.string);
    XR_CHECK(!o->execution->stack.depth && !o->strings.owners,
        "actual normal Cell DROP precedes frame retirement and root drain");
    o->before_drain=true;
}
static void frame_cell_growth(unsigned requested_fault,bool panic) {
    uint32_t root[12],body[23];
    immediate(root,0,XR_RBC_STRING_LITERAL,0,0);emit(root,1,XR_RBC_STRING_CELL_NEW,1,0,0);
    emit(root,2,XR_RBC_STRING_DROP,0,0,0);immediate(root,3,XR_RBC_LOADI,4,40);
    emit(root,4,XR_RBC_MOV,5,1,0);immediate(root,5,XR_RBC_CALL,3,1);
    emit(root,6,XR_RBC_CELL_READ,2,1,0);emit(root,7,XR_RBC_STRING_LEN,4,2,0);
    emit(root,8,XR_RBC_STRING_DROP,2,0,0);emit(root,9,XR_RBC_ADD,3,3,4);
    emit(root,10,XR_RBC_STRING_CELL_DROP,1,0,0);emit(root,11,XR_RBC_RET,3,0,0);
    immediate(body,0,XR_RBC_STRING_LITERAL,2,1);emit(body,1,XR_RBC_STRING_CELL_NEW,3,2,0);
    emit(body,2,XR_RBC_STRING_DROP,2,0,0);immediate(body,3,XR_RBC_LOADI,4,1);
    immediate(body,4,XR_RBC_LOADI,6,0);emit(body,5,XR_RBC_EQ,0,6,0);
    XR_CHECK(xr_rbc_jump(body+6,6,12,23),"base return path");emit(body,7,XR_RBC_SUB,5,0,4);
    emit(body,8,XR_RBC_MOV,12,5,0);emit(body,9,XR_RBC_MOV,13,1,0);
    immediate(body,10,XR_RBC_CALL,11,1);XR_CHECK(xr_rbc_jump(body+11,11,13,23),"normal join");
    if(panic)emit(body,12,XR_RBC_DIV,11,0,0);else immediate(body,12,XR_RBC_LOADI,11,0);
    emit(body,13,XR_RBC_CELL_READ,7,1,0);emit(body,14,XR_RBC_STRING_LEN,9,7,0);
    emit(body,15,XR_RBC_STRING_DROP,7,0,0);emit(body,16,XR_RBC_CELL_READ,8,3,0);
    emit(body,17,XR_RBC_STRING_LEN,10,8,0);emit(body,18,XR_RBC_STRING_DROP,8,0,0);
    emit(body,19,XR_RBC_ADD,10,10,9);emit(body,20,XR_RBC_ADD,10,10,11);
    emit(body,21,XR_RBC_STRING_CELL_DROP,3,0,0);emit(body,22,XR_RBC_RET,10,0,0);
    XrRbcRegisterKind outer[]={XR_RBC_STRING,XR_RBC_STRING_CELL,XR_RBC_STRING,
        XR_RBC_CALL_WINDOW,XR_RBC_CALL_WINDOW,XR_RBC_CALL_WINDOW};
    XrRbcRegisterKind inner[]={XR_RBC_BITS,XR_RBC_STRING_CELL,XR_RBC_STRING,XR_RBC_STRING_CELL,
        XR_RBC_BITS,XR_RBC_BITS,XR_RBC_BITS,XR_RBC_STRING,XR_RBC_STRING,XR_RBC_BITS,XR_RBC_BITS,
        XR_RBC_CALL_WINDOW,XR_RBC_CALL_WINDOW,XR_RBC_CALL_WINDOW};
    uint32_t ids[23]={0};ids[12]=panic?1u:0u;
    XrRbcRegisterKind cell=XR_RBC_STRING;
    XrRbcFunction functions[]={
        {.code=root,.code_count=12,.register_count=6,.cell_count=1,.register_kinds=outer,
         .register_kind_count=6,.cell_kinds=&cell,.cell_kind_count=1},
        {.code=body,.code_count=23,.register_count=14,.parameter_count=2,.cell_count=1,
         .register_kinds=inner,.register_kind_count=14,.cell_kinds=&cell,.cell_kind_count=1,
         .source_ids=ids,.source_id_count=23}};
    XrRbcLiteral literals[]={{"ancestor",8},{"frame",5}};
    XrRbcModule module={.functions=functions,.function_count=2,.literals=literals,.literal_count=2};
    XrtLocationOwner *locations=NULL;XrtSourceLocation point={"frame-cells.xr",14,20,3,20,8};
    XR_CHECK(xrt_locations_create(&point,1,&locations)==XRT_LOCATION_OK,"frame failure location");
    FrameObservation o={.status=XR_RBC_INVALID};
    fault=requested_fault;XrRbcStatus prepared=xr_rbc_module_prepare(&module,0,NULL,0,locations,&o.execution);
    if(requested_fault==CELL_INITIAL_FAILURE) {
        XR_CHECK(prepared==XR_RBC_OUT_OF_MEMORY && !o.execution && fault==NO_FAILURE,"real Cell bank initial failure");
    } else {
        XR_CHECK(prepared==XR_RBC_OK && o.execution,"complete frame Cell graph prepared");
        observed_cell_drops=observed_cell_writes=0;observe_frame_drops=true;
        XrtExecutionStatus status=xrt_run_embedded_owned(locations,&o.failure,observe_frame_root,&o,&o.strings);
        observe_frame_drops=false;
        if(panic)XR_CHECK(status==XRT_EXECUTION_FAILURE && o.failure.kind==XRT_FAILURE_ARITHMETIC_PANIC &&
            o.failure.detail.panic.code==420 && o.failure.detail.panic.source_id==1 && !o.before_drain,
            "live Cell panic retains its first exact cause");
        else if(requested_fault!=NO_FAILURE)XR_CHECK(status==XRT_EXECUTION_COMPLETED && o.status==XR_RBC_OUT_OF_MEMORY &&
            !o.before_drain && fault==NO_FAILURE,"real moving bank OOM is not a normal guest return");
        else XR_CHECK(status==XRT_EXECUTION_COMPLETED && o.status==XR_RBC_OK && o.before_drain &&
            o.result.bits==541 && observed_cell_drops==42 && !observed_cell_writes,
            "42 actual Cell drops precede successful drain after moving banks");
        XR_CHECK(!o.strings.domain && !o.strings.owners,"failure or completed boundary reclaimed its root");
        xr_rbc_execution_free(&o.execution);
    }
    xrt_locations_release(locations);
    for(unsigned i=0;i<4;++i)XR_CHECK(!blocks[i].pointer&&!blocks[i].size,"all relocated banks reclaimed");
}
static void frame_cell_string_result(void) {
    uint32_t code[7];immediate(code,0,XR_RBC_STRING_LITERAL,0,0);emit(code,1,XR_RBC_STRING_CELL_NEW,1,0,0);
    emit(code,2,XR_RBC_STRING_DROP,0,0,0);XR_CHECK(xr_rbc_jump(code+3,3,4,7),"result forward edge");
    emit(code,4,XR_RBC_CELL_READ,0,1,0);emit(code,5,XR_RBC_STRING_CELL_DROP,1,0,0);emit(code,6,XR_RBC_RET,0,0,0);
    XrRbcRegisterKind kinds[]={XR_RBC_STRING,XR_RBC_STRING_CELL},cell=XR_RBC_STRING;
    XrRbcFunction function={.code=code,.code_count=7,.register_count=2,.cell_count=1,.register_kinds=kinds,
        .register_kind_count=2,.cell_kinds=&cell,.cell_kind_count=1,.result_kind=XR_RBC_STRING};
    XrRbcLiteral literal={"cell\0\xc3\xa9",7};XrRbcModule module={.functions=&function,.function_count=1,.literals=&literal,.literal_count=1};
    XrtLocationOwner *locations=NULL;XR_CHECK(xrt_locations_create(NULL,0,&locations)==XRT_LOCATION_OK,"result location");
    FrameObservation o={.status=XR_RBC_INVALID};fault=NO_FAILURE;
    XR_CHECK(xr_rbc_module_prepare(&module,0,NULL,0,locations,&o.execution)==XR_RBC_OK,"owned Cell result prepared");
    observed_cell_drops=0;observe_frame_drops=true;
    XR_CHECK(xrt_run_embedded_owned(locations,&o.failure,observe_frame_root,&o,&o.strings)==XRT_EXECUTION_COMPLETED,
        "owned result actually transfers before successful root drain");observe_frame_drops=false;
    XR_CHECK(o.before_drain && observed_cell_drops==1,"actual Cell drop did not consume the retained result");
    size_t length=0;const char *bytes=xrt_string_bytes(o.result.string,&length);
    XR_CHECK(length==7&&!memcmp(bytes,literal.bytes,7),"result survives its physical Cell and root");
    xrt_string_owned_drop(&o.result.string);xr_rbc_execution_free(&o.execution);xrt_locations_release(locations);
    for(unsigned i=0;i<4;++i)XR_CHECK(!blocks[i].pointer&&!blocks[i].size,"result banks retired");
}

static void spill_cycle_admission_oom(void) {
    uint32_t code[3];immediate(code,0,XR_RBC_LOADI,0,42);
    immediate(code,1,XR_RBC_SPILL,0,0);XR_CHECK(xr_rbc_jump(code+2,2,0,3),"cycle to actual entry");
    XrRbcFunction function={.code=code,.code_count=3,.register_count=1,.spill_count=1};
    XrRbcModule module={.functions=&function,.function_count=1};
    XrtLocationOwner *owner=NULL;
    XR_CHECK(xrt_locations_create(NULL,0,&owner)==XRT_LOCATION_OK,"cycle location owner");
    for(unsigned i=0;i<4;++i) {
        fault=i==0?STRING_INDEX_FAILURE:i==1?STRING_JOIN_FAILURE:i==2?SPILL_STATE_FAILURE:NO_FAILURE;
        XrRbcExecution *execution=NULL;
        XrRbcStatus status=xr_rbc_module_prepare(&module,0,NULL,0,owner,&execution);
        XR_CHECK(fault==NO_FAILURE,"actual graph allocation fault reached");
        if(i<3) XR_CHECK(status==XR_RBC_OUT_OF_MEMORY && !execution,"no Execution after graph OOM");
        else XR_CHECK(status==XR_RBC_OK && execution,"fresh cyclic graph admission recovers");
        // This is an infinite guest loop. Only prepare/free is intentional.
        xr_rbc_execution_free(&execution);
    }
    xrt_locations_release(owner);
    for(unsigned i=0;i<4;++i)XR_CHECK(!blocks[i].pointer && !blocks[i].size,"cycle banks reclaimed");
}

int main(void) {
    spill_cycle_admission_oom();
    frame_cell_growth(NO_FAILURE,false);
    frame_cell_growth(CELL_INITIAL_FAILURE,false);
    frame_cell_growth(CELL_GROWTH_FAILURE,false);
    frame_cell_growth(GROWTH_FAILURE,false);
    frame_cell_growth(NO_FAILURE,true);
    frame_cell_string_result();
    spill_branch_admission_oom();
    spill_growth(false);
    fault=SPILL_STATE_FAILURE;spill_growth(false);
    fault=SPILL_INITIAL_FAILURE;spill_growth(false);
    fault=SPILL_GROWTH_FAILURE;spill_growth(false);
    spill_growth(true);
    string_admission_oom();
    uint32_t code[11];
    XrRbcFunction function = make_countdown(code);
    XrRbcModule module = {&function, 1, NULL, 0, NULL, 0};
    XrtLocationOwner *owner = NULL;
    XR_CHECK(xrt_locations_create(NULL, 0, &owner) == XRT_LOCATION_OK, "real empty image");
    invoke(&module, owner, false, XR_RBC_OK);
    fault = OUTCOME_FAILURE;
    invoke(&module, owner, true, XR_RBC_OUT_OF_MEMORY);
    fault = INITIAL_STACK_FAILURE;
    invoke(&module, owner, true, XR_RBC_OUT_OF_MEMORY);
    fault = GROWTH_FAILURE;
    invoke(&module, owner, false, XR_RBC_OUT_OF_MEMORY);
    invoke(&module, owner, false, XR_RBC_OK);
    xrt_locations_release(owner);
    puts("register_vm_alloc: passed");
    panic_after_growth();
    cell_growth(false);
    fault = CELL_INITIAL_FAILURE;
    cell_growth(false);
    fault = CELL_GROWTH_FAILURE;
    cell_growth(false);
    cell_growth(false);
    cell_growth(true);
    return 0;
}
