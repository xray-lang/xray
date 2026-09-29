/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_cleanup.inc.c - Native static cleanup dispatch and continuations
 */
static void emit_cleanup_step(CBuffer *buffer, const XrXirFunction *function,
    const XrXirFunctionLayout *layout, uint32_t index) {
    const XrXirInstruction *op = &function->instructions[index];
    if (op->op == XR_XIR_CLEANUP_REGISTER) {
        emit_edge(buffer, function, layout, index, op->targets[0], true);
        append(buffer, "        state->frontier = %uu;\n", index + 1);
    } else if (function->blocks[emit_block(function, index)].frontier == (uint32_t)op->immediate) {
        emit_edge(buffer, function, layout, index, op->targets[0], true);
    } else {
        append(buffer, "        state->exit_target = %uu; state->leave_instruction = %uu; state->leaving = true;\n",
            (uint32_t)op->immediate, index + 1);
        if (op->op == XR_XIR_CLEANUP_ERROR) {
            append(buffer, "        { XrXirValue error = "); emit_value(buffer, function, layout, op->args[0]);
            append(buffer, ";\n");
            if (xr_xir_operand_type(function, op->args[0]) == XR_XIR_ERROR)
                append(buffer, "        XrXirValue concrete = {0};\n"
                    "        if (!xr_xir_error_borrow(&error, &concrete)) goto invalid;\n        error = concrete;\n");
            append(buffer, "        return (XrXirAction){XR_XIR_ACTION_LEAVE, 0, NULL, 0, error, {0}, XR_XIR_ACTION_LEAVE_ERROR}; }\n");
        } else append(buffer, "        return (XrXirAction){XR_XIR_ACTION_LEAVE, 0, NULL, 0, {0}, {0}, 0};\n");
        return;
    }
    append(buffer, "        return (XrXirAction){XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n");
}
static void emit_cleanup_entry(CBuffer *buffer, const XrXirFunction *function,
    const XrXirFunctionLayout *layout) {
    if (!emit_has_cleanup(function)) {
        append(buffer, "    if (view->phase == XR_XIR_CALL_EXIT)\n"
            "        return (XrXirAction){XR_XIR_ACTION_EXIT_DONE, 0, NULL, 0, {0}, {0}, 0};\n");
        return;
    }
    append(buffer, "    if (view->phase == XR_XIR_CALL_EXIT) {\n"
        "        if (state->cleanup_waiting) {\n"
        "            if (view->inbox.status != XR_XIR_CALL_RETURNED) goto invalid;\n"
        "            state->frontier = state->cleanup_parent; state->cleanup_waiting = false;\n        }\n"
        "        if (state->frontier == (view->scope_exit ? state->exit_target : 0u))\n"
        "            return (XrXirAction){XR_XIR_ACTION_EXIT_DONE, 0, NULL, 0, {0}, {0}, 0};\n"
        "        switch (state->frontier) {\n");
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op != XR_XIR_CLEANUP_REGISTER) continue;
        append(buffer, "        case %uu:\n", i + 1);
        for (uint32_t p = 0; p < op->args[1]; ++p) {
            append(buffer, "            state->arguments[%u] = ", p);
            emit_value(buffer, function, layout, function->operands[op->args[0] + p]); append(buffer, ";\n");
        }
        append(buffer, "            state->cleanup_parent = %uu; state->cleanup_waiting = true;\n"
            "            return (XrXirAction){XR_XIR_ACTION_CALL, %uu, %s, %uu, {0}, {0}, XR_XIR_ACTION_CLEANUP};\n",
            function->blocks[emit_block(function, i)].frontier, (uint32_t)op->immediate,
            op->args[1] ? "state->arguments" : "NULL", op->args[1]);
    }
    append(buffer, "        default: goto invalid;\n        }\n    }\n"
        "    if (state->leaving) {\n        state->leaving = false;\n"
        "        if (state->exit_pc) {\n            uint32_t pc = state->exit_pc; state->exit_pc = 0;\n"
        "            return xr_xir_instance_panic_land(view, state->frame, (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0,\n"
        "                {XR_XIR_I64, 0, view->inbox.status}, view->inbox.fault, 0}, state->exit_destination, pc, &state->pc);\n        }\n"
        "        switch (state->leave_instruction) {\n");
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op != XR_XIR_CLEANUP_LEAVE && op->op != XR_XIR_CLEANUP_ERROR) continue;
        append(buffer, "        case %uu:\n", i + 1);
        emit_edge(buffer, function, layout, i, op->targets[0], true);
        append(buffer, "            return (XrXirAction){XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n");
    }
    append(buffer, "        default: goto invalid;\n        }\n    }\n");
}
