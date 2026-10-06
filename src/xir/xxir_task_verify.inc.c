/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_verify.inc.c - Authentic direct Task boundary obligations
 *
 * KEY CONCEPT:
 *   A concrete caller cannot repair missing definition-site worker bounds.
 */
static XrXirStatus task_go_shape(const XrXirModule *module, const XrXirInstruction *op,
    XrXirCompileContext *remaining) {
    const XrXirTypeNode *task = xr_xir_type_node(module->types, op->type);
    if (!xr_xir_function_has_go_role(module, (uint32_t)op->immediate) ||
        !task || task->kind != XR_XIR_TYPE_TASK) return XR_XIR_BAD_TYPE;
    uint32_t callee = (uint32_t)op->immediate;
    const XrXirFunction *function = &module->functions[callee];
    if (module->stage == XR_XIR_LOWERED) {
        if (function->result != XR_XIR_UNIT && function->result != XR_XIR_BOOL &&
            function->result != XR_XIR_I64 && function->result != XR_XIR_STRING)
            return XR_XIR_UNSUPPORTED;
        for (uint32_t p = 0; p < function->parameter_count; ++p) {
            if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
            if (!xr_xir_task_parameter_supported(module->types, function->parameters[p]))
                return XR_XIR_UNSUPPORTED;
        }
    }
    XrXirProofContext proof = {module, {XR_XIR_CONTEXT_FUNCTION, callee, 0}};
    XrXirStatus status = xr_xir_compile_type_markers_prove(remaining, &proof, function->result,
        XR_XIR_CONSTRAINT_SENDABLE);
    for (uint32_t p = 0; status == XR_XIR_OK && p < function->parameter_count; ++p) {
        if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
        status = xr_xir_compile_type_markers_prove(remaining, &proof, function->parameters[p],
            XR_XIR_CONSTRAINT_SENDABLE);
    }
    return status;
}
static XrXirType task_await_result(const XrXirModule *module, const XrXirFunction *function,
    const XrXirInstruction *op) {
    return xr_xir_task_element(module->types, xr_xir_operand_type(function, op->args[0]));
}
