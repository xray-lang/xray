/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_hash.inc.c - Hash instructions use the shared active-call operations
 */
static XrXirRunStatus vm_hash_step(ScalarRun *run, VmState *state,
    const XrXirInstruction *op, uint32_t destination, XrXirAction *action) {
    if (!run->view) return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirValue output = {0}, key = {0}, value = {0};
    XrXirFaultDetail fault = {0}; XrXirCallStatus status;
    if (op->op == XR_XIR_MAP_NEW || op->op == XR_XIR_SET_NEW) {
        for (uint32_t i = 0; i < op->args[1]; ++i)
            state->arguments[i] = vm_value_operand(run, run->function->operands[op->args[0] + i]);
        status = xr_xir_instance_hash_new(run->view, op->type,
            op->args[1] ? state->arguments : NULL, op->args[1], &output);
    } else {
        XrXirValueReceiver receiver = {0}; XrXirValuePath path = {0};
        status = vm_path_receiver(run, state, op, &receiver, &path);
        const uint32_t *args = op->op == XR_XIR_MAP_SET ? &run->function->operands[op->args[0]] : op->args;
        bool keyed = op->op >= XR_XIR_SET_UNION ||
            (op->op < XR_XIR_HASH_KEYS && op->op != XR_XIR_HASH_LEN && op->op != XR_XIR_HASH_CLEAR);
        if (keyed) key = vm_value_operand(run, args[1]);
        if (op->op == XR_XIR_MAP_SET) value = vm_value_operand(run, args[2]);
        XrXirHashRequest request = {op->op, op->type, (uint32_t)(op - run->function->instructions),
            &receiver, &path, keyed ? &key : NULL, op->op == XR_XIR_MAP_SET ? &value : NULL};
        if (status == XR_XIR_CALL_READY) status = xr_xir_instance_hash_apply(run->view, &request, &output, &fault);
    }
    if (status != XR_XIR_CALL_READY) {
        *action = (XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0,
            {XR_XIR_I64, 0, status}, {fault, {0}}, 0};
        return XR_XIR_RUN_OK;
    }
    if (xr_xir_type_is_owned(run->module->types, op->type))
        xr_xir_owned_slot_move(run->frame, destination, &output);
    else if (op->type != XR_XIR_UNIT) xr_xir_scalar_store(run->frame, destination, output.payload);
    return XR_XIR_RUN_OK;
}
