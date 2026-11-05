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

        const XrXirInstruction *op = &function->instructions[i];
        if (op->op==XR_XIR_TASK_AWAIT_RESULT) {
            uint32_t block=op->targets[0],incoming=graph->head[block];
            if (incoming==UINT32_MAX || graph->next[incoming]!=UINT32_MAX ||
                graph->predecessor[incoming]!=graph->owner[i]) return XR_XIR_BAD_STRUCTURE;
            const XrXirInstruction *first=&function->instructions[function->blocks[block].first];
            if (first->op!=XR_XIR_INVOKE_RESULT || first->immediate!=i ||
                !xr_xir_task_result_matches_task(context->module->types,first->type,
                    xr_xir_operand_type(function,op->args[0]))) return XR_XIR_BAD_TYPE;
        }
        if (op->op == XR_XIR_INVOKE || op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_INVOKE_DEFAULT ||
            (op->op == XR_XIR_TASK_AWAIT || op->op == XR_XIR_ITERATOR_HAS_NEXT || op->op == XR_XIR_ITERATOR_NEXT || op->op == XR_XIR_ITERATOR_NTH || op->op == XR_XIR_ITERATOR_CLOSE)) {
            XrXirType result = (op->op == XR_XIR_TASK_AWAIT || op->op == XR_XIR_ITERATOR_HAS_NEXT || op->op == XR_XIR_ITERATOR_NEXT || op->op == XR_XIR_ITERATOR_NTH || op->op == XR_XIR_ITERATOR_CLOSE) ?
                task_await_result(context->module, function, op) : op->type;
            if ((op->op == XR_XIR_TASK_AWAIT || op->op == XR_XIR_ITERATOR_HAS_NEXT || op->op == XR_XIR_ITERATOR_NEXT || op->op == XR_XIR_ITERATOR_NTH || op->op == XR_XIR_ITERATOR_CLOSE)) {
                const XrXirTypeNode *task = xr_xir_type_node(context->module->types,
                    xr_xir_operand_type(function, op->args[0]));
                if (!task || task->kind != (uint32_t)(op->op==XR_XIR_TASK_AWAIT ? XR_XIR_TYPE_TASK : XR_XIR_TYPE_ITERATOR))
                    return XR_XIR_BAD_TYPE;
            }
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
                    call->op != XR_XIR_TASK_AWAIT && call->op!=XR_XIR_TASK_AWAIT_RESULT && call->op != XR_XIR_ITERATOR_HAS_NEXT && call->op != XR_XIR_ITERATOR_NEXT && call->op != XR_XIR_ITERATOR_NTH && call->op != XR_XIR_ITERATOR_CLOSE) ||
                call->targets[op->op == XR_XIR_INVOKE_ERROR ? 1 : 0] != block)
                return XR_XIR_BAD_STRUCTURE;
            if (call->op==XR_XIR_TASK_AWAIT_RESULT && op->op!=XR_XIR_INVOKE_RESULT)
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
