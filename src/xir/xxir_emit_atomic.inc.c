/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_atomic.inc.c - Portable statements for the common atomic runtime
 */
static void emit_atomic_step(CBuffer *buffer, const XrXirFunction *function,
    const XrXirInstruction *op, const XrXirFunctionLayout *layout, uint32_t destination, uint32_t index) {
    append(buffer, "        state->pc = %uu;\n        { XrXirValue value = {0};\n"
        "        XrXirCallStatus permission = xr_xir_call_execution_status(view);\n"
        "        if (permission == XR_XIR_CALL_CANCELLED) return (XrXirAction)"
        "{XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n"
        "        if (permission != XR_XIR_CALL_READY) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);\n", index);
    if (op->op == XR_XIR_ATOMIC_NEW) {
        append(buffer, "        XrXirValue initial = ");
        emit_value(buffer, function, layout, op->args[0]);
        append(buffer, ";\n        XrXirValueStatus status = xr_xir_atomic_new((XrXirType)%uu, "
            "&initial, xr_xir_call_admission(view), &value);\n"
            "        if (status != XR_XIR_VALUE_OK) return xr_xir_call_fault("
            "status == XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY : "
            "status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : "
            "status == XR_XIR_VALUE_UNSUPPORTED ? XR_XIR_RUN_UNSUPPORTED : XR_XIR_RUN_BAD_ARTIFACT);\n"
            "        permission = xr_xir_call_execution_status(view);\n"
            "        if (permission != XR_XIR_CALL_READY) { xr_xir_value_drop(&value);\n"
            "            if (permission == XR_XIR_CALL_CANCELLED) return (XrXirAction)"
            "{XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n"
            "            return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT); }\n", (uint32_t)op->type);
    } else {
        uint32_t operation = (uint32_t)(op->op - XR_XIR_ATOMIC_LOAD);
        uint32_t count = op->op == XR_XIR_ATOMIC_COMPARE_EXCHANGE ? 2u :
            op->op == XR_XIR_ATOMIC_LOAD || op->op == XR_XIR_ATOMIC_TOGGLE ||
            op->op == XR_XIR_ATOMIC_TO_STRING ? 0u : 1u;
        const uint32_t *ids = &function->operands[op->args[0]];
        bool has_ordering = op->args[1] == count + 2;
        append(buffer, "        XrXirAtomicOutcome outcome;\n"
            "        if (state->atomic.phase) outcome = xr_xir_atomic_resume(&state->atomic, "
            "xr_xir_call_admission(view), view, &value);\n"
            "        else {\n        XrXirValue receiver = ");
        emit_value(buffer, function, layout, ids[0]);
        append(buffer, ";\n");
        if (count) {
            append(buffer, "        XrXirValue operands[%u] = {", count);
            for (uint32_t i = 0; i < count && emit_work(buffer, 1); ++i) {
                if (i) append(buffer, ", ");
                emit_value(buffer, function, layout, ids[i + 1]);
            }
            append(buffer, "};\n");
        }
        if (has_ordering) {
            append(buffer, "        XrXirValue ordering = ");
            emit_value(buffer, function, layout, ids[count + 1]);
            append(buffer, ";\n");
        }
        append(buffer, "        XrXirAtomicRequest request = {&receiver, %s, %s, %uu, "
            "(XrXirAtomicOperation)%uu, (XrXirType)%uu};\n"
            "        outcome = xr_xir_atomic_start(&request, xr_xir_call_admission(view), "
            "view, &state->atomic, &value); }\n"
            "        if (outcome.permission == XR_XIR_CALL_CANCELLED) return (XrXirAction)"
            "{XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n"
            "        if (outcome.permission != XR_XIR_CALL_READY) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);\n"
            "        if (outcome.status != XR_XIR_RUN_OK) return xr_xir_call_fault(outcome.status);\n"
            "        if (outcome.continuing) return (XrXirAction)"
            "{XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n",
            count ? "operands" : "NULL", has_ordering ? "&ordering" : "NULL", count, operation, (uint32_t)op->type);
    }
    if (xr_xir_type_is_owned(buffer->types, op->type))
        append(buffer, "        xr_xir_owned_slot_move(state->frame, %uu, &value);\n", destination);
    else if (op->type != XR_XIR_UNIT)
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, value.payload);\n", destination);
    append(buffer, "        xr_xir_atomic_progress_clear(&state->atomic);\n        state->pc = %uu;\n"
        "        }\n        return (XrXirAction){XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n", index + 1);
}
