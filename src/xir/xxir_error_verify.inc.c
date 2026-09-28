/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_error_verify.inc.c - Exact nominal filters and dedicated successful casts
 */
static XrXirStatus error_filter_uses(const Graph *graph, const XrXirFunction *function,
    VerifyContext *context, uint32_t instruction) {
    const XrXirInstruction *op = &function->instructions[instruction];
    bool test = op->op == XR_XIR_ERROR_IS;
    if (test && (op->immediate < 0 || (uint64_t) op->immediate > UINT32_MAX)) return XR_XIR_BAD_TYPE;
    XrXirType target = test ? (XrXirType) op->immediate : op->type;
    if (!xr_xir_type_is_enum(context->module->types, target)) return XR_XIR_BAD_TYPE;
    XrXirStatus status = type_use_context(context, context->location.function, target);
    if (status == XR_XIR_OK) status = local_operand(context->module->types, function, op, op->args[0], 0);
    if (status == XR_XIR_OK) status = value_use(function, graph, instruction, op->args[0], XR_XIR_ERROR);
    if (status != XR_XIR_OK || test) return status;
    uint32_t block = graph->owner[instruction], incoming = graph->head[block];
    if (!block || function->blocks[block].first != instruction || incoming == UINT32_MAX ||
        graph->next[incoming] != UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    const XrXirBlock *from = &function->blocks[graph->predecessor[incoming]];
    const XrXirInstruction *branch = &function->instructions[from->first + from->count - 1];
    if (branch->op != XR_XIR_BRANCH || branch->targets[0] != block || branch->targets[1] == block ||
        branch->args[0] < function->parameter_count ||
        branch->args[0] - function->parameter_count >= function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *guard = &function->instructions[branch->args[0] - function->parameter_count];
    return guard->op == XR_XIR_ERROR_IS && guard->args[0] == op->args[0] &&
        guard->immediate == (int64_t) target ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
