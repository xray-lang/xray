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
    const XrXirFunction *function;
    const XrXirFunctionLayout *layout;
    XrXirRunContext *context;
    void *frame;
} ScalarRun;

static XrXirRunStatus execute_scalar(ScalarRun *run, XrXirScalar *result) {
    uint32_t instruction = 0;
    for (;;) {
        if (!xr_xir_scalar_step(run->context))
            return XR_XIR_RUN_STEP_LIMIT;
        const XrXirInstruction *op = &run->function->instructions[instruction];
        uint32_t result_id = run->function->parameter_count + instruction;
        uint32_t next = instruction + 1;
        int64_t value = 0;
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
            if (status != XR_XIR_RUN_OK)
                return status;
            break;
        }
        case XR_XIR_EQ_I64:
            value = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]) ==
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
        case XR_XIR_RETURN:
            result->type = (uint32_t) run->function->result;
            if (run->function->result != XR_XIR_UNIT)
                result->payload = xr_xir_scalar_load(run->frame, run->layout->offsets[op->args[0]]);
            return XR_XIR_RUN_OK;
        default:
            return XR_XIR_RUN_BAD_ARTIFACT;
        }
        if (op->type != XR_XIR_UNIT)
            xr_xir_scalar_store(run->frame, run->layout->offsets[result_id], value);
        instruction = next;
    }
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
    const XrXirFunctionLayout *layout = xr_xir_artifact_layout(artifact, function);
    void *frame = NULL;
    XrXirRunStatus status = xr_xir_scalar_frame_begin(context, layout->frame_bytes, &frame);
    if (status != XR_XIR_RUN_OK)
        return status;
    for (uint32_t i = 0; i < argument_count; ++i)
        xr_xir_scalar_store(frame, layout->offsets[i], arguments[i].payload);
    ScalarRun run = {body, layout, context, frame};
    status = execute_scalar(&run, result);
    xr_xir_scalar_frame_end(context, layout->frame_bytes, frame);
    if (status != XR_XIR_RUN_OK)
        *result = (XrXirScalar) {0, 0, 0};
    return status;
}
