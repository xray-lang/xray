/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_pending.inc.c - Publish primitive facts from the actual VM call
 */
static XrXirValueStatus vm_debug_pending(const XirVmDebugPoint *point,const XrXirAction *action,
    XirVmDebugPendingCall *output) {
    if (!xir_call_vm_view_current(point->view) || point->view->state!=point->state || action->kind!=XR_XIR_ACTION_CALL)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirModule *module=xr_xir_compile_artifact_module(point->binding->artifact);
    uint32_t function=point->binding->function;
    if (!module || function>=module->function_count || action->callee>=module->function_count ||
        xr_xir_call_current_entry(point->view->activation)!=function) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirFunction *body=&module->functions[function];const VmState *state=point->state;
    if (point->instruction>=body->instruction_count || point->frame!=(const void *)(state+1))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirInstruction *op=&body->instructions[point->instruction];
    bool invoked=op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_INDIRECT;
    bool indirect=op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT;
    if ((!invoked && op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_INDIRECT) ||
        !state->initialized || !state->waiting || state->cleanup_waiting || state->leaving ||
        state->instruction!=(uint64_t)point->instruction+1 || state->invoke!=(invoked ? point->instruction+1 : 0) ||
        state->expected!=op->type || state->destination!=point->layout->offsets[body->parameter_count+point->instruction] ||
        (!indirect && (op->immediate<0 || (uint64_t)op->immediate!=action->callee))) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=(XirVmDebugPendingCall){point->instruction,action->callee,invoked};return XR_XIR_VALUE_OK;
}
