/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm.c - Typed Lowered instruction execution
 *
 * KEY CONCEPT:
 *   Values reside at lowering-owned frame offsets; scalar rules are shared
 *   with generated native code and all exits pass through one frame owner.
 */

#include "xxir_vm.h"

typedef struct ScalarRun {
    const XrXirModule *module;
    const XrXirFunction *function;
    const XrXirFunctionLayout *layout;
    void *frame;
} ScalarRun;

typedef struct VmState {
    uint32_t instruction, destination;
    XrXirType expected;
    bool initialized, waiting;
    XrXirScalar arguments[2];
} VmState;

static XrXirRunStatus scalar_step(ScalarRun *run, VmState *state, XrXirAction *action) {
    uint32_t instruction = state->instruction;
    const XrXirInstruction *op = &run->function->instructions[instruction];
    uint32_t result_id = run->function->parameter_count + instruction;
    uint32_t next = instruction + 1;
    int64_t value = 0;
    *action = (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}};
    switch (op->op) {
    case XR_XIR_CONST_BOOL:
    case XR_XIR_CONST_I64:
        value = op->immediate;
        break;
    case XR_XIR_SCALAR_COPY:
        value = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        break;
    case XR_XIR_ADD_I64: {
        int64_t left = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        int64_t right = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]);
        XrXirRunStatus status = xr_xir_scalar_add(left, right, &value);
        if (status != XR_XIR_RUN_OK) return status;
        break;
    }
    case XR_XIR_EQ_I64:
        value = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]) ==
                xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]);
        break;
    case XR_XIR_LT_I64:
        value = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]) <
                xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]);
        break;
    case XR_XIR_JUMP:
        next = run->function->blocks[op->targets[0]].first;
        break;
    case XR_XIR_BRANCH: {
        int64_t condition = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        next = run->function->blocks[op->targets[condition ? 0 : 1]].first;
        break;
    }
    case XR_XIR_CALL: {
        const XrXirFunction *callee = &run->module->functions[op->immediate];
        for (uint32_t i = 0; i < callee->parameter_count; ++i)
            state->arguments[i] = (XrXirScalar) {(uint32_t) callee->parameters[i], 0,
                xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[i]])};
        state->waiting = true;
        state->destination = run->layout->offsets[result_id];
        state->expected = op->type;
        *action = (XrXirAction) {XR_XIR_ACTION_CALL, (uint32_t) op->immediate,
            state->arguments, callee->parameter_count, {0, 0, 0}};
        state->instruction = next;
        return XR_XIR_RUN_OK;
    }
    case XR_XIR_SUSPEND:
        action->kind = XR_XIR_ACTION_SUSPEND;
        break;
    case XR_XIR_THROW:
        *action = (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, {XR_XIR_I64, 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])}};
        return XR_XIR_RUN_OK;
    case XR_XIR_RETURN:
        action->kind = XR_XIR_ACTION_RETURN;
        action->value.type = (uint32_t) run->function->result;
        if (run->function->result != XR_XIR_UNIT)
            action->value.payload = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        return XR_XIR_RUN_OK;
    default:
        return XR_XIR_RUN_BAD_ARTIFACT;
    }
    if (op->type != XR_XIR_UNIT)
        xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], value);
    state->instruction = next;
    return XR_XIR_RUN_OK;
}

static XrXirAction vm_resume(XrXirCallView *view) {
    const XrXirVmBinding *binding = view->environment;
    const XrXirModule *module = xr_xir_artifact_module(binding->artifact);
    const XrXirFunction *function = &module->functions[binding->function];
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(binding->artifact, binding->function);
    VmState *state = view->state;
    ScalarRun run = {module, function, layout, state + 1};
    if (!state->initialized) {
        if (view->argument_count != function->parameter_count)
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};
        for (uint32_t i = 0; i < view->argument_count; ++i)
            if (!xr_xir_scalar_argument(&view->arguments[i], function->parameters[i]))
                return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};
        for (uint32_t i = 0; i < view->argument_count; ++i)
            xr_xir_scalar_store(run.frame, layout->offsets[i], view->arguments[i].payload);
        state->initialized = true;
    }
    if (state->waiting) {
        state->waiting = false;
        if (view->inbox.status == XR_XIR_CALL_THROWN)
            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, view->inbox.value};
        if (view->inbox.status != XR_XIR_CALL_RETURNED)
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};
        if (state->expected == XR_XIR_UNIT ?
            (view->inbox.value.type != XR_XIR_UNIT || view->inbox.value.reserved || view->inbox.value.payload) :
            !xr_xir_scalar_argument(&view->inbox.value, state->expected))
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};
        if (state->destination != UINT32_MAX)
            xr_xir_scalar_store(run.frame, state->destination, view->inbox.value.payload);
    }
    XrXirAction action;
    XrXirRunStatus status = scalar_step(&run, state, &action);
    if (status != XR_XIR_RUN_OK)
        return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0,
            status == XR_XIR_RUN_OVERFLOW ? XR_XIR_CALL_OVERFLOW : XR_XIR_CALL_BAD_STATE}};
    return action;
}

XrXirStatus xr_xir_vm_bind(const XrXirArtifact *artifact, uint32_t function,
                          XrXirVmBinding *binding, XrXirCallEntry *entry) {
    if (!binding || !entry) return XR_XIR_BAD_STRUCTURE;
    *binding = (XrXirVmBinding) {NULL, 0};
    *entry = (XrXirCallEntry) {0};
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    if (function >= module->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_artifact_verify(artifact, NULL, NULL);
    if (status != XR_XIR_OK) return status;
    const XrXirFunction *body = &module->functions[function];
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(artifact, function);
    if (layout->frame_bytes > UINT32_MAX - sizeof(VmState)) return XR_XIR_BUDGET;
    *binding = (XrXirVmBinding) {artifact, function};
    *entry = (XrXirCallEntry) {XR_XIR_CALL_ABI_VERSION, body->parameters, body->parameter_count,
        body->result, (uint32_t) sizeof(VmState) + layout->frame_bytes, vm_resume, NULL, binding};
    return XR_XIR_OK;
}

XrXirRunStatus xr_xir_vm_run(const XrXirArtifact *artifact, uint32_t function,
                           XrXirRunContext *context, const XrXirScalar *arguments,
                           uint32_t argument_count, XrXirScalar *result) {
    if (!result)
        return XR_XIR_RUN_BAD_ARGUMENT;
    *result = (XrXirScalar) {0, 0, 0};
    if (!context || (argument_count && !arguments))
        return XR_XIR_RUN_BAD_ARGUMENT;
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED || function >= module->function_count)
        return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirStatus verified = xr_xir_artifact_verify(artifact, NULL, NULL);
    if (verified != XR_XIR_OK)
        return verified == XR_XIR_OUT_OF_MEMORY ? XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_BAD_ARTIFACT;
    const XrXirFunction *body = &module->functions[function];
    if (argument_count != body->parameter_count)
        return XR_XIR_RUN_BAD_ARGUMENT;
    for (uint32_t i = 0; i < argument_count; ++i)
        if (!xr_xir_scalar_argument(&arguments[i], body->parameters[i]))
            return XR_XIR_RUN_BAD_ARGUMENT;
    for (uint32_t i = 0; i < body->instruction_count; ++i)
        if (body->instructions[i].op == XR_XIR_CALL || body->instructions[i].op == XR_XIR_SUSPEND ||
            body->instructions[i].op == XR_XIR_THROW)
            return XR_XIR_RUN_BAD_ARTIFACT;
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(artifact, function);
    void *frame = NULL;
    XrXirRunStatus status = xr_xir_scalar_frame_begin(context, layout->frame_bytes, &frame);
    if (status != XR_XIR_RUN_OK)
        return status;
    for (uint32_t i = 0; i < argument_count; ++i)
        xr_xir_scalar_store(frame, layout->offsets[i], arguments[i].payload);
    ScalarRun run = {module, body, layout, frame};
    VmState state = {0};
    for (;;) {
        if (!xr_xir_scalar_step(context)) { status = XR_XIR_RUN_STEP_LIMIT; break; }
        XrXirAction action;
        status = scalar_step(&run, &state, &action);
        if (status != XR_XIR_RUN_OK) break;
        if (action.kind == XR_XIR_ACTION_RETURN) { *result = action.value; break; }
    }
    xr_xir_scalar_frame_end(context, layout->frame_bytes, frame);
    if (status != XR_XIR_RUN_OK)
        *result = (XrXirScalar) {0, 0, 0};
    return status;
}
