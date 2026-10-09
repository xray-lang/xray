/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_compile_c_buffer_shapes.h - Fixed outputs and charged decimal emission
 */
static void compile_c_buffer_shape_cases(void) {
    static const char *const expected[] = {
        "[4294967295]", "[18446744073709551615]"
    };
    /* Creation1, two literal reads/stores4, digit divisions/reads/stores
     * 30 or 60, allocation1 and final NUL1: exactly37 or67. */
    for (unsigned wide = 0; wide < 2; ++wide) {
        uint64_t exact = wide ? 67 : 37;
        size_t length = wide ? 22 : 12;
        for (uint64_t work = 1; work <= exact; ++work) {
            reset_observer();
            XrXirCompileContext context = context_new(work);
            CBuffer buffer = {.limit = length + 1, .status = XR_XIR_OK,
                .context = &context};
            if (wide) emit_decimal_ull(&buffer, ULLONG_MAX, "[", 1, "]", 1);
            else emit_decimal_u(&buffer, UINT_MAX, "[", 1, "]", 1);
            (void) emit_finalize(&buffer);
            CHECK(buffer.status == (work == exact ? XR_XIR_OK : XR_XIR_BUDGET));
            CHECK(stats(&context).work == work);
            CHECK(buffer.length <= length);
            if (buffer.text) CHECK(!memcmp(buffer.text, expected[wide], buffer.length));
            if (work == exact) CHECK(buffer.length == length && !strcmp(buffer.text, expected[wide]));
            else {
                size_t saved = buffer.length;
                emit_decimal_u(&buffer, 0, (const char *)(uintptr_t)1, 1,
                    (const char *)(uintptr_t)1, 1);
                CHECK(buffer.length == saved && stats(&context).work == work);
            }
            xr_compile_resources_free(buffer.text);
            xr_compile_resources_release(context.resources);
            CHECK(!live && !physical);
        }
        reset_observer();
        XrXirCompileContext context = context_new(UINT64_MAX);
        CBuffer measured = {.limit = length + 1, .status = XR_XIR_OK,
            .context = &context, .measuring = true};
        if (wide) emit_decimal_ull(&measured, ULLONG_MAX, "[", 1, "]", 1);
        else emit_decimal_u(&measured, UINT_MAX, "[", 1, "]", 1);
        /* Creation1, two span updates2, digit divisions10/20, length update1. */
        CHECK(stats(&context).work == (wide ? 24 : 14));
        CHECK(measured.status == XR_XIR_OK && measured.length == length);
        CHECK(!measured.text && !measured.capacity && stats(&context).allocation_count == 1);
        xr_compile_resources_release(context.resources);
        CHECK(!live && !physical);
    }
    for (unsigned tracking = 0; tracking < 2; ++tracking) {
        reset_observer();
        XrXirCompileContext context = context_new(UINT64_MAX);
        CBuffer buffer = {.limit = 64, .status = XR_XIR_OK, .context = &context,
            .tracking = tracking != 0};
        EMIT_FORMAT(&buffer, et_d13fcec1f19e2337, " state->pc = %uu;\n", UINT_MAX);
        CHECK(emit_finalize(&buffer));
        CHECK(!strcmp(buffer.text, " state->pc = 4294967295u;\n"));
        CBuffer measured = {.limit = buffer.length + 1, .status = XR_XIR_OK,
            .context = &context, .measuring = true, .tracking = tracking != 0};
        EMIT_FORMAT(&measured, et_d13fcec1f19e2337, " state->pc = %uu;\n", UINT_MAX);
        CHECK(measured.status == XR_XIR_OK && measured.length == buffer.length);
        CHECK(!measured.text && !measured.capacity);
        xr_compile_resources_free(buffer.text);
        xr_compile_resources_release(context.resources);
        CHECK(!live && !physical);
    }
    reset_observer();
    XrXirCompileContext context = context_new(1);
    CBuffer blocked = {.limit = 32, .status = XR_XIR_OK, .context = &context};
    emit_template(&blocked, (const EmitTemplate *)(uintptr_t)1);
    CHECK(blocked.status == XR_XIR_BUDGET && !blocked.text && !blocked.length);
    CHECK(stats(&context).work == 1 && stats(&context).allocation_count == 1);
    xr_compile_resources_release(context.resources);
    CHECK(!live && !physical);
}
