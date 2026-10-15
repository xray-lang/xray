/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_dispatch.inc.c - Compiler step dispatch with explicit fault results
 *
 * KEY CONCEPT:
 *   A language fault reaches the same frame unwind path regardless of which
 *   value primitive produced it. Resource failures never become catchable.
 */
static XrXirCtfeStatus ctfe_call_step(CtfeCallExecution *execution,XirScalarCoreAction *action) {
    CtfeCallFrame *frame=execution->top;ScalarRun *run=&frame->run;
    const XrXirInstruction *op=&run->function->instructions[frame->state.instruction];
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_INDIRECT) {
        if (!scalar_compile_work(run,2)) return XR_XIR_CTFE_BUDGET;
        return ctfe_call_enter(execution,op,false);
    }
    uint64_t work=0;if (!ctfe_opcode_work(op->op,&work)) return XR_XIR_CTFE_NOT_SUPPORTED;
    if (!scalar_compile_work(run,work+1)) return XR_XIR_CTFE_BUDGET;
    XrXirCtfeStatus flow=ctfe_tuple_flow_work(run,op,&execution->owned);if (flow!=XR_XIR_CTFE_OK) return flow;
    if (op->op==XR_XIR_THROW || op->op==XR_XIR_CLEANUP_ERROR) {
        XrXirValue error=ctfe_nominal_operand(run,op->args[0]);
        return ctfe_error_begin(execution,&error,op->op==XR_XIR_THROW?CTFE_EXIT_ERROR:CTFE_EXIT_LOCAL_ERROR);
    }
    if (op->op==XR_XIR_ERROR_ERASE || op->op==XR_XIR_ERROR_IS || op->op==XR_XIR_ERROR_NARROW)
        return ctfe_error_value_step(run,&frame->state,&execution->owned);
    if (op->op==XR_XIR_PANIC_CODE || op->op==XR_XIR_PANIC_MESSAGE) return ctfe_panic_value_step(run,&frame->state,&execution->owned);
    if (op->op==XR_XIR_ASSERT_CONDITION) {
        if (!xr_xir_scalar_load(run->frame,run->layout->offsets[op->args[0]])) return XR_XIR_CTFE_ASSERTION_FAILURE;
        ++frame->state.instruction;return XR_XIR_CTFE_OK;
    }
    if (op->op==XR_XIR_MATCH_FAIL) return XR_XIR_CTFE_MATCH_FAILURE;
    if (op->op==XR_XIR_CLEANUP_REGISTER || op->op==XR_XIR_CLEANUP_LEAVE) return ctfe_cleanup_step(execution,op);
    if (op->op==XR_XIR_FUNCTION_REF || op->op==XR_XIR_FUNCTION_WEAKEN) return ctfe_function_step(run,&frame->state,&execution->owned,frame->function);
    if (ctfe_float_opcode(run,op)) return ctfe_float_step(run,&frame->state.instruction);
    if (ctfe_owned_equal_opcode(run,op)) return ctfe_owned_equal_step(run,&frame->state.instruction,&execution->owned);
    if (ctfe_nominal_opcode(op->op)) return ctfe_nominal_step(run,&frame->state,&execution->owned);
    if (ctfe_array_opcode(op->op)) return ctfe_array_step(run,&frame->state,&execution->owned);
    if (ctfe_nullable_opcode(op)) return ctfe_nullable_step(run,&frame->state.instruction,&execution->owned);
    if (ctfe_tuple_opcode(op->op)) return ctfe_tuple_step(run,&frame->state,&execution->owned);
    if (ctfe_cell_opcode(op->op)) return ctfe_cell_step(run,&frame->state.instruction,&execution->owned);
    if (ctfe_string_opcode(run,op)) return ctfe_string_step(run,&frame->state.instruction,&execution->owned);
    XrXirRunStatus ran=xir_scalar_core_step(run,&frame->state.instruction,action);
    return ran==XR_XIR_RUN_OK?XR_XIR_CTFE_OK:ran==XR_XIR_RUN_STEP_LIMIT?XR_XIR_CTFE_BUDGET:
        ran==XR_XIR_RUN_DIVIDE_BY_ZERO?XR_XIR_CTFE_DIVIDE_BY_ZERO:ran==XR_XIR_RUN_NUMERIC_RANGE?XR_XIR_CTFE_NUMERIC_RANGE:XR_XIR_CTFE_BAD_ARTIFACT;
}
