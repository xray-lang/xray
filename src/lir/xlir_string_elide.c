/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_string_elide.c - One reference for a copy and the owner it outlives
 *
 * KEY CONCEPT:
 *   Verified LIR retires every owned String exactly once inside its defining
 *   block, through a drop, an owned append or the terminator. An incoming
 *   parameter stays valid until its call returns. A retain and its source each
 *   have one known end. When the earlier end is a plain drop, the later owner can
 *   carry the source's reference instead of a fresh one. Every operand other
 *   than a drop, an owned append receiver, a String result or an edge argument
 *   borrows, so substituting the source for the copy only lengthens borrows the
 *   source already covers.
 */
#include "xlir_string_elide.h"
#include "xlir_rebuild_internal.h"
#include "xlir_string.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"

typedef struct {
    const XrLirModule *input;
    XrLirFunction function;
    XrLirFunctionView view;
    size_t *first;     // flat offset of each one-based block's instructions
    uint8_t *omit;     // flat instructions the projection removes
    uint8_t *borrowed; // incoming parameters, valid until the call returns
    uint32_t *root;    // the value whose reference an elided copy shares
    uint32_t *end;     // one-based ordinal of an owner's consuming instruction
} Elide;

static void *elide_storage(uint64_t count, size_t size) {
    return count && count <= SIZE_MAX / size ? xr_calloc((size_t)count, size) : NULL;
}
static void elide_end(void *context) {
    Elide *c = context;
    xr_free(c->first); xr_free(c->omit); xr_free(c->borrowed);
    xr_free(c->root); xr_free(c->end);
    *c = (Elide){0};
}
static XrLirInstructionView elide_view(const Elide *c, XrLirBlock block, uint32_t ordinal) {
    XrLirInstructionView view;
    xr_lir_instruction_view(c->input, block, ordinal, &view);
    return view;
}

static XrLirStatus elide_allocate(Elide *c) {
    uint64_t values = (uint64_t)c->view.value_count + 1;
    c->first = elide_storage((uint64_t)c->view.block_count + 1, sizeof(*c->first));
    c->borrowed = elide_storage(values, sizeof(*c->borrowed));
    c->root = elide_storage(values, sizeof(*c->root));
    c->end = elide_storage(values, sizeof(*c->end));
    if (!c->first || !c->borrowed || !c->root || !c->end) return XR_LIR_OUT_OF_MEMORY;
    uint64_t total = 0;
    for (uint32_t b = 1; b <= c->view.block_count; ++b) {
        XrLirBlockView view;
        xr_lir_block_view(c->input, (XrLirBlock){c->input, c->function.id, b}, &view);
        // A truncated offset is never read: the storage below fails first.
        c->first[b] = (size_t)total;
        total += view.instruction_count;
    }
    XR_DCHECK(total >= c->view.block_count, "every verified block ends in a terminator");
    c->omit = elide_storage(total, sizeof(*c->omit));
    if (!c->omit) return XR_LIR_OUT_OF_MEMORY;
    for (uint32_t v = 1; v <= c->view.value_count; ++v) c->root[v] = v;
    for (uint32_t p = 0; p < c->view.parameter_count; ++p) c->borrowed[c->view.parameters[p]] = 1;
    return XR_LIR_OK;
}

/* Values never outlive their block as owners, so ends recorded here are only
 * read for this block's retains; scalar entries are recorded but never read.
 * Retains are visited in order, so each operand's root is already final. */
static void elide_block(Elide *c, uint32_t id) {
    XrLirBlock block = {c->input, c->function.id, id};
    XrLirBlockView view;
    xr_lir_block_view(c->input, block, &view);
    uint32_t last = view.instruction_count;
    XR_DCHECK(last > 0, "verified blocks end in a terminator");
    for (uint32_t n = 0; n < last; ++n) {
        XrLirInstructionView v = elide_view(c, block, n);
        if (v.op == XR_LIR_STRING_DROP ||
            xr_lir_string_role(c->input, &v) == XR_LIR_STRING_OP_APPEND_OWNED ||
            (v.op == XR_LIR_RET && v.operand_count))
            c->end[v.operands[0]] = n + 1;
        for (uint32_t e = 0; e < v.edge_count; ++e)
            for (uint32_t a = 0; a < v.edges[e].argument_count; ++a)
                c->end[v.edges[e].arguments[a]] = n + 1;
    }
    uint8_t *omit = c->omit + c->first[id];
    for (uint32_t n = 0; n + 1 < last; ++n) {
        XrLirInstructionView v = elide_view(c, block, n);
        if (v.op != XR_LIR_STRING_RETAIN) continue;
        uint32_t copy = v.result, source = c->root[v.operands[0]];
        uint32_t copy_end = c->end[copy];
        uint32_t source_end = c->borrowed[source] ? UINT32_MAX : c->end[source];
        XR_DCHECK(copy_end > n + 1 && copy_end <= last && source_end > n + 1,
            "verified owners retire after their copy inside the block");
        uint32_t earlier = copy_end < source_end ? copy_end : source_end;
        // An owner leaving through the terminator while the other is still
        // live needs its own reference; both leaving there need two.
        if (earlier >= last || elide_view(c, block, earlier - 1).op != XR_LIR_STRING_DROP)
            continue;
        omit[n] = omit[earlier - 1] = 1;
        c->root[copy] = source;
        if (earlier == source_end) c->end[source] = copy_end;
    }
}

static XrLirStatus elide_begin(void *context, const XrLirModule *input, XrLirFunction function) {
    Elide *c = context;
    XR_DCHECK(!c->first && !c->omit, "the preceding function released its scratch");
    c->input = input;
    c->function = function;
    xr_lir_function_view(input, function, &c->view);
    XrLirStatus status = elide_allocate(c);
    if (status != XR_LIR_OK) return status;
    for (uint32_t b = 1; b <= c->view.block_count; ++b) elide_block(c, b);
    return XR_LIR_OK;
}
static bool elide_keep(void *context, uint32_t block) {
    (void)context; (void)block;
    return true;
}
static void elide_project(void *context, uint32_t block, uint32_t ordinal,
    XrLirInstructionView *v) {
    const Elide *c = context;
    XR_DCHECK(block && block <= c->view.block_count, "projection visits verified blocks");
    if (c->omit[c->first[block] + ordinal]) v->op = XR_LIR_OP_INVALID;
}

XR_FUNC XrLirStatus xr_lir_string_elide(const XrLirModule *input, XrLirModule **output) {
    Elide c = {0};
    const XrLirCopyPlan plan = {&c, elide_begin, elide_keep, elide_project, elide_end};
    return xr_lir_internal_copy(input, output, &plan);
}
