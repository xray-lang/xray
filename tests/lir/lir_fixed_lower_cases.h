/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_fixed_lower_cases.h - Exact byte values retain evaluation and state traps
 */
#include "lir/xlir_rebuild.h"
#include "lir/xlir_types.h"
#include "lir/xlir_inline.h"
#include "lir/xlir_sccp.h"
#include "lir/xlir_string_elide.h"

static void fixed_length_observation(const XrLirModule *m) {
    uint32_t reads = 0;
    for (uint32_t f = 0; f < xr_lir_function_count(m); ++f) {
        XrLirFunction function;
        XrLirFunctionView view;
        OK(xr_lir_function_at(m, f, &function));
        OK(xr_lir_function_view(m, function, &view));
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block;
            XrLirBlockView body;
            OK(xr_lir_block_at(m, function, b, &block));
            OK(xr_lir_block_view(m, block, &body));
            for (uint32_t i = 0; i < body.instruction_count; ++i) {
                XrLirInstructionView op;
                OK(xr_lir_instruction_view(m, block, i, &op));
                if (op.op != XR_LIR_STATE_READ) continue;
                XrLirTypeView shape;
                OK(xr_lir_type_view(m, op.type, &shape));
                if (shape.kind != XR_LIR_TYPE_FIXED_ARRAY) continue;
                CHECK(shape.element == XR_LIR_U8 && shape.count == 3);
                CHECK(op.source.path && op.source.line == 1 && op.source.column > 0);
                ++reads;
            }
        }
    }
    CHECK(reads >= 1);
}

static void fixed_slot_diagnostic(void) {
    SourceFixture fixture = source_fixture("var first=1\nvar later:Array<bool>=[true]\n");
    XrLirLowered output = {0};
    XrLirLowerFailure failure = {0};
    XrLirLowerRequest request = {fixture.closed, fixture.source.snapshot, false};
    CHECK(xr_lir_lower_program_checked(&request, &output, &failure) == XR_LIR_UNSUPPORTED);
    source_fixture_free(&fixture);
    CHECK(!output.module && !output.entry.id);
    CHECK(failure.diagnostic.source.path == failure.owned_path && failure.owned_path);
    CHECK(failure.diagnostic.source.line == 2 && failure.diagnostic.source.column > 0);
    CHECK(strstr(failure.diagnostic.message, "module slot payload type"));
    xr_lir_lower_failure_free(&failure);
}

static void fixed_sources(void) {
    fixed_slot_diagnostic();
    XrLirLowered builtin = lower_text("fn ignored()->[u8;3]{return b\"abc\"}\nprint(17)\n");
    CHECK(!xr_lir_constructed_type_count(builtin.module));
    CHECK(!xr_lir_intrinsic_count(builtin.module));
    xr_lir_lowered_free(&builtin);
    static const char *cases[] = {
        "const bytes=b\"\\x00\\x80\\xff\"\nprint(bytes[0])\nprint(bytes[1])\nprint(bytes[2])\n",
        "fn choose(flag:bool)->[u8;3]{return flag?b\"abc\":b\"XYZ\"}\n"
        "fn cycle(n:i64)->[u8;3]{var value=b\"abc\"\nvar i=0\nwhile(i<n){value=choose(i==1)\n"
        "i=i+1}\nreturn value}\nprint(cycle(3)[0])\n",
        "var bytes=b\"abc\"\nconst saved=bytes\nfn change(){bytes=b\"XYZ\"}\n"
        "change()\nprint(saved[0])\nprint(bytes[0])\n",
        "fn identity(value:[u8;17])->[u8;17]{return value}\n"
        "fn at(value:[u8;17],index:i64)->u8{return value[index]}\n"
        "const bytes=b\"0123456789ABCDEFG\"\nprint(len(identity(bytes)))\n"
        "print(identity(bytes)[16])\nprint(at(bytes,16))\n",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrLirLowered output = lower_text(cases[i]);
        CHECK(xr_lir_constructed_type_count(output.module) == 1);
        CHECK(!xr_lir_intrinsic_count(output.module));
        XrLirModule *copy = NULL;
        OK(xr_lir_rebuild(output.module, &copy));
        xr_lir_lowered_free(&output);
        OK(xr_lir_verify(copy, NULL));
        XrLirTypeView shape;
        OK(xr_lir_type_view(copy, XR_LIR_TYPE_COUNT, &shape));
        CHECK(shape.kind == XR_LIR_TYPE_FIXED_ARRAY && shape.element == XR_LIR_U8);
        CHECK(shape.count == (i == 3 ? 17u : 3u));
        xr_lir_module_destroy(copy);
    }
    XrLirLowered output = lower_text(
        "fn inspect()->i64{return len(later)}\nconst measured=inspect()\n"
        "const later=b\"abc\"\nprint(measured)\n");
    XrLirModule *copies[4] = {0};
    OK(xr_lir_rebuild(output.module, &copies[0]));
    OK(xr_lir_inline(output.module, &copies[1]));
    OK(xr_lir_sccp(output.module, &copies[2]));
    OK(xr_lir_string_elide(output.module, &copies[3]));
    xr_lir_lowered_free(&output);
    for (size_t i = 0; i < 4; ++i) {
        OK(xr_lir_verify(copies[i], NULL));
        fixed_length_observation(copies[i]);
        xr_lir_module_destroy(copies[i]);
    }
    puts("fixed byte lowering preserves owned shapes and length receiver observations");
}
