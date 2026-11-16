/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void test_sparse_cell_ordinals(void) {
    uint32_t code[8];
    CHECK(xr_rbc_jump(code,0,3,8));
    immediate(code,1,XR_RBC_LOADI,0,99);
    immediate(code,2,XR_RBC_CELL_NEW,0,0);
    immediate(code,3,XR_RBC_LOADI,1,42);
    immediate(code,4,XR_RBC_CELL_NEW,1,1);
    immediate(code,5,XR_RBC_LOADI,0,1);
    emit(code,6,XR_RBC_CELL_READ,2,1,0);
    emit(code,7,XR_RBC_RET,2,0,0);
    XrRbcFunction function={.code=code,.code_count=8,.register_count=3,.cell_count=2};
    CHECK(run_function(function,NULL,0)==42);
    CHECK(run_function(function,NULL,0)==42);
    XrRbcModule module={.functions=&function,.function_count=1};
    uint64_t bits=999;
    // Ordinal zero has a real static producer, but this activation skipped it.
    // A forged numeric handle must not borrow the later constructor's prefix.
    emit(code,6,XR_RBC_CELL_READ,2,0,0);
    CHECK(execute(&module,0,NULL,0,&bits)==XR_RBC_INVALID && bits==999);
    emit(code,6,XR_RBC_CELL_READ,2,1,0);
    immediate(code,4,XR_RBC_CELL_NEW,1,0);
    reject_prepare(&module,0,NULL,0);
    immediate(code,4,XR_RBC_CELL_NEW,1,1);
    CHECK(xr_rbc_jump(code,0,1,8));
    CHECK(xr_rbc_jump(code+6,6,1,8));
    reject_prepare(&module,0,NULL,0);
    uint32_t caller[6],callee[5];
    immediate(caller,0,XR_RBC_LOADI,0,40); immediate(caller,1,XR_RBC_CELL_NEW,0,0);
    emit(caller,2,XR_RBC_MOV,2,0,0); immediate(caller,3,XR_RBC_CALL,1,1);
    emit(caller,4,XR_RBC_CELL_READ,0,2,0); emit(caller,5,XR_RBC_RET,0,0,0);
    emit(callee,0,XR_RBC_CELL_READ,1,0,0); immediate(callee,1,XR_RBC_LOADI,2,1);
    emit(callee,2,XR_RBC_ADD,1,1,2); emit(callee,3,XR_RBC_CELL_WRITE,0,1,0);
    emit(callee,4,XR_RBC_RET,1,0,0);
    XrRbcFunction functions[]={{.code=caller,.code_count=6,.register_count=3,.cell_count=1},
        {.code=callee,.code_count=5,.register_count=3,.parameter_count=1}};
    module=(XrRbcModule){.functions=functions,.function_count=2};
    CHECK(execute(&module,0,NULL,0,&bits)==XR_RBC_OK && bits==41);
}
