/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cleanup_frontier.inc.c - Registration dominance and lexical exit edges
 *
 * KEY CONCEPT:
 *   An active cleanup is proved by its registration edge, never by a body name.
 */
static XrXirStatus cleanup_ancestor(const Graph *graph, const XrXirFunction *function,
    uint32_t from, uint32_t to, XrXirCompileContext *remaining) {
    while (from != to && from) {
        if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
        if (from > function->instruction_count ||
            function->instructions[from - 1].op != XR_XIR_CLEANUP_REGISTER) return XR_XIR_BAD_STRUCTURE;
        uint32_t parent = function->blocks[graph->owner[from - 1]].frontier;
        if (parent >= from) return XR_XIR_BAD_STRUCTURE;
        from = parent;
    }
    return from == to ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus cleanup_frontiers(const Graph *graph, const XrXirFunction *function,
    VerifyContext *context) {
    if (function->blocks[0].frontier) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t b = 0; b < function->block_count; ++b) {
        context->location.block = b;
        const XrXirBlock *block = &function->blocks[b];
        uint32_t frontier = block->frontier, last = block->first + block->count - 1;
        context->location.instruction = last;
        if (!xir_compile_work(&context->remaining, 1)) return XR_XIR_BUDGET;
        if (frontier) {
            if (frontier > function->instruction_count) return XR_XIR_BAD_STRUCTURE;
            const XrXirInstruction *registration = &function->instructions[frontier - 1];
            if (registration->op != XR_XIR_CLEANUP_REGISTER ||
                function->blocks[graph->owner[frontier - 1]].frontier >= frontier) return XR_XIR_BAD_STRUCTURE;
            uint32_t entry = registration->targets[0];
            uint64_t bits = graph->dominators[(size_t)b * graph->words + entry / 64];
            if (!(bits & (UINT64_C(1) << (entry % 64)))) return XR_XIR_BAD_DOMINANCE;
        }
        const XrXirInstruction *end = &function->instructions[last];
        if (end->op == XR_XIR_CLEANUP_REGISTER) {
            if (frontier >= last + 1 || function->blocks[end->targets[0]].frontier != last + 1)
                return XR_XIR_BAD_STRUCTURE;
        } else if (end->op == XR_XIR_CLEANUP_LEAVE || end->op == XR_XIR_CLEANUP_ERROR) {
            uint32_t target = (uint32_t)end->immediate;
            if (function->blocks[end->targets[0]].frontier != target) return XR_XIR_BAD_STRUCTURE;
            XrXirStatus status = cleanup_ancestor(graph, function, frontier, target, &context->remaining);
            if (status != XR_XIR_OK) return status;
        } else for (uint32_t e = 0; e < op_rules[end->op].edges; ++e) {
            if (function->blocks[end->targets[e]].frontier != frontier) return XR_XIR_BAD_STRUCTURE;
        }
        if (block->panic) {
            XrXirStatus status = cleanup_ancestor(graph, function, frontier,
                function->blocks[block->panic].frontier, &context->remaining);
            if (status != XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
