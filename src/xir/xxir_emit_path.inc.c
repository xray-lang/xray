/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_path.inc.c - Native frame-bounded logical path assembly
 */
static void emit_path_component(CBuffer *buffer, const XrXirFunctionLayout *layout,
    uint32_t slot, XrXirType container, bool field, int64_t selector) {
    append(buffer, "        state->path_steps[%u] = (XrXirValuePathStep) {%s, (XrXirType) %uu, ",
        slot, field ? "XR_XIR_PATH_FIELD" : "XR_XIR_PATH_INDEX", (uint32_t) container);
    if (field) emit_constant(buffer, selector);
    else append(buffer, "xr_xir_scalar_load(state->frame, %uu)", layout->offsets[(uint32_t) selector]);
    append(buffer, "};\n");
}
static void emit_path_receiver(CBuffer *buffer, const XrXirFunction *function,
    const XrXirFunctionLayout *layout, const XrXirInstruction *op) {
    const uint32_t *args = op->op == XR_XIR_ARRAY_SET ? &function->operands[op->args[0]] : op->args;
    uint32_t id = args[0], begin = layout->path_count;
    if (op->op == XR_XIR_ARRAY_GET || op->op == XR_XIR_ARRAY_SET || op->op == XR_XIR_STRUCT_SET) {
        if (!begin) { emit_reject(buffer, XR_XIR_BAD_LAYOUT); return; }
        emit_path_component(buffer, layout, --begin, xr_xir_operand_type(function, id),
            op->op == XR_XIR_STRUCT_SET, op->op == XR_XIR_STRUCT_SET ? op->immediate : args[1]);
    }
    for (;;) {
        if (!emit_work(buffer, 1)) return;
        XrXirPlaceKind kind = xr_xir_place_kind(function, id);
        if (kind != XR_XIR_PLACE_FIELD && kind != XR_XIR_PLACE_INDEX) break;
        if (!begin) { emit_reject(buffer, XR_XIR_BAD_LAYOUT); return; }
        const XrXirInstruction *projection = &function->instructions[id - function->parameter_count];
        emit_path_component(buffer, layout, --begin, xr_xir_operand_type(function, projection->args[0]),
            kind == XR_XIR_PLACE_FIELD,
            kind == XR_XIR_PLACE_FIELD ? projection->immediate : projection->args[1]);
        id = projection->args[0];
    }
    uint32_t count = layout->path_count - begin;
    emit_value_receiver(buffer, function, layout, id);
    if (count) append(buffer, "        XrXirValuePath path = {state->path_steps + %uu, %uu};\n", begin, count);
    else append(buffer, "        XrXirValuePath path = {NULL, 0};\n");
    append(buffer, "        XrXirValueAdmission *admission = xr_xir_call_admission(view);\n"
        "        XrXirCallStatus status = !admission ? XR_XIR_CALL_BAD_STATE :\n"
        "            admission->work < %uu ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY;\n"
        "        if (status == XR_XIR_CALL_READY) admission->work -= %uu;\n", count, count);
}
static void emit_path_step(CBuffer *buffer, const XrXirFunction *function,
    const XrXirInstruction *op, const XrXirFunctionLayout *layout, uint32_t destination, uint32_t index) {
    append(buffer, "        { XrXirFaultDetail fault = {0};\n");
    bool read = op->op == XR_XIR_CELL_PROJECT || op->op == XR_XIR_PLACE_READ || op->op == XR_XIR_ARRAY_GET ||
        op->op == XR_XIR_ARRAY_LEN || op->op == XR_XIR_ARRAY_CAPACITY;
    if (read) append(buffer, "        XrXirValue value = {0};\n");
    else {
        uint32_t id = op->op == XR_XIR_ARRAY_SET ? function->operands[op->args[0] + 2] : op->args[1];
        append(buffer, "        XrXirValue value = "); emit_value(buffer, function, layout, id);
        append(buffer, ";\n");
    }
    emit_path_receiver(buffer, function, layout, op);
    const char *operation = read ? (op->op == XR_XIR_ARRAY_CAPACITY ? "capacity" :
        op->op == XR_XIR_ARRAY_LEN ? "length" : "read") :
        op->op == XR_XIR_ARRAY_PUSH ? "push" : "write";
    if (op->op == XR_XIR_CELL_PROJECT)
        append(buffer, "        if (status == XR_XIR_CALL_READY)\n"
            "            status = xr_xir_instance_cell_project(view, (XrXirType)%uu, &receiver, &path, &value);\n"
            "        if (status != XR_XIR_CALL_READY) ", (uint32_t)op->type);
    else append(buffer, "        if (status == XR_XIR_CALL_READY)\n"
        "            status = xr_xir_instance_path_%s(view, &receiver, &path, &value, &fault);\n"
        "        if (status != XR_XIR_CALL_READY) ", operation);
    emit_fault_return(buffer, function, layout, index,
        "(XrXirAction) {XR_XIR_ACTION_FAULT, 0, NULL, 0, {XR_XIR_I64, 0, status}, {fault, {0}}, 0}");
    append(buffer, "\n");
    if (xr_xir_type_is_owned(buffer->types, op->type))
        append(buffer, "        xr_xir_owned_slot_move(state->frame, %uu, &value);\n", destination);
    else if (op->type != XR_XIR_UNIT)
        append(buffer, "        xr_xir_scalar_store(state->frame, %uu, value.payload);\n", destination);
    append(buffer, "        }\n        return (XrXirAction) {XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n");
}
