/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_slice_copy.h - Resolve non-reading keeper identities after SSA copying
 */
#ifndef XLIR_SLICE_COPY_H
#define XLIR_SLICE_COPY_H
#include "xlir_internal.h"
#include "base/xmalloc.h"

typedef struct LirSliceCopyCleanups {
    XrLirSliceCleanupInsertion *items;
    size_t count, capacity;
} LirSliceCopyCleanups;

static XrLirStatus lir_slice_copy_cleanup_record(LirSliceCopyCleanups *pending, XrLirModule *output,
    XrLirBlock block, const XrLirInstructionView *source) {
    if (pending->count == pending->capacity) {
        size_t capacity = pending->capacity ? pending->capacity : 4;
        if (capacity > SIZE_MAX / 2 || capacity * 2 > SIZE_MAX / sizeof(*pending->items)) return XR_LIR_OUT_OF_MEMORY;
        capacity *= 2;
        void *grown = xr_realloc(pending->items, capacity * sizeof(*pending->items));
        if (!grown) return XR_LIR_OUT_OF_MEMORY;
        pending->items = grown; pending->capacity = capacity;
    }
    uint32_t ordinal = output->functions[block.function - 1].blocks[block.id - 1].instruction_count;
    pending->items[pending->count++] = (XrLirSliceCleanupInsertion){.block=block,.ordinal=ordinal,
        .target={output,block.function,(uint32_t)source->bits},.source=source->source,
        .formal=source->op == XR_LIR_SLICE_FORMAL_CLEANUP};
    return XR_LIR_OK;
}

static XrLirStatus lir_slice_copy_cleanup_finish(LirSliceCopyCleanups *pending, XrLirModule *output,
    XrLirFunction function, const XrLirValue *values, uint32_t count) {
    for (size_t n = 0; n < pending->count; ++n) {
        uint32_t old = pending->items[n].target.id;
        if (!old || old > count || !values[old].id) return XR_LIR_VERIFY_ERROR;
        pending->items[n].target = values[old];
    }
    return xr_lir_internal_slice_cleanup_batch(output, function, pending->items, pending->count);
}
#endif
