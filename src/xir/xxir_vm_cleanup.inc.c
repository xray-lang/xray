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
#include "xxir_scope_cancel_shape.h"
static XrXirAction vm_cleanup_exit(ScalarRun *run, VmState *state) {
    if (state->cleanup_waiting) {
        if ((run->view->inbox.status != XR_XIR_CALL_RETURNED &&
            !xr_xir_call_panic_status(run->view->inbox.status)) || !state->frontier)
            return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
        uint32_t parent = run->function->blocks[vm_block(run->function, state->frontier - 1)].frontier;
        if(xr_xir_call_scope_cancel_frontier(run->view)==state->frontier) {
            uint32_t join=xr_xir_scope_cancel_join(run->function,state->frontier);
            if(join==UINT32_MAX)return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
            state->exit_target=parent;state->leaving=false;
            state->waiting=true;state->invoke=join+1;
            state->panic=run->function->blocks[vm_block(run->function,join)].panic;
        }
        if (xr_xir_call_cleanup_frontier(run->view, parent) != XR_XIR_CALL_READY)
            return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
        state->frontier = parent;
        state->cleanup_waiting = false;
    }
    uint32_t target = run->view->scope_exit ? state->exit_target : 0;
    if (state->frontier == target)
        return (XrXirAction) {XR_XIR_ACTION_EXIT_DONE, 0, NULL, 0, {0}, {{0}, {0}, {0}, {0}}, 0};
    if (!state->frontier || state->frontier > run->function->instruction_count)
        return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
    const XrXirInstruction *registration = &run->function->instructions[state->frontier - 1];
    if (registration->op != XR_XIR_CLEANUP_REGISTER) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
    for (uint32_t i = 0; i < registration->args[1]; ++i)
        state->arguments[i] = vm_value_operand(run, run->function->operands[registration->args[0] + i]);
    state->cleanup_waiting = true;
    return (XrXirAction) {XR_XIR_ACTION_CALL,
                          (uint32_t) registration->immediate,
                          registration->args[1] ? state->arguments : NULL,
                          registration->args[1],
                          {0},
                          {{0}, {0}, {0}, {0}},
                          XR_XIR_ACTION_CLEANUP};
}
static XrXirAction vm_cleanup_continue(ScalarRun *run, VmState *state) {
    state->leaving = false;
    if (xr_xir_call_panic_status(run->view->inbox.status) && !state->landing) {
        uint32_t handler=run->function->blocks[vm_block(run->function,state->instruction)].panic;
        XrXirAction panic={XR_XIR_ACTION_FAULT,0,NULL,0,
            {XR_XIR_I64,0,run->view->inbox.status},run->view->inbox.panic,0};
        return handler?vm_panic_land(run,state,handler,panic):panic;
    }
    if (state->landing) {
        uint32_t handler = state->landing; state->landing = 0;
        return vm_panic_land(run, state, handler, (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0,
            {XR_XIR_I64, 0, run->view->inbox.status}, run->view->inbox.panic, 0});
    }
    const XrXirInstruction *producer=run->function->instructions+state->instruction;
    uint32_t first=run->function->blocks[state->exit_block].first;
    const XrXirInstruction *landing=run->function->instructions+first;
    if(producer->op==XR_XIR_CLEANUP_ERROR&&landing->op==XR_XIR_INVOKE_ERROR) {
        if(landing->type!=XR_XIR_ERROR||landing->immediate!=state->instruction||
            run->view->inbox.status!=XR_XIR_CALL_THROWN)return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);
        uint32_t destination=run->layout->offsets[run->function->parameter_count+first];
        XrXirValueStatus copied=xr_xir_owned_slot_copy(run->frame,destination,run->view->arena,
            XR_XIR_ERROR,run->view->inbox.value.payload);
        if(copied!=XR_XIR_VALUE_OK)return xr_xir_call_view_operation_fault(copied,(XrXirFaultDetail){0});
        state->instruction=first+1;
        return (XrXirAction){.kind=XR_XIR_ACTION_CONTINUE};
    }
    XrXirRunStatus status = scalar_edge(run, state->instruction, state->exit_block);
    if (status != XR_XIR_RUN_OK) return xr_xir_call_fault(status);
    state->instruction = run->function->blocks[state->exit_block].first;
    return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {{0}, {0}, {0}, {0}}, 0};
}
static XrXirRunStatus vm_cleanup_step(ScalarRun *run, VmState *state,
    const XrXirInstruction *op, XrXirAction *action) {
    if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
    uint32_t target = op->targets[0];
    if (op->op == XR_XIR_CLEANUP_REGISTER ||
        (op->op!=XR_XIR_CLEANUP_ERROR && state->frontier == (uint32_t)op->immediate)) {
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
