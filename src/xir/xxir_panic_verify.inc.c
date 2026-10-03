/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_panic_verify.inc.c - Dedicated panic handler blocks
 *
 * KEY CONCEPT:
 *   A handler is entered only through panic edges, and its selector is the
 *   first instruction there; ordinary control flow can never reach it.
 */
static XrXirStatus panic_edges(const Graph *graph, const XrXirFunction *function,
    VerifyContext *context) {
    for (uint32_t b = 0; b < function->block_count; ++b) {
        context->location.block = b;
        if (!xir_compile_work(&context->remaining, 1)) return XR_XIR_BUDGET;
        bool handler = graph->fault_head[b] != UINT32_MAX;
        bool selector = function->instructions[function->blocks[b].first].op == XR_XIR_PANIC_CATCH;
        if (handler != selector || (handler && graph->head[b] != UINT32_MAX)) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        context->location.instruction = i;
        if (!xir_compile_work(&context->remaining, 1)) return XR_XIR_BUDGET;
        if (function->instructions[i].op == XR_XIR_PANIC_CATCH &&
            function->blocks[graph->owner[i]].first != i) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}
