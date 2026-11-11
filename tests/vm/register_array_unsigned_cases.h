/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_array_unsigned_cases.h - Typed Array edges reject unnormalized byte bits
 */
#ifndef REGISTER_ARRAY_UNSIGNED_CASES_H
#define REGISTER_ARRAY_UNSIGNED_CASES_H
static void raw_unsigned_kinds(RawArray *f, bool wide) {
    f->kinds[2] = f->kinds[3] = f->kinds[5] = wide ? XR_RBC_ARRAY_U64 : XR_RBC_ARRAY_U8;
    f->kinds[4] = wide ? XR_RBC_ARRAY_U64_CELL : XR_RBC_ARRAY_U8_CELL;
    f->cells[0] = f->kinds[2];
    for (uint32_t i = 0; i < f->function.array_access_count; ++i) f->accesses[i].kind = f->kinds[2];
}
static void raw_unsigned_access(RawArray *f, XrRbcArrayAccess op, const XrRbcArrayLocation *args) {
    op.kind = f->kinds[2];
    raw_array_access(f, op, args);
}
static void test_raw_unsigned_arrays(void) {
    for (unsigned wide = 0; wide < 2; ++wide) {
        RawArray f;
        raw_array_cow(&f);
        raw_unsigned_kinds(&f, wide != 0);
        uint64_t result = 0;
        CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == 19);
    }
    for (uint32_t operation = 0; operation < 4; ++operation) {
        RawArray f;
        raw_array_init(&f);
        raw_unsigned_kinds(&f, false);
        f.function.parameter_count = 1;
        immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 1, operation == 1 ? 1 : 0);
        XrRbcArrayLocation args[] = {{0, false}, {1, false}, {0, false}};
        if (operation < 2) {
            if (operation) { args[0].index = 1; args[1].index = 0; }
            raw_unsigned_access(&f, raw_array_op(operation ? XR_RBC_ARRAY_REPEAT : XR_RBC_ARRAY_NEW,
                2, operation ? 2 : 1), args);
            args[0].index = 2;
        } else {
            args[0].index = 1;
            raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 1), args);
            args[0].index = 2;
            raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_NEW, 4, 1), args);
            raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
            args[0].index = 4; args[1].index = operation == 2 ? 1 : 0;
            raw_unsigned_access(&f, raw_array_op(operation == 2 ? XR_RBC_ARRAY_SET : XR_RBC_ARRAY_PUSH,
                0, operation == 2 ? 3 : 2), args);
            raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_READ, 5, 1), args);
            raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_CELL_DROP, 0, 1), args);
            args[0].index = 5;
        }
        raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_LEN, 6, 1), args);
        raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
        emit(f.code, f.function.code_count++, XR_RBC_RET, 6, 0, 0);
        uint64_t value = 255, result = 0;
        CHECK(execute(&f.module, 0, &value, 1, &result) == XR_RBC_OK && result == (operation == 3 ? 2u : 1u));
        const uint64_t invalid[] = {256, 257, UINT64_MAX};
        for (uint32_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
            result = 91;
            CHECK(execute(&f.module, 0, &invalid[i], 1, &result) == XR_RBC_INVALID && result == 91);
        }
        f.accesses[0].kind = XR_RBC_ARRAY_U64;
        reject_prepare(&f.module, 0, &value, 1);
    }
    RawArray f;
    raw_array_init(&f);
    raw_unsigned_kinds(&f, true);
    f.function.parameter_count = 1;
    XrRbcArrayLocation args[] = {{0, false}, {1, false}};
    raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_NEW, 2, 1), args);
    immediate(f.code, f.function.code_count++, XR_RBC_LOADI, 1, 0);
    args[0].index = 2;
    raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_GET, 6, 2), args);
    raw_unsigned_access(&f, raw_array_op(XR_RBC_ARRAY_DROP, 0, 1), args);
    emit(f.code, f.function.code_count++, XR_RBC_RET, 6, 0, 0);
    const uint64_t bits[] = {0, UINT64_C(0x8000000000000000), UINT64_MAX};
    for (uint32_t i = 0; i < sizeof(bits) / sizeof(*bits); ++i) {
        uint64_t result = 0;
        CHECK(execute(&f.module, 0, &bits[i], 1, &result) == XR_RBC_OK && result == bits[i]);
    }
}
#endif // REGISTER_ARRAY_UNSIGNED_CASES_H
