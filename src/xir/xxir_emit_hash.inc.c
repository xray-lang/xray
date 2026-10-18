/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_hash.inc.c - Portable C11 calls for the common hash instructions
 */
static void emit_hash_step(CBuffer *buffer, const XrXirFunction *function,
    const XrXirInstruction *op, const XrXirFunctionLayout *layout, uint32_t destination, uint32_t index) {
    append(buffer, "        { XrXirValue value = {0}; XrXirFaultDetail fault = {0};\n");
    if (op->op == XR_XIR_MAP_NEW || op->op == XR_XIR_SET_NEW) {
        for (uint32_t i = 0; i < op->args[1] && emit_work(buffer, 1); ++i) {
            append(buffer, "        state->arguments[%u] = ", i);
            emit_value(buffer, function, layout, function->operands[op->args[0] + i]);
            append(buffer, ";\n");
        }
        append(buffer, "        XrXirCallStatus status = xr_xir_instance_hash_new(view, (XrXirType)%uu, %s, %uu, &value);\n",
            (uint32_t)op->type, op->args[1] ? "state->arguments" : "NULL", op->args[1]);
    } else {
        emit_path_receiver(buffer, function, layout, op);
        const uint32_t *args = op->op == XR_XIR_MAP_SET ? &function->operands[op->args[0]] : op->args;
        bool keyed = op->op >= XR_XIR_SET_UNION ||
            (op->op < XR_XIR_HASH_KEYS && op->op != XR_XIR_HASH_LEN && op->op != XR_XIR_HASH_CLEAR);
        if (keyed) {
            append(buffer, "        XrXirValue key = "); emit_value(buffer, function, layout, args[1]); append(buffer, ";\n");
        }
        if (op->op == XR_XIR_MAP_SET) {
            append(buffer, "        XrXirValue element = "); emit_value(buffer, function, layout, args[2]); append(buffer, ";\n");
        }
        append(buffer, "        XrXirHashRequest request = {(XrXirOp)%uu, (XrXirType)%uu, %uu, &receiver, &path, %s, %s};\n",
            (uint32_t)op->op, (uint32_t)op->type, index, keyed ? "&key" : "NULL", op->op == XR_XIR_MAP_SET ? "&element" : "NULL");
        append(buffer, "        if (status == XR_XIR_CALL_READY) status = xr_xir_instance_hash_apply(view, &request, &value, &fault);\n");
    }
    append(buffer, "        if (status != XR_XIR_CALL_READY) ");
    emit_fault_return(buffer, function, layout, index,
        "(XrXirAction){XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {fault, {0}}, 0}");
    append(buffer, "\n");
    if (xr_xir_type_is_owned(buffer->types, op->type))
        append(buffer, "        xr_xir_owned_slot_move(state->frame, %uu, &value);\n", destination);
    else if (op->type != XR_XIR_UNIT) append(buffer, "        xr_xir_scalar_store(state->frame, %uu, value.payload);\n", destination);
    append(buffer, "        }\n        return (XrXirAction){XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n");
}
