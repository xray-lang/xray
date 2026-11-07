/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_scc_internal.h - Iterative closure of numeric effect dependencies
 *
 * KEY CONCEPT:
 *   A component inherits local masks and already closed dependencies. The
 *   workspace reads no function body and expires before result publication.
 */
#ifndef XXIR_EFFECT_SCC_INTERNAL_H
#define XXIR_EFFECT_SCC_INTERNAL_H

#include "xxir_compile_memory.h"
#include "../base/xchecks.h"

typedef struct XirEffectSccNode { uint32_t head, seed, closed; } XirEffectSccNode;
typedef struct XirEffectSccEdge { uint32_t target, next; } XirEffectSccEdge;
typedef struct XirEffectSccFrame { uint32_t node, edge, parent; } XirEffectSccFrame;

static XrXirStatus xir_effect_scc_close(const XrXirCompileContext *memory,
    XirEffectSccNode *nodes, uint32_t count, const XirEffectSccEdge *edges,
    uint32_t edge_count, XrCompileRequestControl *control) {
    if (!xir_compile_context_valid(memory) || (count && !nodes) || (edge_count && !edges))
        return XR_XIR_BAD_STRUCTURE;
    if (!count) return XR_XIR_OK;
    uint64_t frame_bytes = (uint64_t)count * sizeof(XirEffectSccFrame);
    if (frame_bytes > SIZE_MAX) return XR_XIR_OVERFLOW;
    XrXirStatus status = control ? xir_compile_resource_status(xr_compile_request_control_poll(control)) : XR_XIR_OK;
    uint32_t *index = xir_compile_calloc(memory, count, sizeof(*index), &status);
    uint32_t *low = xir_compile_calloc(memory, count, sizeof(*low), &status);
    uint32_t *component = xir_compile_alloc(memory, (size_t)count * sizeof(*component), &status);
    uint32_t *stack = xir_compile_alloc(memory, (size_t)count * sizeof(*stack), &status);
    XirEffectSccFrame *frames = xir_compile_alloc(memory, (size_t)frame_bytes, &status);
    if (!index || !low || !component || !stack || !frames) goto done;
    for (uint32_t n = 0; n < count; ++n) component[n] = UINT32_MAX;
    uint32_t next = 0, components = 0, stacked = 0, progress = 0;
    for (uint32_t start = 0; start < count && status == XR_XIR_OK; ++start) {
        if (index[start]) continue;
        uint32_t depth = 1;
        frames[0] = (XirEffectSccFrame){start, nodes[start].head, UINT32_MAX};
        index[start] = low[start] = ++next;
        stack[stacked++] = start;
        while (depth && status == XR_XIR_OK) {
            if (control && !(++progress & 255u)) {
                status = xir_compile_resource_status(xr_compile_request_control_poll(control));
                if (status != XR_XIR_OK) break;
            }
            XirEffectSccFrame *frame = &frames[depth - 1];
            uint32_t node = frame->node;
            if (frame->edge != UINT32_MAX) {
                if (frame->edge >= edge_count) { status = XR_XIR_BAD_STRUCTURE; break; }
                const XirEffectSccEdge *edge = &edges[frame->edge];
                frame->edge = edge->next;
                uint32_t target = edge->target;
                if (target >= count) { status = XR_XIR_BAD_STRUCTURE; break; }
                if (!index[target]) {
                    index[target] = low[target] = ++next;
                    stack[stacked++] = target;
                    frames[depth++] = (XirEffectSccFrame){target, nodes[target].head, node};
                } else if (component[target] == UINT32_MAX && index[target] < low[node]) {
                    low[node] = index[target];
                }
                continue;
            }
            uint32_t parent = frame->parent;
            --depth;
            if (low[node] == index[node]) {
                uint32_t end = stacked, mask = 0;
                do {
                    XR_DCHECK(stacked, "An active SCC owns at least its root");
                    uint32_t member = stack[--stacked];
                    component[member] = components;
                    mask |= nodes[member].seed;
                } while (stack[stacked] != node);
                for (uint32_t at = stacked; at < end && status == XR_XIR_OK; ++at) {
                    for (uint32_t e = nodes[stack[at]].head; e != UINT32_MAX; e = edges[e].next) {
                        if (e >= edge_count || edges[e].target >= count) { status = XR_XIR_BAD_STRUCTURE; break; }
                        uint32_t target = edges[e].target;
                        XR_DCHECK(component[target] != UINT32_MAX, "Dependencies close before their callers");
                        if (component[target] != components) mask |= nodes[target].closed;
                    }
                }
                for (uint32_t at = stacked; at < end; ++at) nodes[stack[at]].closed = mask;
                ++components;
            }
            if (parent != UINT32_MAX && low[node] < low[parent]) low[parent] = low[node];
        }
    }
done:
    xr_compile_resources_free(index);
    xr_compile_resources_free(low);
    xr_compile_resources_free(component);
    xr_compile_resources_free(stack);
    xr_compile_resources_free(frames);
    return status;
}

#endif // XXIR_EFFECT_SCC_INTERNAL_H
