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
#include "../base/xmalloc.h"

typedef struct ScalarRun {
    const XrXirModule *module;
    const XrXirFunction *function;
    const XrXirFunctionLayout *layout;
    void *frame;
    XrXirCallView *view;
} ScalarRun;

typedef struct VmState {
    uint32_t instruction, destination;
    XrXirType expected;
    bool initialized, waiting;
    XrXirValue arguments[2];
} VmState;

static XrXirRunStatus string_status(XrXirValueStatus status) {
    if (status == XR_XIR_VALUE_OK) return XR_XIR_RUN_OK;
    if (status == XR_XIR_VALUE_OOM) return XR_XIR_RUN_OUT_OF_MEMORY;
    if (status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT)
        return XR_XIR_RUN_FRAME_LIMIT;
    return XR_XIR_RUN_BAD_ARTIFACT;
}

static XrXirRunStatus instance_step(ScalarRun *run, const XrXirInstruction *op, uint32_t destination) {
    XrXirValue value = {0};
    XrXirCallStatus status = XR_XIR_CALL_READY;
    switch (op->op) {
    case XR_XIR_CONST_STRING:
        status = xr_xir_instance_literal(run->view, (uint32_t) op->immediate, &value); break;
    case XR_XIR_SLOT_LOAD:
        status = xr_xir_instance_slot_read(run->view, (uint32_t) op->immediate, &value); break;
    case XR_XIR_SLOT_INIT:
    case XR_XIR_SLOT_STORE:
        value = (XrXirValue) {(uint32_t) run->module->declarations->slots[op->immediate].type, 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])};
        status = xr_xir_instance_slot_write(run->view, (uint32_t) op->immediate, &value, op->op == XR_XIR_SLOT_INIT);
        break;
    case XR_XIR_ATOMIC_I64_NEW:
        status = xr_xir_instance_atomic(run->view,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]), &value); break;
    case XR_XIR_ATOMIC_I64_LOAD:
    case XR_XIR_ATOMIC_I64_FETCH_ADD: {
        XrXirValue atomic = {XR_XIR_ATOMIC_I64, 0,
            xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]])};
        value.type = XR_XIR_I64;
        bool valid = op->op == XR_XIR_ATOMIC_I64_LOAD ? xr_xir_atomic_i64_load(&atomic, &value.payload) :
            xr_xir_atomic_i64_fetch_add(&atomic,
                xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]), &value.payload);
        if (!valid) status = XR_XIR_CALL_BAD_STATE;
        break;
    }
    default: return XR_XIR_RUN_BAD_ARTIFACT;
    }
    if (status != XR_XIR_CALL_READY) return status == XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :
        status == XR_XIR_CALL_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : XR_XIR_RUN_BAD_ARTIFACT;
    if (xr_xir_type_is_owned(op->type)) xr_xir_owned_slot_move(run->frame, destination, &value);
    else if (op->type != XR_XIR_UNIT) xr_xir_scalar_store(run->frame, destination, value.payload);
    return XR_XIR_RUN_OK;
}

static XrXirRunStatus scalar_step(ScalarRun *run, VmState *state, XrXirAction *action) {
    uint32_t instruction = state->instruction;
    const XrXirInstruction *op = &run->function->instructions[instruction];
    uint32_t result_id = run->function->parameter_count + instruction;
    uint32_t next = instruction + 1;
    int64_t value = 0;
    *action = (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0, 0, 0}};
    if (op->op >= XR_XIR_CONST_STRING && op->op <= XR_XIR_ATOMIC_I64_FETCH_ADD) {
        state->instruction = next;
        return instance_step(run, op, run->layout->offsets[result_id]);
    }
    switch (op->op) {
    case XR_XIR_CONST_BOOL:
    case XR_XIR_CONST_I64:
        value = op->immediate;
        break;
    case XR_XIR_SCALAR_COPY:
        value = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        break;
    case XR_XIR_OWNED_RETAIN:
    case XR_XIR_CONCAT_STRING: {
        int64_t left = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
        XrXirValueStatus status = op->op == XR_XIR_OWNED_RETAIN ?
            xr_xir_owned_slot_copy(run->frame, run->layout->offsets[result_id], op->type, left) :
            xr_xir_string_slot_concat(run->frame, run->layout->offsets[result_id], left,
                xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[1]]));
        state->instruction = next;
        return string_status(status);
    }
    case XR_XIR_OUTPUT:
    case XR_XIR_PRINT: {
        uint32_t count = op->op == XR_XIR_PRINT ? (uint32_t) op->immediate : 1;
        for (uint32_t p = 0; p < count; ++p) {
            uint32_t id = op->args[p];
            XrXirType type = id < run->function->parameter_count ? run->function->parameters[id] :
                run->function->instructions[id - run->function->parameter_count].type;
            state->arguments[p] = (XrXirValue) {(uint32_t) type, 0,
                xr_xir_scalar_load(run->frame, run->layout->offsets[id])};
        }
        *action = (XrXirAction) {XR_XIR_ACTION_OUTPUT, op->op == XR_XIR_PRINT ? XR_XIR_OUTPUT_LINE :
            (uint32_t) op->immediate, state->arguments, count, {0}};
        state->instruction = next;
        return XR_XIR_RUN_OK;
    }
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
            state->arguments[i] = (XrXirValue) {(uint32_t) callee->parameters[i], 0,
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
    ScalarRun run = {module, function, layout, state + 1, view};
    if (!state->initialized) {
        if (view->argument_count != function->parameter_count)
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};
        for (uint32_t i = 0; i < view->argument_count; ++i)
            if (!xr_xir_value_argument(&view->arguments[i], function->parameters[i]))
                return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};
        for (uint32_t i = 0; i < view->argument_count; ++i) {
            if (xr_xir_type_is_owned(function->parameters[i])) {
                if (xr_xir_owned_slot_copy(run.frame, layout->offsets[i], function->parameters[i], view->arguments[i].payload) != XR_XIR_VALUE_OK)
                    return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}};
            } else xr_xir_scalar_store(run.frame, layout->offsets[i], view->arguments[i].payload);
        }
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
            !xr_xir_value_argument(&view->inbox.value, state->expected))
            return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {0, 0, 0}};
        if (xr_xir_type_is_owned(state->expected)) {
            if (xr_xir_owned_slot_copy(run.frame, state->destination, state->expected, view->inbox.value.payload) != XR_XIR_VALUE_OK)
                return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, XR_XIR_CALL_LIMIT}};
        } else if (state->destination != UINT32_MAX)
            xr_xir_scalar_store(run.frame, state->destination, view->inbox.value.payload);
    }
    XrXirAction action;
    XrXirRunStatus status = scalar_step(&run, state, &action);
    if (status != XR_XIR_RUN_OK)
        return (XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0,
            status == XR_XIR_RUN_OVERFLOW ? XR_XIR_CALL_OVERFLOW :
            status == XR_XIR_RUN_OUT_OF_MEMORY ? XR_XIR_CALL_OOM :
            status == XR_XIR_RUN_FRAME_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_STATE}};
    return action;
}

static void vm_cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    (void) reason;
    const XrXirVmBinding *binding = view->environment;
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(binding->artifact, binding->function);
    VmState *state = view->state;
    for (uint32_t i = layout->owned_count; i > 0; --i)
        xr_xir_owned_slot_clear(state + 1, layout->owned_offsets[i - 1]);
}

static XrXirStatus bind_verified(const XrXirArtifact *artifact, uint32_t function,
                                 XrXirVmBinding *binding, XrXirCallEntry *entry) {
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    const XrXirFunction *body = &module->functions[function];
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(artifact, function);
    if (layout->frame_bytes > UINT32_MAX - sizeof(VmState)) return XR_XIR_BUDGET;
    *binding = (XrXirVmBinding) {artifact, function};
    *entry = (XrXirCallEntry) {XR_XIR_CALL_ABI_VERSION, body->parameters, body->parameter_count,
        body->result, (uint32_t) sizeof(VmState) + layout->frame_bytes, vm_resume, vm_cleanup, binding};
    return XR_XIR_OK;
}

XrXirStatus xr_xir_vm_bind(const XrXirArtifact *artifact, uint32_t function,
                          XrXirVmBinding *binding, XrXirCallEntry *entry) {
    if (!binding || !entry) return XR_XIR_BAD_STRUCTURE;
    *binding = (XrXirVmBinding) {NULL, 0}; *entry = (XrXirCallEntry) {0};
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    if (function >= module->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_artifact_verify(artifact, NULL, NULL);
    return status == XR_XIR_OK ? bind_verified(artifact, function, binding, entry) : status;
}

typedef struct VmProgramOwner { XrXirArtifact *artifact; XrXirVmBinding *bindings; } VmProgramOwner;
static void vm_program_release(void *pointer) {
    VmProgramOwner *owner = pointer;
    xr_xir_artifact_free(owner->artifact); xr_free(owner->bindings); xr_free(owner);
}
XrXirStatus xr_xir_vm_program_take(XrXirArtifact **artifact, uint64_t byte_limit, XrXirProgram **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
    const XrXirModule *module = xr_xir_artifact_module(*artifact);
    if (!module || module->stage != XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    if (!module->declarations) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_artifact_verify(*artifact, NULL, NULL);
    if (status != XR_XIR_OK) return status;
    uint64_t bytes = sizeof(VmProgramOwner) + (uint64_t) module->function_count *
        (sizeof(XrXirVmBinding) + sizeof(XrXirCallEntry));
    if (bytes > byte_limit || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    VmProgramOwner *owner = xr_calloc(1, sizeof(*owner));
    if (!owner) return XR_XIR_OUT_OF_MEMORY;
    owner->bindings = xr_calloc(module->function_count, sizeof(*owner->bindings));
    XrXirCallEntry *entries = xr_calloc(module->function_count, sizeof(*entries));
    if (!owner->bindings || !entries) { status = XR_XIR_OUT_OF_MEMORY; goto finish; }
    for (uint32_t i = 0; i < module->function_count; ++i) {
        status = bind_verified(*artifact, i, &owner->bindings[i], &entries[i]);
        if (status != XR_XIR_OK) goto finish;
    }
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, *xr_xir_artifact_target(*artifact),
        entries, module->function_count, module->declarations, {owner, vm_program_release}};
    status = xr_xir_program_seal(&spec, byte_limit - bytes, output);
    if (status == XR_XIR_OK) { owner->artifact = *artifact; *artifact = NULL; }
 finish:
    xr_free(entries);
    if (status != XR_XIR_OK) vm_program_release(owner);
    return status;
}

XrXirRunStatus xr_xir_vm_run(const XrXirArtifact *artifact, uint32_t function,
                           XrXirRunContext *context, const XrXirValue *arguments,
                           uint32_t argument_count, XrXirValue *result) {
    if (!result)
        return XR_XIR_RUN_BAD_ARGUMENT;
    *result = (XrXirValue) {0, 0, 0};
    if (!context || (argument_count && !arguments))
        return XR_XIR_RUN_BAD_ARGUMENT;
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    if (!module || module->stage != XR_XIR_LOWERED || function >= module->function_count)
        return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirStatus verified = xr_xir_artifact_verify(artifact, NULL, NULL);
    if (verified != XR_XIR_OK)
        return verified == XR_XIR_OUT_OF_MEMORY ? XR_XIR_RUN_OUT_OF_MEMORY : XR_XIR_RUN_BAD_ARTIFACT;
    const XrXirFunction *body = &module->functions[function];
    if (xr_xir_type_is_owned(body->result)) return XR_XIR_RUN_BAD_ARTIFACT;
    for (uint32_t i = 0; i < body->parameter_count; ++i)
        if (xr_xir_type_is_owned(body->parameters[i])) return XR_XIR_RUN_BAD_ARTIFACT;
    if (argument_count != body->parameter_count)
        return XR_XIR_RUN_BAD_ARGUMENT;
    for (uint32_t i = 0; i < argument_count; ++i)
        if (!xr_xir_value_argument(&arguments[i], body->parameters[i]))
            return XR_XIR_RUN_BAD_ARGUMENT;
    for (uint32_t i = 0; i < body->instruction_count; ++i)
        if (body->instructions[i].op == XR_XIR_CALL || body->instructions[i].op == XR_XIR_SUSPEND ||
            body->instructions[i].op == XR_XIR_THROW || xr_xir_type_is_owned(body->instructions[i].type) ||
            body->instructions[i].op == XR_XIR_OUTPUT || body->instructions[i].op == XR_XIR_PRINT)
            return XR_XIR_RUN_BAD_ARTIFACT;
    for (uint32_t i = 0; i < body->instruction_count; ++i)
        if (body->instructions[i].op >= XR_XIR_CONST_STRING &&
            body->instructions[i].op <= XR_XIR_ATOMIC_I64_FETCH_ADD) return XR_XIR_RUN_BAD_ARTIFACT;
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(artifact, function);
    void *frame = NULL;
    XrXirRunStatus status = xr_xir_scalar_frame_begin(context, layout->frame_bytes, &frame);
    if (status != XR_XIR_RUN_OK)
        return status;
    for (uint32_t i = 0; i < argument_count; ++i)
        xr_xir_scalar_store(frame, layout->offsets[i], arguments[i].payload);
    ScalarRun run = {module, body, layout, frame, NULL};
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
        *result = (XrXirValue) {0, 0, 0};
    return status;
}
