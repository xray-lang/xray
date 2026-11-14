/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_boxed_cases.h - External canonical owners require complete flow checks
 */
#include "runtime/native/xnative_callable.h"

typedef struct RawBoxed {
    XrRbcModule module;
    XrRbcFunction function;
    uint32_t code[16], exact[4];
    XrRbcRegisterKind kinds[4];
    XrRbcAggregateAccess accesses[8];
    XrRbcArrayLocation operands[8];
} RawBoxed;

static void raw_boxed_access(RawBoxed *f, XrRbcAggregateOperation operation,
    uint32_t source, uint32_t result) {
    uint32_t row = (uint32_t)f->function.aggregate_access_count++;
    f->operands[row] = (XrRbcArrayLocation){source, false};
    f->accesses[row] = (XrRbcAggregateAccess){.operation=operation, .result={result, false},
        .first=row, .count=1, .slot=XR_XIR_CONSTRUCTED_TYPE_BASE + XRT_CALLABLE_PREFIX_COUNT};
    ++f->function.aggregate_operand_count;
    immediate(f->code, f->function.code_count++, XR_RBC_AGGREGATE_ACCESS, 0, row);
}
static void raw_boxed_init(RawBoxed *f, XrXirTypeArena *arena) {
    memset(f, 0, sizeof(*f));
    f->kinds[0] = f->kinds[3] = XR_RBC_BITS;
    f->kinds[1] = f->kinds[2] = XR_RBC_BOXED;
    f->exact[1] = f->exact[2] = XR_XIR_CONSTRUCTED_TYPE_BASE + XRT_CALLABLE_PREFIX_COUNT;
    f->function = (XrRbcFunction){.code=f->code, .register_count=4,
        .register_kinds=f->kinds, .register_kind_count=4, .register_boxed_types=f->exact,
        .register_boxed_type_count=4, .aggregate_accesses=f->accesses, .aggregate_operands=f->operands};
    f->module = (XrRbcModule){.functions=&f->function, .function_count=1, .closure_types=arena};
    immediate(f->code, f->function.code_count++, XR_RBC_LOADI, 0, 7);
    raw_boxed_access(f, XR_RBC_BOXED_NEW, 0, 1);
    raw_boxed_access(f, XR_RBC_BOXED_RETAIN, 1, 2);
    raw_boxed_access(f, XR_RBC_BOXED_DROP, 1, 0);
    raw_boxed_access(f, XR_RBC_BOXED_FIELD, 2, 3);
    raw_boxed_access(f, XR_RBC_BOXED_DROP, 2, 0);
    emit(f->code, f->function.code_count++, XR_RBC_RET, 3, 0, 0);
}
static void test_raw_boxed(void) {
    XrtCallableParameterSpec field = {.value={XRT_CALLABLE_SCALAR, XR_POD_I64}, .name="score", .name_length=5};
    XrtCallableTypeSpec type = {.kind=XRT_CALLABLE_RECORD, .parameters=&field, .parameter_count=1};
    XrXirTypeArena *arena = NULL;
    CHECK(xrt_callable_types_new(&type, 1, UINT64_MAX, &arena) == XR_XIR_VALUE_OK);
    RawBoxed f;
    raw_boxed_init(&f, arena);
    uint64_t result = 0;
    CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == 7);
    /* These functions contain no String or State, so boxed flow itself must
     * activate the ownership verifier. MOV only creates a synchronous borrow. */
    emit(f.code, 2, XR_RBC_MOV, 2, 1, 0);
    reject_prepare(&f.module, 0, NULL, 0);
    raw_boxed_init(&f, arena);
    f.operands[3].index = 1;
    reject_prepare(&f.module, 0, NULL, 0);
    raw_boxed_init(&f, arena);
    f.operands[4].index = 1;
    reject_prepare(&f.module, 0, NULL, 0);
    raw_boxed_init(&f, arena);
    emit(f.code, 5, XR_RBC_RET, 3, 0, 0);
    reject_prepare(&f.module, 0, NULL, 0);
    raw_boxed_init(&f, arena);
    f.exact[2]++;
    reject_prepare(&f.module, 0, NULL, 0);
    raw_boxed_init(&f, arena);
    XrRbcRegisterKind spill_kind = XR_RBC_BOXED;
    f.function.spill_count = f.function.reference_spill_count = 1;
    f.function.reference_spill_kinds = &spill_kind;
    f.function.reference_spill_kind_count = 1;
    reject_prepare(&f.module, 0, NULL, 0);
    raw_boxed_init(&f, arena);
    f.function.code_count = 3;
    emit(f.code, 2, XR_RBC_RET, 1, 0, 0);
    f.function.result_kind = XR_RBC_BOXED;
    f.function.result_boxed_type = f.exact[1];
    /* Internal boxed returns have an owner; the external scalar outcome does
     * not grant one and therefore cannot expose such an entry. */
    reject_prepare(&f.module, 0, NULL, 0);
    xr_xir_compile_type_arena_drop(arena);
}
