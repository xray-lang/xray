/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_rebuild.c - Copy finished SSA without borrowing its backing storage
 *
 * KEY CONCEPT:
 *   Predeclare functions, blocks and parameters, then construct ordinary values
 *   in entry-rooted discovery order. Dominators precede uses even when their
 *   storage ordinals do not; successor ordinals keep parallel edges distinct.
 */
#include "xlir_rebuild.h"
#include "xlir_rebuild_internal.h"
#include "xlir_builder.h"
#include "xlir_verify.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"

typedef struct { XrLirBlock handle; bool emitted; } RebuildBlock;
typedef struct { uint32_t block, next; } RebuildFrame;
typedef struct {
    const XrLirModule *input;
    XrLirModule *output;
    XrLirFunction *functions;
    XrLirFunction source;
    XrLirFunctionView view;
    RebuildBlock *blocks;
    RebuildFrame *stack;
    XrLirValue *values, *arguments;
    XrLirType *types;
    const XrLirCopyPlan *plan;
} Rebuild;

static void *rebuild_storage(size_t count, size_t size) {
    return count && count <= SIZE_MAX / size ? xr_calloc(count, size) : NULL;
}

static XrLirStatus rebuild_registry(Rebuild *c) {
    uint32_t count = xr_lir_intrinsic_count(c->input);
    XrLirIntrinsic *entries = count ? rebuild_storage(count, sizeof(*entries)) : NULL;
    if (count && !entries) return XR_LIR_OUT_OF_MEMORY;
    XrLirStatus status = XR_LIR_OK;
    for (uint32_t i = 0; i < count; ++i) {
        status = xr_lir_intrinsic_at(c->input, i, &entries[i]);
        if (status != XR_LIR_OK) break;
    }
    if (status == XR_LIR_OK) {
        XrLirRegistry registry = {entries, count};
        status = xr_lir_module_create(&registry, &c->output);
    }
    // module_create copied parameter slices while their source was still live.
    xr_free(entries);
    return status;
}

static XrLirStatus rebuild_declarations(Rebuild *c, uint32_t count) {
    if (!count) return XR_LIR_OK;
    if ((uint64_t)count + 1 > SIZE_MAX / sizeof(*c->functions)) return XR_LIR_OUT_OF_MEMORY;
    c->functions = rebuild_storage((size_t)count + 1, sizeof(*c->functions));
    if (!c->functions) return XR_LIR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < count; ++i) {
        XrLirFunction function;
        XrLirFunctionView view;
        xr_lir_function_at(c->input, i, &function);
        xr_lir_function_view(c->input, function, &view);
        XrLirFunctionSpec spec = {.name = view.name, .name_length = view.name_length,
            .parameters = view.parameter_types, .parameter_count = view.parameter_count,
            .result = view.result, .kind = view.kind, .extern_kind = view.extern_kind};
        XrLirStatus status = xr_lir_declare_function(c->output, &spec, &c->functions[function.id]);
        if (status != XR_LIR_OK) return status;
    }
    return XR_LIR_OK;
}

static void rebuild_function_free(Rebuild *c) {
    xr_free(c->blocks); c->blocks = NULL;
    xr_free(c->stack); c->stack = NULL;
    xr_free(c->values); c->values = NULL;
    xr_free(c->arguments); c->arguments = NULL;
    xr_free(c->types); c->types = NULL;
}

static XrLirStatus rebuild_function_storage(Rebuild *c) {
    XR_DCHECK(c->view.kind == XR_LIR_FUNCTION_DEFINITION, "Only definitions require body storage");
    uint64_t blocks = (uint64_t)c->view.block_count + 1;
    uint64_t values = (uint64_t)c->view.value_count + 1;
    if (blocks > SIZE_MAX / sizeof(*c->blocks) || values > SIZE_MAX / sizeof(*c->values))
        return XR_LIR_OUT_OF_MEMORY;
    c->blocks = rebuild_storage((size_t)blocks, sizeof(*c->blocks));
    c->stack = rebuild_storage(c->view.block_count, sizeof(*c->stack));
    c->values = rebuild_storage((size_t)values, sizeof(*c->values));
    c->types = c->view.value_count ? rebuild_storage(c->view.value_count, sizeof(*c->types)) : NULL;
    if (!c->blocks || !c->stack || !c->values || (c->view.value_count && !c->types))
        return XR_LIR_OUT_OF_MEMORY;
    uint64_t maximum = 0;
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->input, c->source, b, &block);
        xr_lir_block_view(c->input, block, &view);
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView instruction;
            xr_lir_instruction_view(c->input, block, n, &instruction);
            uint64_t count = instruction.operand_count;
            for (uint32_t e = 0; e < instruction.edge_count; ++e)
                count += instruction.edges[e].argument_count;
            if (count > maximum) maximum = count;
        }
    }
    if (maximum > SIZE_MAX / sizeof(*c->arguments)) return XR_LIR_OUT_OF_MEMORY;
    c->arguments = maximum ? rebuild_storage((size_t)maximum, sizeof(*c->arguments)) : NULL;
    return maximum && !c->arguments ? XR_LIR_OUT_OF_MEMORY : XR_LIR_OK;
}

static XrLirStatus rebuild_parameters(Rebuild *c) {
    XrLirFunction function = c->functions[c->source.id];
    XrLirStatus status = xr_lir_define_function(c->output, function);
    if (status != XR_LIR_OK) return status;
    for (uint32_t i = 0; i < c->view.parameter_count; ++i) {
        status = xr_lir_function_parameter(c->output, function, i, &c->values[c->view.parameters[i]]);
        if (status != XR_LIR_OK) return status;
    }
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->input, c->source, b, &block);
        xr_lir_block_view(c->input, block, &view);
        if (c->plan && !c->plan->keep_block(c->plan->context, block.id)) continue;
        for (uint32_t p = 0; p < view.parameter_count; ++p)
            xr_lir_value_type(c->input, (XrLirValue){c->input, c->source.id, view.parameters[p]}, &c->types[p]);
        XrLirBlock *target = &c->blocks[block.id].handle;
        status = xr_lir_add_block(c->output, function, c->types, view.parameter_count, target);
        if (status != XR_LIR_OK) return status;
        for (uint32_t p = 0; p < view.parameter_count; ++p) {
            status = xr_lir_block_parameter(c->output, *target, p, &c->values[view.parameters[p]]);
            if (status != XR_LIR_OK) return status;
        }
    }
    if (!c->blocks[c->view.entry].handle.id) return XR_LIR_VERIFY_ERROR;
    return xr_lir_set_entry(c->output, function, c->blocks[c->view.entry].handle);
}

static XrLirStatus rebuild_arguments(Rebuild *c, const uint32_t *ids, uint32_t count, size_t offset) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!ids[i] || ids[i] > c->view.value_count) return XR_LIR_VERIFY_ERROR;
        XrLirValue mapped = c->values[ids[i]];
        if (!mapped.id) return XR_LIR_VERIFY_ERROR;
        c->arguments[offset + i] = mapped;
    }
    return XR_LIR_OK;
}

static XrLirStatus rebuild_instruction(Rebuild *c, XrLirBlock block,
    const XrLirInstructionView *source, bool terminal) {
    XR_DCHECK(source->edge_count <= 2, "Verified scalar control flow has at most two successors");
    XrLirInstruction instruction = {.op = source->op, .type = source->type,
        .predicate = source->predicate, .bits = source->bits, .intrinsic = source->intrinsic,
        .operands = source->operand_count ? c->arguments : NULL,
        .operand_count = source->operand_count, .edge_count = source->edge_count,
        .source = source->source};
    if (source->callee) instruction.callee = c->functions[source->callee];
    XrLirStatus status = rebuild_arguments(c, source->operands, source->operand_count, 0);
    if (status != XR_LIR_OK) return status;
    size_t offset = source->operand_count;
    for (uint32_t e = 0; e < source->edge_count; ++e) {
        XrLirEdgeView edge = source->edges[e];
        if (!edge.target || edge.target > c->view.block_count ||
            !c->blocks[edge.target].handle.id) return XR_LIR_VERIFY_ERROR;
        status = rebuild_arguments(c, edge.arguments, edge.argument_count, offset);
        if (status != XR_LIR_OK) return status;
        instruction.edges[e] = (XrLirEdge){c->blocks[edge.target].handle,
            edge.argument_count ? c->arguments + offset : NULL, edge.argument_count};
        offset += edge.argument_count;
    }
    if (terminal) return xr_lir_set_terminator(c->output, block, &instruction);
    if (source->result)
        return xr_lir_emit_value(c->output, block, &instruction, &c->values[source->result]);
    return xr_lir_emit_effect(c->output, block, &instruction);
}

static XrLirStatus rebuild_project(Rebuild *c, XrLirBlock block, uint32_t ordinal,
    XrLirInstructionView *view) {
    xr_lir_instruction_view(c->input, block, ordinal, view);
    uint64_t original = view->operand_count;
    for (uint32_t e = 0; e < view->edge_count; ++e) original += view->edges[e].argument_count;
    if (c->plan) c->plan->instruction(c->plan->context, block.id, ordinal, view);
    if (view->edge_count > 2) return XR_LIR_VERIFY_ERROR;
    uint64_t projected = view->operand_count;
    for (uint32_t e = 0; e < view->edge_count; ++e) projected += view->edges[e].argument_count;
    return projected <= original ? XR_LIR_OK : XR_LIR_VERIFY_ERROR;
}

static XrLirStatus rebuild_body(Rebuild *c) {
    size_t depth = 1;
    c->stack[0] = (RebuildFrame){c->view.entry, 0};
    while (depth) {
        RebuildFrame *frame = &c->stack[depth - 1];
        XrLirBlock block = {c->input, c->source.id, frame->block};
        XrLirBlockView view;
        xr_lir_block_view(c->input, block, &view);
        XrLirInstructionView instruction;
        if (!c->blocks[block.id].emitted) {
            c->blocks[block.id].emitted = true;
            for (uint32_t n = 0; n < view.instruction_count; ++n) {
                XrLirStatus status = rebuild_project(c, block, n, &instruction);
                if (status != XR_LIR_OK) return status;
                status = rebuild_instruction(c, c->blocks[block.id].handle,
                    &instruction, n + 1 == view.instruction_count);
                if (status != XR_LIR_OK) return status;
            }
        }
        XrLirStatus status = rebuild_project(c, block, view.instruction_count - 1, &instruction);
        if (status != XR_LIR_OK) return status;
        if (frame->next < instruction.edge_count) {
            uint32_t target = instruction.edges[frame->next++].target;
            if (!c->blocks[target].emitted) {
                XR_DCHECK(depth < c->view.block_count, "DFS contains no repeated active block");
                c->stack[depth++] = (RebuildFrame){target, 0};
            }
        } else --depth;
    }
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_internal_copy(const XrLirModule *input, XrLirModule **output,
    const XrLirCopyPlan *plan) {
    if (!output || *output || !xr_lir_module_is_readable(input)) return XR_LIR_INVALID_ARGUMENT;
    XrLirStatus status = xr_lir_verify(input, NULL);
    if (status != XR_LIR_OK) return status;
    Rebuild c = {.input = input, .plan = plan};
    status = rebuild_registry(&c);
    uint32_t count = xr_lir_function_count(input);
    if (status == XR_LIR_OK) status = rebuild_declarations(&c, count);
    for (uint32_t f = 0; f < count && status == XR_LIR_OK; ++f) {
        xr_lir_function_at(input, f, &c.source);
        xr_lir_function_view(input, c.source, &c.view);
        if (c.view.kind == XR_LIR_FUNCTION_EXTERNAL) continue;
        if (plan) status = plan->begin(plan->context, input, c.source);
        if (status == XR_LIR_OK) status = rebuild_function_storage(&c);
        if (status == XR_LIR_OK) status = rebuild_parameters(&c);
        if (status == XR_LIR_OK) status = rebuild_body(&c);
        rebuild_function_free(&c);
        if (plan) plan->end(plan->context);
    }
    if (status == XR_LIR_OK) status = xr_lir_module_finish(c.output, NULL);
    xr_free(c.functions);
    if (status == XR_LIR_OK) *output = c.output;
    else xr_lir_module_destroy(c.output);
    return status;
}

XR_FUNC XrLirStatus xr_lir_rebuild(const XrLirModule *input, XrLirModule **output) {
    return xr_lir_internal_copy(input, output, NULL);
}
