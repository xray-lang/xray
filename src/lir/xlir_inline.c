/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_inline.c - Direct call expansion with stable caller entries
 *
 * KEY CONCEPT:
 *   Plan original call sites before construction. Each instance owns separate
 *   maps; a local continuation cursor never replaces a caller's entry mapping.
 */
#include "xlir_inline.h"
#include "xlir_builder.h"
#include "xlir_verify.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"

typedef struct {
    XrLirFunctionView view;
    size_t first, count;
    uint64_t instructions, scratch;
    uint32_t index, low, component, component_size;
    bool active, self, returns, eligible, cell_storage, string_owners;
} InlineFunction;
typedef struct { uint32_t function; size_t next; } InlineGraphFrame;
typedef struct {
    const XrLirModule *input;
    XrLirModule *output;
    InlineFunction *info;
    XrLirFunction *functions;
    uint32_t count, *edges, *active;
    InlineGraphFrame *graph_stack;
    size_t edge_count;
} Inliner;
typedef struct { XrLirBlock entry; size_t first; bool visited; } InlineBlock;
typedef struct {
    uint32_t block, instruction, next;
    XrLirBlock cursor;
} InlineFrame;
typedef struct {
    Inliner *owner;
    uint32_t source;
    XrLirFunction destination;
    XrLirFunctionView view;
    InlineBlock *blocks;
    InlineFrame *stack;
    XrLirValue *values, *arguments;
    XrLirType *types;
    bool *sites;
    size_t depth;
    uint64_t scratch;
} InlineBody;

static void *inline_storage(uint64_t count, size_t size) {
    return count && count <= SIZE_MAX / size ? xr_calloc((size_t)count, size) : NULL;
}
static XrLirBlock inline_source_block(const Inliner *c, uint32_t function, uint32_t block) {
    return (XrLirBlock){c->input, function, block};
}
static XrLirBlockView inline_block_view(const Inliner *c, uint32_t function, uint32_t block) {
    XrLirBlockView view;
    xr_lir_block_view(c->input, inline_source_block(c, function, block), &view);
    return view;
}
static XrLirInstructionView inline_view(const Inliner *c, uint32_t function,
    uint32_t block, uint32_t ordinal) {
    XrLirInstructionView view;
    xr_lir_instruction_view(c->input, inline_source_block(c, function, block), ordinal, &view);
    return view;
}
static uint64_t inline_scratch(const XrLirInstructionView *v) {
    uint64_t count = v->operand_count;
    for (uint32_t e = 0; e < v->edge_count; ++e) count += v->edges[e].argument_count;
    return count;
}

static XrLirStatus inline_graph_storage(Inliner *c) {
    uint64_t count = (uint64_t)c->count + 1;
    c->info = inline_storage(count, sizeof(*c->info));
    c->functions = inline_storage(count, sizeof(*c->functions));
    c->active = inline_storage(count, sizeof(*c->active));
    c->graph_stack = inline_storage(count, sizeof(*c->graph_stack));
    if (!c->info || !c->functions || !c->active || !c->graph_stack) return XR_LIR_OUT_OF_MEMORY;
    for (uint32_t f = 0; f < c->count; ++f) {
        InlineFunction *info = &c->info[f + 1];
        xr_lir_function_view(c->input, (XrLirFunction){c->input, f + 1}, &info->view);
        for (uint32_t v = 1; v <= info->view.value_count; ++v) {
            XrLirType type = XR_LIR_TYPE_INVALID;
            xr_lir_value_type(c->input, (XrLirValue){c->input, f + 1, v}, &type);
            info->string_owners |= type == XR_LIR_STRING || type == XR_LIR_STRING_CELL;
        }
        info->first = c->edge_count;
        for (uint32_t b = 0; b < info->view.block_count; ++b) {
            XrLirBlockView block = inline_block_view(c, f + 1, b + 1);
            info->instructions += block.instruction_count;
            for (uint32_t n = 0; n < block.instruction_count; ++n) {
                XrLirInstructionView v = inline_view(c, f + 1, b + 1, n);
                uint64_t scratch = inline_scratch(&v);
                if (scratch > info->scratch) info->scratch = scratch;
                info->returns |= v.op == XR_LIR_RET;
                info->cell_storage |= v.op == XR_LIR_CELL_NEW;
                if (v.op != XR_LIR_CALL) continue;
                if (c->edge_count == SIZE_MAX) return XR_LIR_OUT_OF_MEMORY;
                ++c->edge_count;
                ++info->count;
                info->self |= v.callee == f + 1;
            }
        }
    }
    c->edges = inline_storage(c->edge_count, sizeof(*c->edges));
    return c->edge_count && !c->edges ? XR_LIR_OUT_OF_MEMORY : XR_LIR_OK;
}
static void inline_graph_edges(Inliner *c) {
    size_t offset = 0;
    for (uint32_t f = 0; f < c->count; ++f)
        for (uint32_t b = 0; b < c->info[f + 1].view.block_count; ++b) {
            XrLirBlockView block = inline_block_view(c, f + 1, b + 1);
            for (uint32_t n = 0; n < block.instruction_count; ++n) {
                XrLirInstructionView v = inline_view(c, f + 1, b + 1, n);
                if (v.op == XR_LIR_CALL) c->edges[offset++] = v.callee;
            }
        }
    XR_DCHECK(offset == c->edge_count, "Immutable direct calls match graph storage");
}
static void inline_graph_visit(Inliner *c, uint32_t f, uint32_t *sequence, size_t *top) {
    c->info[f].index = c->info[f].low = ++*sequence;
    c->info[f].active = true;
    c->active[(*top)++] = f;
}
static void inline_components(Inliner *c) {
    uint32_t sequence = 0;
    size_t top = 0;
    for (uint32_t root = 0; root < c->count; ++root) {
        if (c->info[root + 1].index) continue;
        size_t depth = 1;
        inline_graph_visit(c, root + 1, &sequence, &top);
        c->graph_stack[0] = (InlineGraphFrame){root + 1, c->info[root + 1].first};
        while (depth) {
            InlineGraphFrame *frame = &c->graph_stack[depth - 1];
            InlineFunction *info = &c->info[frame->function];
            if (frame->next < info->first + info->count) {
                uint32_t target = c->edges[frame->next++];
                if (!c->info[target].index) {
                    XR_DCHECK(depth < c->count, "Each graph frame discovers a new function");
                    inline_graph_visit(c, target, &sequence, &top);
                    c->graph_stack[depth++] = (InlineGraphFrame){target, c->info[target].first};
                } else if (c->info[target].active && c->info[target].index < info->low)
                    info->low = c->info[target].index;
                continue;
            }
            if (info->low == info->index) {
                uint32_t member, size = 0;
                do {
                    member = c->active[--top];
                    c->info[member].active = false;
                    c->info[member].component = frame->function;
                    ++size;
                } while (member != frame->function);
                info->component_size = size;
            }
            --depth;
            if (depth) {
                InlineFunction *parent = &c->info[c->graph_stack[depth - 1].function];
                if (info->low < parent->low) parent->low = info->low;
            }
        }
    }
    // These caps control duplication, not the admission of otherwise valid programs.
    for (uint32_t f = 0; f < c->count; ++f) {
        InlineFunction *info = &c->info[f + 1];
        info->eligible = info->view.kind == XR_LIR_FUNCTION_DEFINITION && info->returns &&
            !info->self && !info->cell_storage && !info->string_owners &&
            c->info[info->component].component_size == 1 &&
            info->view.block_count <= 16 && info->view.value_count <= 128 &&
            info->instructions <= 256 && info->scratch <= 256;
    }
}
static XrLirStatus inline_registry(Inliner *c) {
    uint32_t count = xr_lir_intrinsic_count(c->input);
    XrLirIntrinsic *entries = inline_storage(count, sizeof(*entries));
    if (count && !entries) return XR_LIR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < count; ++i) xr_lir_intrinsic_at(c->input, i, &entries[i]);
    XrLirRegistry registry = {entries, count};
    XrLirStatus status = xr_lir_module_create(&registry, &c->output);
    xr_free(entries);
    for (uint32_t i = 0; status == XR_LIR_OK && i < xr_lir_literal_count(c->input); ++i) {
        XrLirLiteral old = {0}, owned = {0};const uint8_t *bytes = NULL;size_t length = 0;
        status = xr_lir_literal_at(c->input, i, &old);
        if (status == XR_LIR_OK) status = xr_lir_literal_view(c->input, old, &bytes, &length);
        if (status == XR_LIR_OK) status = xr_lir_add_literal(c->output, bytes, length, &owned);
    }
    return status;
}
static XrLirStatus inline_declarations(Inliner *c) {
    for (uint32_t f = 0; f < c->count; ++f) {
        const XrLirFunctionView *v = &c->info[f + 1].view;
        XrLirFunctionSpec spec = {.name = v->name, .name_length = v->name_length,
            .parameters = v->parameter_types, .parameter_count = v->parameter_count,
            .result = v->result, .kind = v->kind, .extern_kind = v->extern_kind};
        XrLirStatus status = xr_lir_declare_function(c->output, &spec, &c->functions[f + 1]);
        if (status != XR_LIR_OK) return status;
    }
    return XR_LIR_OK;
}

static void inline_body_free(InlineBody *b) {
    xr_free(b->blocks); xr_free(b->stack); xr_free(b->values);
    xr_free(b->arguments); xr_free(b->types); xr_free(b->sites);
}
static XrLirStatus inline_plan(InlineBody *b) {
    uint64_t added_blocks = 0, added_values = 0, added_instructions = 0;
    size_t offset = 0;
    for (uint32_t block = 0; block < b->view.block_count; ++block) {
        b->blocks[block + 1].first = offset;
        XrLirBlockView view = inline_block_view(b->owner, b->source, block + 1);
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView v = inline_view(b->owner, b->source, block + 1, n);
            if (v.op != XR_LIR_CALL) continue;
            const InlineFunction *callee = &b->owner->info[v.callee];
            // Continuation blocks cannot introduce an unrepresented String transfer.
            if (!callee->eligible || b->owner->info[b->source].string_owners) continue;
            if (callee->view.value_count < callee->view.parameter_count) return XR_LIR_VERIFY_ERROR;
            uint64_t blocks = (uint64_t)callee->view.block_count + 1;
            uint64_t values = callee->view.value_count - callee->view.parameter_count;
            if (blocks > 64 - added_blocks || values > 512 - added_values ||
                callee->instructions > 1024 - added_instructions ||
                (uint64_t)b->view.block_count + added_blocks + blocks > UINT32_MAX ||
                (uint64_t)b->view.value_count + added_values + values > UINT32_MAX) continue;
            b->sites[offset + n] = true;
            added_blocks += blocks; added_values += values; added_instructions += callee->instructions;
            if (callee->scratch > b->scratch) b->scratch = callee->scratch;
        }
        offset += view.instruction_count;
    }
    return XR_LIR_OK;
}
static XrLirStatus inline_body_storage(InlineBody *b, bool plan) {
    uint64_t blocks = (uint64_t)b->view.block_count + 1;
    uint64_t values = (uint64_t)b->view.value_count + 1;
    b->blocks = inline_storage(blocks, sizeof(*b->blocks));
    b->stack = inline_storage(b->view.block_count, sizeof(*b->stack));
    b->values = inline_storage(values, sizeof(*b->values));
    b->types = inline_storage(b->view.value_count, sizeof(*b->types));
    if (!b->blocks || !b->stack || !b->values || (b->view.value_count && !b->types))
        return XR_LIR_OUT_OF_MEMORY;
    if (plan) {
        b->sites = inline_storage(b->owner->info[b->source].instructions, sizeof(*b->sites));
        if (!b->sites) return XR_LIR_OUT_OF_MEMORY;
        XrLirStatus status = inline_plan(b);
        if (status != XR_LIR_OK) return status;
    }
    b->arguments = inline_storage(b->scratch, sizeof(*b->arguments));
    return b->scratch && !b->arguments ? XR_LIR_OUT_OF_MEMORY : XR_LIR_OK;
}
static XrLirStatus inline_blocks(InlineBody *b) {
    for (uint32_t block = 0; block < b->view.block_count; ++block) {
        XrLirBlockView view = inline_block_view(b->owner, b->source, block + 1);
        for (uint32_t p = 0; p < view.parameter_count; ++p)
            xr_lir_value_type(b->owner->input,
                (XrLirValue){b->owner->input, b->source, view.parameters[p]}, &b->types[p]);
        XrLirStatus status = xr_lir_add_block(b->owner->output, b->destination,
            b->types, view.parameter_count, &b->blocks[block + 1].entry);
        if (status != XR_LIR_OK) return status;
        for (uint32_t p = 0; p < view.parameter_count; ++p) {
            status = xr_lir_block_parameter(b->owner->output, b->blocks[block + 1].entry,
                p, &b->values[view.parameters[p]]);
            if (status != XR_LIR_OK) return status;
        }
    }
    return XR_LIR_OK;
}
static void inline_walk_start(InlineBody *b) {
    b->depth = 1;
    b->blocks[b->view.entry].visited = true;
    b->stack[0] = (InlineFrame){b->view.entry, 0, 0, b->blocks[b->view.entry].entry};
}
static bool inline_next(InlineBody *b, InlineFrame **location, XrLirInstructionView *instruction) {
    while (b->depth) {
        InlineFrame *frame = &b->stack[b->depth - 1];
        XrLirBlockView view = inline_block_view(b->owner, b->source, frame->block);
        if (frame->instruction < view.instruction_count) {
            *location = frame;
            *instruction = inline_view(b->owner, b->source, frame->block, frame->instruction++);
            return true;
        }
        XrLirInstructionView term = inline_view(b->owner, b->source, frame->block, view.instruction_count - 1);
        if (frame->next < term.edge_count) {
            uint32_t target = term.edges[frame->next++].target;
            if (!b->blocks[target].visited) {
                XR_DCHECK(b->depth < b->view.block_count, "Entry DFS discovers each source block once");
                b->blocks[target].visited = true;
                b->stack[b->depth++] = (InlineFrame){target, 0, 0, b->blocks[target].entry};
            }
        } else --b->depth;
    }
    return false;
}
static XrLirStatus inline_arguments(InlineBody *b, const uint32_t *ids, uint32_t count, size_t offset) {
    if ((uint64_t)count + offset > b->scratch) return XR_LIR_VERIFY_ERROR;
    for (uint32_t i = 0; i < count; ++i) {
        if (!ids[i] || ids[i] > b->view.value_count || !b->values[ids[i]].id)
            return XR_LIR_VERIFY_ERROR;
        b->arguments[offset + i] = b->values[ids[i]];
    }
    return XR_LIR_OK;
}
static XrLirStatus inline_branch(Inliner *c, XrLirBlock from, XrLirBlock to,
    XrLirValue value, XrLirSource source) {
    XrLirInstruction branch = {.op = XR_LIR_BR, .type = XR_LIR_UNIT,
        .edges = {{to, value.id ? &value : NULL, value.id ? 1u : 0u}}, .edge_count = 1, .source = source};
    return xr_lir_set_terminator(c->output, from, &branch);
}
static XrLirStatus inline_copy(InlineBody *b, XrLirBlock cursor,
    const XrLirInstructionView *v, XrLirBlock continuation) {
    if (continuation.id && v->op == XR_LIR_RET) {
        XrLirValue value = v->operand_count ? b->values[v->operands[0]] : (XrLirValue){0};
        if (v->operand_count && !value.id) return XR_LIR_VERIFY_ERROR;
        return inline_branch(b->owner, cursor, continuation, value, v->source);
    }
    XrLirInstruction copy = {.op = v->op, .type = v->type, .predicate = v->predicate,
        .operands = v->operand_count ? b->arguments : NULL, .operand_count = v->operand_count,
        .bits = v->bits, .intrinsic = v->intrinsic, .edge_count = v->edge_count, .source = v->source};
    if (v->callee) copy.callee = b->owner->functions[v->callee];
    XrLirStatus status = inline_arguments(b, v->operands, v->operand_count, 0);
    if (status != XR_LIR_OK) return status;
    size_t offset = v->operand_count;
    for (uint32_t e = 0; e < v->edge_count; ++e) {
        XrLirEdgeView edge = v->edges[e];
        if (!edge.target || edge.target > b->view.block_count || !b->blocks[edge.target].entry.id)
            return XR_LIR_VERIFY_ERROR;
        status = inline_arguments(b, edge.arguments, edge.argument_count, offset);
        if (status != XR_LIR_OK) return status;
        copy.edges[e] = (XrLirEdge){b->blocks[edge.target].entry,
            edge.argument_count ? b->arguments + offset : NULL, edge.argument_count};
        offset += edge.argument_count;
    }
    if (v->op == XR_LIR_BR || v->op == XR_LIR_CONDBR || v->op == XR_LIR_RET)
        return xr_lir_set_terminator(b->owner->output, cursor, &copy);
    if (v->result) return xr_lir_emit_value(b->owner->output, cursor, &copy, &b->values[v->result]);
    return xr_lir_emit_effect(b->owner->output, cursor, &copy);
}
static XrLirStatus inline_site(InlineBody *caller, XrLirBlock *cursor,
    const XrLirInstructionView *call) {
    Inliner *c = caller->owner;
    InlineBody clone = {.owner = c, .source = call->callee, .destination = caller->destination,
        .view = c->info[call->callee].view, .scratch = c->info[call->callee].scratch};
    XrLirStatus status = inline_body_storage(&clone, false);
    XrLirBlock continuation = {0};
    for (uint32_t p = 0; p < call->operand_count && status == XR_LIR_OK; ++p) {
        uint32_t id = call->operands[p];
        if (!id || id > caller->view.value_count || !caller->values[id].id) status = XR_LIR_VERIFY_ERROR;
        else clone.values[clone.view.parameters[p]] = caller->values[id];
    }
    if (status == XR_LIR_OK)
        status = xr_lir_add_block(c->output, caller->destination,
            call->result ? &call->type : NULL, call->result ? 1u : 0u, &continuation);
    if (status == XR_LIR_OK && call->result)
        status = xr_lir_block_parameter(c->output, continuation, 0, &caller->values[call->result]);
    if (status == XR_LIR_OK) status = inline_blocks(&clone);
    if (status == XR_LIR_OK)
        status = inline_branch(c, *cursor, clone.blocks[clone.view.entry].entry, (XrLirValue){0}, call->source);
    if (status == XR_LIR_OK) {
        inline_walk_start(&clone);
        InlineFrame *frame; XrLirInstructionView v;
        while (status == XR_LIR_OK && inline_next(&clone, &frame, &v))
            status = inline_copy(&clone, frame->cursor, &v, continuation);
    }
    // Original incoming edges still target the prefix; only emission advances.
    if (status == XR_LIR_OK) *cursor = continuation;
    inline_body_free(&clone);
    return status;
}
static XrLirStatus inline_definition(Inliner *c, uint32_t id) {
    InlineBody body = {.owner = c, .source = id, .destination = c->functions[id],
        .view = c->info[id].view, .scratch = c->info[id].scratch};
    XrLirStatus status = inline_body_storage(&body, true);
    if (status == XR_LIR_OK) status = xr_lir_define_function(c->output, body.destination);
    for (uint32_t p = 0; p < body.view.parameter_count && status == XR_LIR_OK; ++p)
        status = xr_lir_function_parameter(c->output, body.destination, p, &body.values[body.view.parameters[p]]);
    if (status == XR_LIR_OK) status = inline_blocks(&body);
    if (status == XR_LIR_OK)
        status = xr_lir_set_entry(c->output, body.destination, body.blocks[body.view.entry].entry);
    if (status == XR_LIR_OK) {
        inline_walk_start(&body);
        InlineFrame *frame; XrLirInstructionView v;
        while (status == XR_LIR_OK && inline_next(&body, &frame, &v)) {
            size_t site = body.blocks[frame->block].first + frame->instruction - 1;
            if (body.sites[site]) status = inline_site(&body, &frame->cursor, &v);
            else status = inline_copy(&body, frame->cursor, &v, (XrLirBlock){0});
        }
    }
    inline_body_free(&body);
    return status;
}

XR_FUNC XrLirStatus xr_lir_inline(const XrLirModule *input, XrLirModule **output) {
    if (!output || *output || !xr_lir_module_is_readable(input)) return XR_LIR_INVALID_ARGUMENT;
    XrLirStatus status = xr_lir_verify(input, NULL);
    if (status != XR_LIR_OK) return status;
    Inliner c = {.input = input, .count = xr_lir_function_count(input)};
    status = inline_graph_storage(&c);
    if (status == XR_LIR_OK) { inline_graph_edges(&c); inline_components(&c); }
    if (status == XR_LIR_OK) status = inline_registry(&c);
    if (status == XR_LIR_OK) status = inline_declarations(&c);
    for (uint32_t f = 0; f < c.count && status == XR_LIR_OK; ++f)
        if (c.info[f + 1].view.kind == XR_LIR_FUNCTION_DEFINITION) status = inline_definition(&c, f + 1);
    if (status == XR_LIR_OK) status = xr_lir_module_finish(c.output, NULL);
    xr_free(c.info); xr_free(c.functions); xr_free(c.edges); xr_free(c.active); xr_free(c.graph_stack);
    if (status == XR_LIR_OK) *output = c.output;
    else xr_lir_module_destroy(c.output);
    return status;
}
