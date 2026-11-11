/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_array_cases.h - Raw closed Array owners, cells and typed spills
 */
#ifndef REGISTER_ARRAY_CASES_H
#define REGISTER_ARRAY_CASES_H
typedef struct RawArray {
    XrRbcModule module;
    XrRbcFunction function;
    uint32_t code[64];
    XrRbcArrayAccess accesses[32];
    XrRbcArrayLocation operands[96];
    XrRbcRegisterKind kinds[8], cells[2], spill_kinds[2];
} RawArray;

static void raw_array_init(RawArray *f) {
    memset(f, 0, sizeof(*f));
    const XrRbcRegisterKind kinds[] = {XR_RBC_BITS, XR_RBC_BITS, XR_RBC_ARRAY_I64,
        XR_RBC_ARRAY_I64, XR_RBC_ARRAY_I64_CELL, XR_RBC_ARRAY_I64, XR_RBC_BITS};
    memcpy(f->kinds, kinds, sizeof(kinds));
    f->cells[0] = XR_RBC_ARRAY_I64;
    f->function = (XrRbcFunction){.code=f->code, .register_count=7, .register_kinds=f->kinds,
        .register_kind_count=7, .cell_count=1, .cell_kinds=f->cells, .cell_kind_count=1,
        .array_accesses=f->accesses, .array_operands=f->operands};
    f->module = (XrRbcModule){.functions=&f->function, .function_count=1};
}
static void raw_array_access(RawArray *f, XrRbcArrayAccess access, const XrRbcArrayLocation *args) {
    CHECK(f->function.code_count < 64 && f->function.array_access_count < 32 &&
        access.count <= 96 - f->function.array_operand_count);
    access.first = (uint32_t)f->function.array_operand_count;
    if (access.count) memcpy(f->operands + access.first, args, (size_t)access.count * sizeof(*args));
    f->function.array_operand_count += access.count;
    f->accesses[f->function.array_access_count] = access;
    immediate(f->code, f->function.code_count++, XR_RBC_ARRAY_ACCESS, 0,
        (int64_t)f->function.array_access_count++);
}
static XrRbcArrayAccess raw_array_op(XrRbcArrayOperation operation, uint32_t result, uint32_t count) {
    return (XrRbcArrayAccess){.operation=operation, .kind=XR_RBC_ARRAY_I64, .result={result, false}, .count=count};
}
static void raw_array_cow(RawArray *f) {
    raw_array_init(f);
    immediate(f->code, f->function.code_count++, XR_RBC_LOADI, 0, 7);
    immediate(f->code, f->function.code_count++, XR_RBC_LOADI, 1, 9);
    XrRbcArrayLocation args[] = {{0, false}, {1, false}, {1, false}};
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 2), args);
    args[0].index = 2;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_RETAIN, 3, 1), args);
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1), args);
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    args[0].index = 4;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_PUSH, 0, 2), args);
    immediate(f->code, f->function.code_count++, XR_RBC_LOADI, 0, 0);
    args[1].index = 0;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_SET, 0, 3), args);
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_CELL_READ, 5, 1), args);
    args[0].index = 3;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_GET, 6, 2), args);
    args[0].index = 5;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_GET, 1, 2), args);
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_LEN, 0, 1), args);
    emit(f->code, f->function.code_count++, XR_RBC_ADD, 6, 6, 1);
    emit(f->code, f->function.code_count++, XR_RBC_ADD, 6, 6, 0);
    args[0].index = 3;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    args[0].index = 5;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    args[0].index = 4;
    raw_array_access(f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), args);
    emit(f->code, f->function.code_count++, XR_RBC_RET, 6, 0, 0);
}
static void test_raw_arrays(void) {
    RawArray f;
    raw_array_cow(&f);
    uint64_t result = 0;
    CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == 19);
    f.accesses[0].kind = XR_RBC_ARRAY_STRING;
    reject_prepare(&f.module, 0, NULL, 0);
    f.accesses[0].kind = XR_RBC_ARRAY_I64;
    f.operands[0].index = 7;
    reject_prepare(&f.module, 0, NULL, 0);
    f.operands[0].index = 0;
    f.operands[0].spill = true;
    reject_prepare(&f.module, 0, NULL, 0);
    f.operands[0].spill = false;
    uint32_t code = f.code[2];
    immediate(f.code, 2, XR_RBC_LOADI, 2, 7);
    reject_prepare(&f.module, 0, NULL, 0);
    f.code[2] = code;
    f.accesses[3].operation = XR_RBC_ARRAY_RETAIN;
    f.accesses[3].result.index = 3;
    reject_prepare(&f.module, 0, NULL, 0);
}
static void test_raw_array_spills(void) {
    RawArray f;
    raw_array_init(&f);
    f.function.spill_count = f.function.reference_spill_count = 1;
    f.function.reference_spill_kinds = f.spill_kinds;
    f.function.reference_spill_kind_count = 1;
    f.spill_kinds[0] = XR_RBC_ARRAY_I64;
    immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 0, 17);
    XrRbcArrayAccess access = raw_array_op(XR_RBC_ARRAY_NEW, 0, 1);
    access.result.spill = true;
    XrRbcArrayLocation args[] = {{0, false}};
    raw_array_access(&f, access, args);
    immediate(f.code, f.function.code_count++, XR_RBC_RELOAD, 2, 0);
    immediate(f.code, f.function.code_count++, XR_RBC_SPILL, 2, 0);
    args[0] = (XrRbcArrayLocation){0, true};
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_LEN, 1, 1), args);
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    emit(f.code, f.function.code_count++, XR_RBC_RET, 1, 0, 0);
    uint64_t result = 0;
    CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == 1);
    f.spill_kinds[0] = XR_RBC_STRING;
    reject_prepare(&f.module, 0, NULL, 0);
    f.spill_kinds[0] = XR_RBC_ARRAY_STRING;
    reject_prepare(&f.module, 0, NULL, 0);
}

typedef struct RawArrayObservation { XrtArray *array; int64_t sum; } RawArrayObservation;
static void raw_array_observe(void *context) {
    RawArrayObservation *probe = context;
    CHECK(xrt_array_length(probe->array, 0) == 300);
    for (int64_t i = 0; i < 300; ++i) probe->sum += xrt_array_i64_get(probe->array, i, 0);
    XrtArray *copy = xrt_array_retain(probe->array, 0);
    xrt_array_i64_push(&copy, 999, 0);
    CHECK(xrt_array_length(copy, 0) == 301 && xrt_array_length(probe->array, 0) == 300);
    xrt_array_drop(&copy);
}
static void test_raw_array_wide_return(void) {
    RawArray f;
    raw_array_init(&f);
    uint32_t code[605], cursor = 0;
    XrRbcArrayLocation operands[300];
    for (uint32_t i = 0; i < 300; ++i) {
        immediate(code, cursor++, XR_RBC_LOADI, 0, i);
        immediate(code, cursor++, XR_RBC_SPILL, 0, i + 1);
        operands[i] = (XrRbcArrayLocation){i + 1, true};
    }
    XrRbcArrayAccess constructor = raw_array_op(XR_RBC_ARRAY_NEW, 2, 300);
    immediate(code, cursor++, XR_RBC_ARRAY_ACCESS, 0, 0);
    immediate(code, cursor++, XR_RBC_SPILL, 2, 0);
    immediate(code, cursor++, XR_RBC_RELOAD, 3, 0);
    emit(code, cursor++, XR_RBC_RET, 3, 0, 0);
    f.function.code = code; f.function.code_count = cursor;
    f.function.result_kind = XR_RBC_ARRAY_I64;
    f.function.spill_count = 301; f.function.reference_spill_count = 1;
    f.spill_kinds[0] = XR_RBC_ARRAY_I64;
    f.function.reference_spill_kinds = f.spill_kinds; f.function.reference_spill_kind_count = 1;
    f.function.array_accesses = &constructor; f.function.array_access_count = 1;
    f.function.array_operands = operands; f.function.array_operand_count = 300;
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL, 0, &locations) == XRT_LOCATION_OK);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&f.module, 0, NULL, 0, locations, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution);
    uint64_t bits = 91;
    const XrtArray *borrowed = NULL;
    RawArrayObservation probe = {0};
    CHECK(!xr_rbc_outcome_bits(outcome, &bits) && bits == 91);
    CHECK(xr_rbc_outcome_array(outcome, &borrowed) && borrowed);
    CHECK(xr_rbc_outcome_take_array(outcome, &probe.array) && probe.array == borrowed);
    xr_rbc_outcome_free(&outcome); xrt_locations_release(locations);
    XrtSourceMap map = {0};
    xrt_run_standalone(&map, raw_array_observe, &probe);
    CHECK(probe.sum == 44850);
    xrt_array_owned_drop(&probe.array);
    CHECK(!probe.array);
}
static void test_raw_array_local_cells(void) {
    RawArray f;
    raw_array_init(&f);
    f.function.parameter_count = 1;
    f.function.cell_count = f.function.cell_kind_count = 2;
    f.cells[1] = XR_RBC_ARRAY_I64;
    immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 1, 7);
    XrRbcArrayLocation arg = {1, false};
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 1), &arg);
    immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 6, 0);
    emit(f.code, f.function.code_count++, XR_RBC_EQ, 0, 6, 0);
    size_t branch = f.function.code_count++;
    arg.index = 2;
    XrRbcArrayAccess cell = raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1);
    cell.cell = 1;
    raw_array_access(&f, cell, &arg);
    arg.index = 4;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), &arg);
    size_t jump = f.function.code_count++, target = f.function.code_count;
    arg.index = 2; cell.cell = 0;
    raw_array_access(&f, cell, &arg);
    arg.index = 4;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), &arg);
    size_t join = f.function.code_count;
    arg.index = 2;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), &arg);
    emit(f.code, f.function.code_count++, XR_RBC_RET, 1, 0, 0);
    CHECK(xr_rbc_jump(f.code + branch, branch, target, f.function.code_count));
    CHECK(xr_rbc_jump(f.code + jump, jump, join, f.function.code_count));
    for (uint64_t choice = 0; choice < 2; ++choice) {
        uint64_t result = 0;
        CHECK(execute(&f.module, 0, &choice, 1, &result) == XR_RBC_OK && result == 7);
    }
    raw_array_init(&f);
    f.function.parameter_count = 1;
    immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 1, 3);
    arg.index = 0;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 1), &arg);
    arg.index = 2;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1), &arg);
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), &arg);
    arg.index = 4;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), &arg);
    CHECK(xr_rbc_encode(f.code + f.function.code_count++, (XrRbcOperands){XR_RBC_ADDI, 0, 0, 0, 1}));
    emit(f.code, f.function.code_count++, XR_RBC_LT, 0, 1, 0);
    jump = f.function.code_count++;
    emit(f.code, f.function.code_count++, XR_RBC_RET, 0, 0, 0);
    CHECK(xr_rbc_jump(f.code + jump, jump, 1, f.function.code_count));
    uint64_t choice = 0, result = 0;
    CHECK(execute(&f.module, 0, &choice, 1, &result) == XR_RBC_OK && result == 3);
}
static void raw_string_access(RawArray *f, XrRbcArrayAccess access, const XrRbcArrayLocation *args) {
    access.kind = XR_RBC_ARRAY_STRING;
    raw_array_access(f, access, args);
}
static void raw_array_string_observe(void *context) {
    RawArrayObservation *probe = context;
    CHECK(xrt_array_length(probe->array, 0) == 2);
    for (int64_t i = 0; i < 2; ++i) {
        XrtString *string = xrt_array_string_get(probe->array, i, 0);
        probe->sum += xrt_string_length(string, 0);
        xrt_string_drop(&string);
    }
}
static void test_raw_array_string_state(void) {
    RawArray f;
    raw_array_init(&f);
    XrtModuleTypeSpec type = {XRT_MODULE_ARRAY_STRING};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {0, 0, XRT_MODULE_SLOT_MUTABLE, 0, sizeof(void *), _Alignof(void *)};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=sizeof(void *), .payload_alignment=_Alignof(void *), .byte_limit=UINT64_MAX};
    XrtModuleLayout *layout = NULL;
    CHECK(xrt_module_layout_new(&spec, &layout) == 0);
    XrRbcRegisterKind kinds[] = {XR_RBC_STATE, XR_RBC_ARRAY_STRING, XR_RBC_STRING, XR_RBC_STATE_REF,
        XR_RBC_BITS, XR_RBC_ARRAY_STRING_CELL, XR_RBC_ARRAY_STRING};
    memcpy(f.kinds, kinds, sizeof(kinds));
    f.function.parameter_count = 1; f.function.result_kind = XR_RBC_ARRAY_STRING;
    f.cells[0] = XR_RBC_ARRAY_STRING;
    XrRbcStateAccess accesses[] = {{0, 0, 1}, {0}};
    f.function.state_accesses = accesses; f.function.state_access_count = 2;
    XrRbcLiteral literals[] = {{"ab\0cd", 5}, {"new!", 4}};
    f.module.literals = literals; f.module.literal_count = 2; f.module.state_layout = layout;
    immediate(f.code, f.function.code_count++, XR_RBC_MODULE_BEGIN, 0, 0);
    immediate(f.code, f.function.code_count++, XR_RBC_STATE_REFERENCE, 3, 1);
    immediate(f.code, f.function.code_count++, XR_RBC_STRING_LITERAL, 2, 0);
    XrRbcArrayLocation args[] = {{2, false}, {4, false}, {2, false}};
    raw_string_access(&f, raw_array_op(XR_RBC_ARRAY_NEW, 1, 1), args);
    immediate(f.code, f.function.code_count++, XR_RBC_STATE_CONSTRUCT, 3, 0);
    emit(f.code, f.function.code_count++, XR_RBC_STATE_PUBLISH, 3, 0, 0);
    args[0].index = 1;
    raw_string_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    emit(f.code, f.function.code_count++, XR_RBC_STRING_DROP, 2, 0, 0);
    immediate(f.code, f.function.code_count++, XR_RBC_MODULE_READY, 0, 0);
    immediate(f.code, f.function.code_count++, XR_RBC_STATE_REFERENCE, 3, 1);
    size_t read = f.function.code_count;
    emit(f.code, f.function.code_count++, XR_RBC_STATE_READ, 1, 3, XRT_MODULE_ARRAY_STRING);
    raw_string_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 5, 1), args);
    immediate(f.code, f.function.code_count++, XR_RBC_STRING_LITERAL, 2, 1);
    args[0].index = 5; args[1].index = 2;
    raw_string_access(&f, raw_array_op(XR_RBC_ARRAY_PUSH, 0, 2), args);
    immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 4, 0);
    args[1].index = 4;
    raw_string_access(&f, raw_array_op(XR_RBC_ARRAY_SET, 0, 3), args);
    emit(f.code, f.function.code_count++, XR_RBC_STRING_DROP, 2, 0, 0);
    raw_string_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_READ, 6, 1), args);
    emit(f.code, f.function.code_count++, XR_RBC_STATE_WRITE, 3, 6, XRT_MODULE_ARRAY_STRING);
    raw_string_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), args);
    args[0].index = 1;
    raw_string_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    emit(f.code, f.function.code_count++, XR_RBC_RET, 6, 0, 0);
    uint32_t saved = f.code[read];
    emit(f.code, read, XR_RBC_STATE_READ, 1, 3, XRT_MODULE_ARRAY_I64);
    reject_prepare(&f.module, 0, NULL, 0);
    f.code[read] = saved;
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL, 0, &locations) == XRT_LOCATION_OK);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_module_prepare(&f.module, 0, NULL, 0, locations, &execution) == XR_RBC_OK);
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    RawArrayObservation probe = {0};
    CHECK(xr_rbc_outcome_take_array(outcome, &probe.array));
    xr_rbc_outcome_free(&outcome); xr_rbc_execution_free(&execution);
    xrt_locations_release(locations); xrt_module_layout_drop(&layout);
    XrtSourceMap map = {0};
    xrt_run_standalone(&map, raw_array_string_observe, &probe);
    CHECK(probe.sum == 8);
    xrt_array_owned_drop(&probe.array);
}
static void test_raw_array_backedges(void) {
    for(unsigned spill=0;spill<2;++spill) {
        RawArray f;raw_array_init(&f);f.function.parameter_count=1;
        XrRbcArrayLocation arg={0,false};
        raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_NEW,2,1),&arg);
        arg.index=2;
        if(spill) {
            f.function.spill_count=f.function.reference_spill_count=1;
            f.spill_kinds[0]=XR_RBC_ARRAY_I64;
            f.function.reference_spill_kinds=f.spill_kinds;f.function.reference_spill_kind_count=1;
            immediate(f.code,f.function.code_count++,XR_RBC_SPILL,2,0);arg=(XrRbcArrayLocation){0,true};
        }
        size_t body=f.function.code_count;
        raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_LEN,6,1),&arg);
        CHECK(xr_rbc_encode(f.code+f.function.code_count++,(XrRbcOperands){XR_RBC_ADDI,0,0,0,1}));
        immediate(f.code,f.function.code_count++,XR_RBC_LTI,0,3);
        size_t jump=f.function.code_count++;
        size_t drop=f.function.code_count;
        raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_DROP,0,1),&arg);
        emit(f.code,f.function.code_count++,XR_RBC_RET,6,0,0);
        CHECK(xr_rbc_jump(f.code+jump,jump,body,f.function.code_count));
        uint64_t initial=0,result=0;
        CHECK(execute(&f.module,0,&initial,1,&result)==XR_RBC_OK && result==1);
        uint32_t saved=f.code[0];immediate(f.code,0,XR_RBC_LOADI,6,0);
        reject_prepare(&f.module,0,&initial,1);f.code[0]=saved;
        saved=f.code[drop];immediate(f.code,drop,XR_RBC_LOADI,6,1);
        reject_prepare(&f.module,0,&initial,1);f.code[drop]=saved;
        CHECK(xr_rbc_jump(f.code+jump,jump,0,f.function.code_count));
        reject_prepare(&f.module,0,&initial,1);
    }
    RawArray f;raw_array_init(&f);
    f.function.register_count=f.function.register_kind_count=5;
    f.kinds[3]=f.kinds[4]=XR_RBC_CALL_WINDOW;
    immediate(f.code,f.function.code_count++,XR_RBC_LOADI,0,7);
    XrRbcArrayLocation arg={0,false};raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_NEW,2,1),&arg);
    emit(f.code,f.function.code_count++,XR_RBC_MOV,4,2,0);
    immediate(f.code,f.function.code_count++,XR_RBC_CALL,3,1);
    arg.index=2;raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_DROP,0,1),&arg);
    emit(f.code,f.function.code_count++,XR_RBC_RET,3,0,0);
    uint32_t code[6];immediate(code,0,XR_RBC_LOADI,1,0);immediate(code,1,XR_RBC_ARRAY_ACCESS,0,0);
    CHECK(xr_rbc_encode(code+2,(XrRbcOperands){XR_RBC_ADDI,1,1,0,1}));
    immediate(code,3,XR_RBC_LTI,1,3);CHECK(xr_rbc_jump(code+4,4,1,6));emit(code,5,XR_RBC_RET,2,0,0);
    XrRbcRegisterKind kinds[]={XR_RBC_ARRAY_I64,XR_RBC_BITS,XR_RBC_BITS};
    XrRbcArrayAccess length=raw_array_op(XR_RBC_ARRAY_LEN,2,1);arg.index=0;
    XrRbcFunction functions[]={f.function,{.code=code,.code_count=6,.register_count=3,.parameter_count=1,
        .register_kinds=kinds,.register_kind_count=3,.array_accesses=&length,.array_access_count=1,
        .array_operands=&arg,.array_operand_count=1}};
    f.module.functions=functions;f.module.function_count=2;
    uint64_t result=0;CHECK(execute(&f.module,0,NULL,0,&result)==XR_RBC_OK && result==1);
}
static void test_raw_array_cell_identity(void) {
    for(unsigned retire=0;retire<2;++retire) {
        RawArray f;raw_array_init(&f);
        f.kinds[5]=XR_RBC_ARRAY_I64_CELL;f.kinds[6]=XR_RBC_ARRAY_I64;
        immediate(f.code,f.function.code_count++,XR_RBC_LOADI,0,10);
        immediate(f.code,f.function.code_count++,XR_RBC_LOADI,1,20);
        XrRbcArrayLocation args[]={{0,false},{1,false}};
        raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_NEW,2,1),args);
        args[0].index=1;raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_NEW,3,1),args);
        args[0].index=2;raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_CELL_NEW,4,1),args);
        if(retire) {
            args[0].index=4;raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_CELL_DROP,0,1),args);
        }
        args[0].index=3;raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_CELL_NEW,5,1),args);
        args[0].index=retire?5u:4u;raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_CELL_READ,6,1),args);
        immediate(f.code,f.function.code_count++,XR_RBC_LOADI,1,0);
        args[0].index=6;raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_GET,0,2),args);
        const uint32_t arrays[]={2,3,6};
        for(size_t a=0;a<sizeof(arrays)/sizeof(*arrays);++a) {
            args[0].index=arrays[a];raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_DROP,0,1),args);
        }
        args[0].index=retire?5u:4u;raw_array_access(&f,raw_array_op(XR_RBC_ARRAY_CELL_DROP,0,1),args);
        emit(f.code,f.function.code_count++,XR_RBC_RET,0,0,0);
        if(retire) {
            uint64_t result=0;CHECK(execute(&f.module,0,NULL,0,&result)==XR_RBC_OK && result==20);
        } else reject_prepare(&f.module,0,NULL,0);
    }
}
static void test_raw_array_cell_moves(void) {
    for (unsigned string = 0; string < 2; ++string) {
        RawArray f;
        raw_array_init(&f);
        XrRbcLiteral literals[] = {{"old", 3}, {"next", 4}};
        void (*access)(RawArray *, XrRbcArrayAccess, const XrRbcArrayLocation *) =
            string ? raw_string_access : raw_array_access;
        if (string) {
            f.kinds[0] = XR_RBC_STRING;
            f.kinds[2] = f.kinds[3] = f.kinds[5] = XR_RBC_ARRAY_STRING;
            f.kinds[4] = XR_RBC_ARRAY_STRING_CELL;
            f.cells[0] = XR_RBC_ARRAY_STRING;
            f.module.literals = literals;
            f.module.literal_count = 2;
            immediate(f.code, f.function.code_count++, XR_RBC_STRING_LITERAL, 0, 0);
        } else immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 0, 7);
        XrRbcArrayLocation args[] = {{0, false}, {1, false}, {0, false}};
        access(&f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 1), args);
        if (string) emit(f.code, f.function.code_count++, XR_RBC_STRING_DROP, 0, 0, 0);
        args[0].index = 2;
        access(&f, raw_array_op(XR_RBC_ARRAY_RETAIN, 3, 1), args);
        access(&f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1), args);
        access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
        immediate(f.code, f.function.code_count++, string ? XR_RBC_STRING_LITERAL : XR_RBC_LOADI, 0, string ? 1 : 9);
        immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 1, 0);
        args[0].index = 4;
        access(&f, raw_array_op(XR_RBC_ARRAY_SET, 0, 3), args);
        if (string) emit(f.code, f.function.code_count++, XR_RBC_STRING_DROP, 0, 0, 0);
        access(&f, raw_array_op(XR_RBC_ARRAY_CELL_READ, 5, 1), args);
        access(&f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), args);
        args[0].index = 3;
        access(&f, raw_array_op(XR_RBC_ARRAY_GET, string ? 0 : 6, 2), args);
        if (string) {
            emit(f.code, f.function.code_count++, XR_RBC_STRING_LEN, 6, 0, 0);
            emit(f.code, f.function.code_count++, XR_RBC_STRING_DROP, 0, 0, 0);
        }
        args[0].index = 5;
        access(&f, raw_array_op(XR_RBC_ARRAY_GET, 0, 2), args);
        if (string) {
            emit(f.code, f.function.code_count++, XR_RBC_STRING_LEN, 1, 0, 0);
            emit(f.code, f.function.code_count++, XR_RBC_STRING_DROP, 0, 0, 0);
        }
        emit(f.code, f.function.code_count++, XR_RBC_ADD, 6, 6, string ? 1 : 0);
        args[0].index = 3;
        access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
        args[0].index = 5;
        access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
        emit(f.code, f.function.code_count++, XR_RBC_RET, 6, 0, 0);
        uint64_t result = 0;
        CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == (string ? 7u : 16u));
    }
}

static void test_raw_array_cell_move_entry(void) {
    RawArray f;
    raw_array_init(&f);
    f.function.parameter_count = 1;
    immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 1, 7);
    XrRbcArrayLocation arg = {1, false};
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 1), &arg);
    immediate(f.code, f.function.code_count++, XR_RBC_EQI, 0, 0);
    size_t alternative_jump = f.function.code_count++;
    arg.index = 2;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1), &arg);
    size_t drop = f.function.code_count;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), &arg);
    arg.index = 4;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_READ, 5, 1), &arg);
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), &arg);
    arg.index = 5;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_LEN, 6, 1), &arg);
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), &arg);
    emit(f.code, f.function.code_count++, XR_RBC_RET, 6, 0, 0);
    size_t alternative = f.function.code_count;
    arg.index = 2;
    raw_array_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1), &arg);
    size_t back = f.function.code_count++;
    CHECK(xr_rbc_jump(f.code + alternative_jump, alternative_jump, alternative, f.function.code_count));
    CHECK(xr_rbc_jump(f.code + back, back, drop, f.function.code_count));
    for (uint64_t choose = 0; choose < 2; ++choose) {
        uint64_t result = 0;
        CHECK(execute(&f.module, 0, &choose, 1, &result) == XR_RBC_OK && result == 1);
    }
}
#endif // REGISTER_ARRAY_CASES_H
