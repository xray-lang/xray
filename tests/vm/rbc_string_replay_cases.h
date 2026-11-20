/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * rbc_string_replay_cases.h - Observe completed String replay with scalar spills
 */
#ifndef RBC_STRING_REPLAY_CASES_H
#define RBC_STRING_REPLAY_CASES_H

static XrLirModule *rbc_string_replay_fixture(XrLirFunction *root, XrLirValue pair[2]) {
    CArrayFixture c = {0};
    OK(xr_lir_module_create(NULL, &c.module));
    OK(xr_lir_string_catalog_install(c.module, false, false, false));
    XrLirType count = XR_LIR_I64;
    XrLirFunctionSpec spec = {.name="string_replay",.name_length=13,.parameters=&count,
        .parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &spec, root));
    OK(xr_lir_define_function(c.module, *root));
    XrLirType carries[] = {XR_LIR_STRING, XR_LIR_I64, XR_LIR_I64, XR_LIR_I64};
    XrLirBlock entry = {0}, header = {0}, body = {0}, result_block = {0};
    OK(xr_lir_add_block(c.module, *root, NULL, 0, &entry));
    OK(xr_lir_set_entry(c.module, *root, entry));

    XrLirValue limit = {0};
    OK(xr_lir_function_parameter(c.module, *root, 0, &limit));
    /* A wide scalar instruction stream forces the compiler's spill path.
     * The values form a real chain and yield the first swap operand, two. */
    XrLirValue left = ca_constant(&c, entry, 2 - (int64_t)XR_RBC_REGISTER_COUNT_MAX);
    XrLirValue unit = ca_constant(&c, entry, 1);
    for (unsigned n = 0; n < XR_RBC_REGISTER_COUNT_MAX; ++n)
        left = ca_binary(&c, entry, XR_LIR_ADD, left, unit);
    XrLirValue initial[] = {ca_text(&c, entry, "", 0), ca_constant(&c, entry, 0),
        left, ca_constant(&c, entry, 7)};
    OK(xr_lir_add_block(c.module, *root, carries, 4, &header));
    OK(xr_lir_add_block(c.module, *root, carries, 2, &body));
    OK(xr_lir_add_block(c.module, *root, carries, 4, &result_block));
    OK(xr_lir_set_terminator(c.module, entry, &(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{header,initial,4}}}));

    XrLirValue h[4] = {{0}}, b[2] = {{0}}, e[4] = {{0}};
    for (uint32_t n = 0; n < 4; ++n) {
        OK(xr_lir_block_parameter(c.module, header, n, &h[n]));
        OK(xr_lir_block_parameter(c.module, result_block, n, &e[n]));
    }
    for (uint32_t n = 0; n < 2; ++n) OK(xr_lir_block_parameter(c.module, body, n, &b[n]));
    pair[0] = h[2]; pair[1] = h[3];
    XrLirValue condition = {0}, compared[] = {h[1], limit};
    OK(xr_lir_emit_value(c.module, header, &(XrLirInstruction){.op=XR_LIR_ICMP,.type=XR_LIR_I1,
        .predicate=XR_LIR_LT,.operands=compared,.operand_count=2}, &condition));
    OK(xr_lir_set_terminator(c.module, header, &(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
        .operands=&condition,.operand_count=1,.edge_count=2,.edges={{body,h,2},{result_block,h,4}}}));

    uint32_t role = 0;
    OK(xr_lir_string_operation(c.module, XR_LIR_STRING_OP_APPEND_OWNED, &role));
    XrLirValue suffix = ca_text(&c, body, "\xc3\xa9", 2), inputs[] = {b[0], suffix}, appended = {0};
    OK(xr_lir_emit_value(c.module, body, &(XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_STRING,
        .intrinsic=role,.operands=inputs,.operand_count=2}, &appended));
    ca_effect(&c, body, XR_LIR_STRING_DROP, suffix);
    XrLirValue step = ca_constant(&c, body, 1);
    XrLirValue next = ca_binary(&c, body, XR_LIR_ADD, b[1], step);
    /* Both original header phis are still live, so the latch must exchange
     * their physical slots simultaneously. */
    XrLirValue back[] = {appended, next, h[3], h[2]};
    OK(xr_lir_set_terminator(c.module, body, &(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
        .edge_count=1,.edges={{header,back,4}}}));

    XrLirValue length = ca_value(&c, result_block, XR_LIR_STRING_LEN, XR_LIR_I64, e, 1);
    ca_effect(&c, result_block, XR_LIR_STRING_DROP, e[0]);
    XrLirValue hundreds = ca_constant(&c, result_block, 100);
    XrLirValue ordered = ca_binary(&c, result_block, XR_LIR_ADD,
        ca_binary(&c, result_block, XR_LIR_MUL, e[2], hundreds), e[3]);
    ca_return(&c, result_block, ca_binary(&c, result_block, XR_LIR_ADD,
        ca_binary(&c, result_block, XR_LIR_MUL, ordered, hundreds), length));
    OK(xr_lir_module_finish(c.module, NULL));
    return c.module;
}

static void rbc_string_replay_check_plan(const XrRbcValueCode *code, const XrLirValue pair[2]) {
    CHECK(code && code->homes->string_window_count == 1 && code->homes->string_windows[0].replay_count);
    CHECK(code->string_replays && code->string_replay_accesses);
    const XrRbcStringReplay *replay = &code->string_replays[0];
    CHECK(replay->step_count && replay->latch_access && replay->latch_access <= code->access_count);
    CHECK(code->string_replay_accesses[replay->latch_access - 1] == 1);
    const XrRbcValueAccess *latch = &code->accesses[replay->latch_access - 1];
    CHECK(latch->operation == XR_RBC_VALUE_EDGE && latch->index < code->homes->edge_count);
    CHECK(code->homes->edges[latch->index].target_block == code->homes->string_windows[0].header);
    for (size_t n = 0; n < code->homes->edge_count; ++n) CHECK(!code->homes->edges[n].string_window);

    XrRbcValueLocation left = code->locations[pair[0].id], right = code->locations[pair[1].id];
    CHECK(left.kind == XR_RBC_VALUE_SPILL && right.kind == XR_RBC_VALUE_SPILL && left.index != right.index);
    const XrRbcStringReplayStep *last = &code->string_replay_steps[replay->first_step + replay->step_count - 1];
    unsigned cycle = 0;
    for (size_t n = 0; n < last->copy_count; ++n) {
        XrRbcStringReplayCopy copy = code->string_replay_copies[last->first_copy + n];
        if (copy.source.kind != XR_RBC_VALUE_SPILL || copy.target.kind != XR_RBC_VALUE_SPILL) continue;
        if (copy.source.index == left.index && copy.target.index == right.index) cycle |= 1;
        if (copy.source.index == right.index && copy.target.index == left.index) cycle |= 2;
    }
    CHECK(cycle == 3);
    bool spilled = false, reloaded = false;
    for (size_t n = 0; n < last->word_count; ++n) {
        uint32_t op = code->string_replay_words[last->first_word + n] & 255u;
        spilled |= op == XR_RBC_SPILL;
        reloaded |= op == XR_RBC_RELOAD;
    }
    CHECK(spilled && reloaded);
}

static void rbc_string_replay_case(void) {
    XrLirFunction root = {0};
    XrLirValue pair[2] = {{0}};
    XrLirModule *input = rbc_string_replay_fixture(&root, pair);
    XrRbcProgram *program = NULL;
    CHECK(xr_rbc_compile(input, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(input);
    const XrRbcModule *module = xr_rbc_program_view(program);
    rbc_string_replay_check_plan(xr_rbc_module_value_code(module, 0), pair);

    uint64_t count = 22;
    size_t replayed = 0;
    for (unsigned mode = 0; mode < 2; ++mode) {
        string_window_force_bypass = mode != 0;
        string_window_attempts = string_window_completed = 0;
        RbcRegionResult result = rbc_region_run_with_args(module, program->locations, &count, 1);
        /* An odd replay count makes losing the swaps observable even if the
         * bulk operation and the final loop index are both correct. */
        CHECK(result.status == XR_RBC_OK && result.bits == 20722);
        CHECK(string_window_attempts);
        if (!mode) {
            replayed = string_window_completed;
            CHECK(replayed > 1 && replayed < count && replayed % 2 == 1);
        } else CHECK(!string_window_completed);
    }
    string_window_force_bypass = false;
    xr_rbc_program_destroy(program);
    CHECK(!tracked_live);
    printf("String replay: %zu appends replayed with parallel spill copies and the ordinary output\n", replayed);
}
#endif // RBC_STRING_REPLAY_CASES_H
