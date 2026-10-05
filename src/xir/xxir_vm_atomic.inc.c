/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_atomic.inc.c - Shared atomic execution with owned retry progress
 */
_Static_assert(XR_XIR_ATOMIC_TO_STRING - XR_XIR_ATOMIC_LOAD == 9,
    "atomic instructions follow the shared operation order");

static XrXirRunStatus vm_atomic_step(ScalarRun *run, VmState *state,
    const XrXirInstruction *op, uint32_t destination, XrXirAction *action) {
    XrXirValueAdmission *admission = xr_xir_call_admission(run->view);
    if (!admission) return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirValue output = {0};
    XrXirCallStatus permission = xr_xir_call_execution_status(run->view);
    if (permission == XR_XIR_CALL_CANCELLED) return XR_XIR_RUN_OK;
    if (permission != XR_XIR_CALL_READY) return XR_XIR_RUN_BAD_ARTIFACT;
    if (op->op == XR_XIR_ATOMIC_NEW) {
        XrXirValue initial = vm_value_operand(run, op->args[0]);
        XrXirValueStatus status = xr_xir_atomic_new(op->type, &initial, admission, &output);
        if (status != XR_XIR_VALUE_OK) return value_run_status(status);
        permission = xr_xir_call_execution_status(run->view);
        if (permission != XR_XIR_CALL_READY) {
            xr_xir_value_drop(&output);
            return permission == XR_XIR_CALL_CANCELLED ? XR_XIR_RUN_OK : XR_XIR_RUN_BAD_ARTIFACT;
        }
    } else {
        XrXirAtomicOutcome outcome;
        if (state->atomic.phase) {
            outcome = xr_xir_atomic_resume(&state->atomic, admission, run->view, &output);
        } else {
            const uint32_t *ids = &run->function->operands[op->args[0]];
            XrXirAtomicOperation operation = (XrXirAtomicOperation)(op->op - XR_XIR_ATOMIC_LOAD);
            uint32_t count = operation == XR_XIR_ATOMIC_OPERATION_COMPARE_EXCHANGE ? 2u :
                operation == XR_XIR_ATOMIC_OPERATION_LOAD || operation == XR_XIR_ATOMIC_OPERATION_TOGGLE ||
                operation == XR_XIR_ATOMIC_OPERATION_TO_STRING ? 0u : 1u;
            XrXirValue receiver = vm_value_operand(run, ids[0]);
            XrXirValue values[2] = {{0}, {0}}, ordering = {0};
            for (uint32_t i = 0; i < count; ++i) values[i] = vm_value_operand(run, ids[i + 1]);
            bool has_ordering = op->args[1] == count + 2;
            if (has_ordering) ordering = vm_value_operand(run, ids[count + 1]);
            XrXirAtomicRequest request = {&receiver, count ? values : NULL,
                has_ordering ? &ordering : NULL, count, operation, op->type};
            outcome = xr_xir_atomic_start(&request, admission, run->view, &state->atomic, &output);
        }
        if (outcome.permission == XR_XIR_CALL_CANCELLED) return XR_XIR_RUN_OK;
        if (outcome.permission != XR_XIR_CALL_READY) return XR_XIR_RUN_BAD_ARTIFACT;
        if (outcome.status != XR_XIR_RUN_OK) return outcome.status;
        if (outcome.continuing) {
            *action = (XrXirAction){XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};
            return XR_XIR_RUN_OK;
        }
    }
    if (xr_xir_type_is_owned(run->module->types, op->type))
        xr_xir_owned_slot_move(run->frame, destination, &output);
    else if (op->type != XR_XIR_UNIT) xr_xir_scalar_store(run->frame, destination, output.payload);
    xr_xir_atomic_progress_clear(&state->atomic);
    ++state->instruction;
    return XR_XIR_RUN_OK;
}
