/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_path.inc.c - Frame-bounded logical path assembly
 */
static XrXirCallStatus vm_path_receiver(const ScalarRun *run, VmState *state,
    const XrXirInstruction *op, XrXirValueReceiver *receiver, XrXirValuePath *path) {
    XrXirValueAdmission *admission = xr_xir_call_admission(run->view);
    if (!admission) return XR_XIR_CALL_BAD_STATE;
    const uint32_t *args = op->op == XR_XIR_ARRAY_SET ? &run->function->operands[op->args[0]] : op->args;
    uint32_t id = args[0], begin = run->layout->path_count;
    if (op->op == XR_XIR_ARRAY_GET || op->op == XR_XIR_ARRAY_SET || op->op == XR_XIR_STRUCT_SET) {
        if (!begin) return XR_XIR_CALL_BAD_STATE;
        state->path_steps[--begin] = (XrXirValuePathStep){
            op->op == XR_XIR_STRUCT_SET ? XR_XIR_PATH_FIELD : XR_XIR_PATH_INDEX,
            xr_xir_operand_type(run->function, id),
            op->op == XR_XIR_STRUCT_SET ? op->immediate : vm_value_operand(run, args[1]).payload};
    }
    for (;;) {
        XrXirPlaceKind kind = xr_xir_place_kind(run->function, id);
        if (kind != XR_XIR_PLACE_FIELD && kind != XR_XIR_PLACE_INDEX) break;
        if (!begin) return XR_XIR_CALL_BAD_STATE;
        const XrXirInstruction *projection = &run->function->instructions[id - run->function->parameter_count];
        state->path_steps[--begin] = (XrXirValuePathStep){
            kind == XR_XIR_PLACE_FIELD ? XR_XIR_PATH_FIELD : XR_XIR_PATH_INDEX,
            xr_xir_operand_type(run->function, projection->args[0]),
            kind == XR_XIR_PLACE_FIELD ? projection->immediate : vm_value_operand(run, projection->args[1]).payload};
        id = projection->args[0];
    }
    uint32_t count = run->layout->path_count - begin;
    if (admission->work < count) return XR_XIR_CALL_LIMIT;
    admission->work -= count;
    *receiver = vm_value_receiver(run, id);
    *path = (XrXirValuePath){count ? state->path_steps + begin : NULL, count};
    return XR_XIR_CALL_READY;
}
static XrXirRunStatus vm_path_step(ScalarRun *run, VmState *state,
    const XrXirInstruction *op, uint32_t destination, XrXirAction *action) {
    XrXirValueReceiver receiver = {0}; XrXirValuePath path = {0};
    XrXirValue output = {0}; XrXirFaultDetail fault = {0};
    XrXirCallStatus status = vm_path_receiver(run, state, op, &receiver, &path);
    if (status == XR_XIR_CALL_READY) {
        if (op->op == XR_XIR_PLACE_READ || op->op == XR_XIR_ARRAY_GET)
            status = xr_xir_instance_path_read(run->view, &receiver, &path, &output, &fault);
        else if (op->op == XR_XIR_ARRAY_LEN)
            status = xr_xir_instance_path_length(run->view, &receiver, &path, &output, &fault);
        else {
            uint32_t id = op->op == XR_XIR_ARRAY_SET ? run->function->operands[op->args[0] + 2] : op->args[1];
            XrXirValue value = vm_value_operand(run, id);
            status = op->op == XR_XIR_ARRAY_PUSH ?
                xr_xir_instance_path_push(run->view, &receiver, &path, &value, &fault) :
                xr_xir_instance_path_write(run->view, &receiver, &path, &value, &fault);
        }
    }
    if (status != XR_XIR_CALL_READY) {
        *action = (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0,
            {XR_XIR_I64, 0, status}, {fault, {0}}, 0};
        return XR_XIR_RUN_OK;
    }
    if (xr_xir_type_is_owned(run->module->types, op->type)) xr_xir_owned_slot_move(run->frame, destination, &output);
    else if (op->type != XR_XIR_UNIT) xr_xir_scalar_store(run->frame, destination, output.payload);
    return XR_XIR_RUN_OK;
}
