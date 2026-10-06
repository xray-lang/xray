/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_invoke_verify.inc.c - Dedicated successor ownership of call results
 */
static XrXirStatus invoke_edges(const Graph *graph, const XrXirFunction *function,
    VerifyContext *context) {
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        if (!xir_compile_work(&context->remaining, 1)) return XR_XIR_BUDGET;
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op == XR_XIR_INVOKE || op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_INVOKE_DEFAULT ||
            op->op == XR_XIR_TASK_AWAIT) {
            XrXirType result = op->op == XR_XIR_TASK_AWAIT ?
                task_await_result(context->module, function, op) : op->type;
            if (op->op == XR_XIR_TASK_AWAIT && !result) return XR_XIR_BAD_TYPE;
            if (op->targets[0] == op->targets[1]) return XR_XIR_BAD_STRUCTURE;
            for (uint32_t edge = 0; edge < 2; ++edge) {
                uint32_t block = op->targets[edge], incoming = graph->head[block];
                if (incoming == UINT32_MAX || graph->next[incoming] != UINT32_MAX ||
                    graph->predecessor[incoming] != graph->owner[i]) return XR_XIR_BAD_STRUCTURE;
                const XrXirInstruction *first = &function->instructions[function->blocks[block].first];
                if (!edge && first->op == XR_XIR_INVOKE_DISCARD) {
                    if (first->immediate != i || first->type != XR_XIR_UNIT) return XR_XIR_BAD_STRUCTURE;
                } else if (!edge && result == XR_XIR_UNIT) {
                    if (first->op == XR_XIR_INVOKE_RESULT || first->op == XR_XIR_INVOKE_ERROR)
                        return XR_XIR_BAD_STRUCTURE;
                } else if (first->op != (edge ? XR_XIR_INVOKE_ERROR : XR_XIR_INVOKE_RESULT) ||
                    first->immediate != i || first->type != (edge ? XR_XIR_ERROR : result))
                    return XR_XIR_BAD_STRUCTURE;
            }
        }
        if (op->op == XR_XIR_INVOKE_RESULT || op->op == XR_XIR_INVOKE_ERROR || op->op == XR_XIR_INVOKE_DISCARD) {
            if (op->immediate < 0 || (uint64_t) op->immediate >= function->instruction_count)
                return XR_XIR_BAD_STRUCTURE;
            uint32_t block = graph->owner[i];
            const XrXirInstruction *call = &function->instructions[op->immediate];
            if (!block || function->blocks[block].first != i ||
                (call->op != XR_XIR_INVOKE && call->op != XR_XIR_INVOKE_INDIRECT && call->op != XR_XIR_INVOKE_DEFAULT &&
                    call->op != XR_XIR_TASK_AWAIT) ||
                call->targets[op->op == XR_XIR_INVOKE_ERROR ? 1 : 0] != block)
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
