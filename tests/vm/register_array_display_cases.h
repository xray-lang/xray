/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_array_display_cases.h - Exact Array sources produce owned String banks
 */
#ifndef REGISTER_ARRAY_DISPLAY_CASES_H
#define REGISTER_ARRAY_DISPLAY_CASES_H
static void test_raw_array_display(void) {
    const XrRbcRegisterKind families[] = {XR_RBC_ARRAY_I64, XR_RBC_ARRAY_STRING, XR_RBC_ARRAY_U8, XR_RBC_ARRAY_U64};
    for (unsigned family = 0; family < 4; ++family) {
        for (unsigned spill = 0; spill < 2; ++spill) {
            RawArray f;
            raw_array_init(&f);
            f.kinds[0] = families[family]; f.kinds[1] = XR_RBC_STRING; f.kinds[2] = XR_RBC_BITS;
            f.function.register_count = f.function.register_kind_count = 3;
            f.function.cell_count = f.function.cell_kind_count = 0; f.function.cell_kinds = NULL;
            XrRbcArrayAccess row = {.operation=XR_RBC_ARRAY_NEW, .kind=families[family], .count=0};
            raw_array_access(&f, row, NULL);
            XrRbcArrayLocation input = {0, false};
            row = (XrRbcArrayAccess){.operation=XR_RBC_ARRAY_DISPLAY, .kind=families[family],
                .result={spill ? 0u : 1u, spill != 0}, .count=1};
            raw_array_access(&f, row, &input);
            row = (XrRbcArrayAccess){.operation=XR_RBC_ARRAY_DROP, .kind=families[family], .count=1};
            raw_array_access(&f, row, &input);
            if (spill) {
                f.function.spill_count = f.function.reference_spill_count = 1;
                f.spill_kinds[0] = XR_RBC_STRING;
                f.function.reference_spill_kinds = f.spill_kinds;
                f.function.reference_spill_kind_count = 1;
                immediate(f.code, f.function.code_count++, XR_RBC_RELOAD, 1, 0);
            }
            emit(f.code, f.function.code_count++, XR_RBC_STRING_LEN, 2, 1, 0);
            emit(f.code, f.function.code_count++, XR_RBC_STRING_DROP, 1, 0, 0);
            emit(f.code, f.function.code_count++, XR_RBC_RET, 2, 0, 0);
            uint64_t result = 0;
            CHECK(execute(&f.module, 0, NULL, 0, &result) == XR_RBC_OK && result == 2);
            f.accesses[1].count = 0;
            reject_prepare(&f.module, 0, NULL, 0);
            f.accesses[1].count = 1;
            f.accesses[1].kind = families[(family + 1) % 4];
            reject_prepare(&f.module, 0, NULL, 0);
            f.accesses[1].kind = families[family];
            f.accesses[1].operation = (XrRbcArrayOperation)(XR_RBC_ARRAY_DISPLAY + 1);
            reject_prepare(&f.module, 0, NULL, 0);
            f.accesses[1].operation = XR_RBC_ARRAY_DISPLAY;
            if (spill) f.spill_kinds[0] = families[family];
            else f.kinds[1] = XR_RBC_BITS;
            reject_prepare(&f.module, 0, NULL, 0);
        }
    }
}
#endif // REGISTER_ARRAY_DISPLAY_CASES_H
