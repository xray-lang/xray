/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_cleanup.inc.c - Static cleanup frontiers on resumable VM frames
 *
 * KEY CONCEPT:
 *   Registration activates an immutable record; only a returned cleanup retires it.
 */
static XrXirAction vm_cleanup_exit(ScalarRun *run, VmState *state) {
    if (state->cleanup_waiting) {
        if (run->view->inbox.status != XR_XIR_CALL_RETURNED || !state->frontier)
            return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
        uint32_t parent = run->function->blocks[vm_block(run->function, state->frontier - 1)].frontier;
        if (xr_xir_call_cleanup_frontier(run->view, parent) != XR_XIR_CALL_READY)
            return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
        state->frontier = parent;
        state->cleanup_waiting = false;
    }
    uint32_t target = run->view->scope_exit ? state->exit_target : 0;
    if (state->frontier == target)
        return (XrXirAction){XR_XIR_ACTION_EXIT_DONE, 0, NULL, 0, {0}, {0}, 0};
    if (!state->frontier || state->frontier > run->function->instruction_count)
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
    const XrXirInstruction *registration = &run->function->instructions[state->frontier - 1];
    if (registration->op != XR_XIR_CLEANUP_REGISTER) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
    for (uint32_t i = 0; i < registration->args[1]; ++i)
        state->arguments[i] = vm_value_operand(run, run->function->operands[registration->args[0] + i]);
    state->cleanup_waiting = true;
    return (XrXirAction){XR_XIR_ACTION_CALL, (uint32_t)registration->immediate, state->arguments,
        registration->args[1], {0}, {0}, XR_XIR_ACTION_CLEANUP};
}
static XrXirAction vm_cleanup_continue(ScalarRun *run, VmState *state) {
    state->leaving = false;
    if (state->landing) {
        uint32_t handler = state->landing; state->landing = 0;
        return vm_panic_land(run, state, handler, (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0,
            {XR_XIR_I64, 0, run->view->inbox.status}, run->view->inbox.panic, 0});
    }
    XrXirRunStatus status = scalar_edge(run, state->instruction, state->exit_block);
    if (status != XR_XIR_RUN_OK) return xr_xir_call_fault(status);
    state->instruction = run->function->blocks[state->exit_block].first;
    return (XrXirAction){XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};
}
static XrXirRunStatus vm_cleanup_step(ScalarRun *run, VmState *state,
    const XrXirInstruction *op, XrXirAction *action) {
    if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
    uint32_t target = op->targets[0];
    if (op->op == XR_XIR_CLEANUP_REGISTER || state->frontier == (uint32_t)op->immediate) {
        XrXirRunStatus status = scalar_edge(run, state->instruction, target);
        if (status != XR_XIR_RUN_OK) return status;
        if (op->op == XR_XIR_CLEANUP_REGISTER) {
            uint32_t frontier = state->instruction + 1;
            if (xr_xir_call_cleanup_frontier(run->view, frontier) != XR_XIR_CALL_READY)
                return XR_XIR_RUN_BAD_ARTIFACT;
            state->frontier = frontier;
        }
        state->instruction = run->function->blocks[target].first;
        return XR_XIR_RUN_OK;
    }
    state->exit_target = (uint32_t)op->immediate; state->exit_block = target; state->leaving = true;
    action->kind = XR_XIR_ACTION_LEAVE;
    if (op->op == XR_XIR_CLEANUP_ERROR) {
        action->value = vm_value_operand(run, op->args[0]);
        if (action->value.type == XR_XIR_ERROR) {
            XrXirValue concrete = {0};
            if (!xr_xir_error_borrow(&action->value, &concrete)) return XR_XIR_RUN_BAD_ARTIFACT;
            action->value = concrete;
        }
        action->flags = XR_XIR_ACTION_LEAVE_ERROR;
    }
    return XR_XIR_RUN_OK;
}
