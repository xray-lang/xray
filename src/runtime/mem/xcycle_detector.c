/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcycle_detector.c - see xcycle_detector.h
 */

#ifdef XR_ENABLE_CYCLE_DETECTOR

#include "xcycle_detector.h"
#include "xcoro_heap.h"
#include "xobj_graph.h"
#include "xobj_header.h"
#include "xregion.h"
#include "../class/xclass.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* ========== Whole-heap object enumeration ==========
 *
 * The collector finds its starting points in a candidate-root buffer, kept up
 * to date by add_root/remove_root on the RC hot path. The detector does not:
 * it walks the heap directly, which is what lets task 247 phase E delete that
 * buffer, the _rsv root-index invariant, and the per-object candidate bit
 * along with it.
 *
 * Two sources, and BOTH are needed. Region blocks hold small objects; large
 * ones (> XR_LARGE_OBJECT_THRESHOLD) are malloc'd or mmap'd outside any block
 * and only appear in heap->large_set. Walking blocks alone silently misses
 * every large object, and a cycle through one would go unreported.
 */

typedef void (*XrHeapObjectFn)(XrObjHeader *obj, void *ctx);

/* Walk one region block's objects linearly.
 *
 * Step is exact: objsize is the bump distance (new_obj rounds to 8 and
 * xr_region_alloc's own 8-byte alignment is then a no-op), and a block holds
 * nothing but objects — xr_region_alloc has exactly one caller.
 *
 * Crucially objsize stays valid after an object dies: the freelist link is
 * written into the payload's first word, past the 16-byte header, so stepping
 * across dead objects does not lose sync. Were that not so, the walk would
 * desync at the first dead object and read garbage.
 *
 * Returns false if a self-check tripped. */
static bool detector_walk_block(XrRegionBlock *block, const char *end, XrHeapObjectFn fn,
                                void *ctx) {
    const char *p = (const char *) block + (size_t) XR_REGION_FIRST_LINE * XR_REGION_LINE_SIZE;
    const char *block_end = (const char *) block + XR_REGION_BLOCK_SIZE;
    if (end > block_end)
        end = block_end;

    while (p + sizeof(XrObjHeader) <= end) {
        XrObjHeader *obj = (XrObjHeader *) p;
        uint32_t size = obj->objsize;

        /* A zero step would spin forever. XR_OBJ_STORAGE_BUMP objects carry
         * objsize 0 and cannot reach a VM region block, so this is a corrupted
         * or misaligned walk rather than a shape to skip. Fail loudly. */
        if (size == 0) {
            fprintf(stderr,
                    "[cycle-detector] internal error: objsize 0 at %p in block %p; "
                    "aborting traversal\n",
                    (void *) obj, (void *) block);
            return false;
        }
        /* Desync is otherwise invisible — it just starts reading neighbouring
         * bytes as if they were headers. */
        if (p + size > block_end) {
            fprintf(stderr,
                    "[cycle-detector] internal error: object at %p size %u overruns block %p; "
                    "aborting traversal\n",
                    (void *) obj, size, (void *) block);
            return false;
        }

        if (!(obj->extra & XR_OBJ_DEAD))
            fn(obj, ctx);
        p += size;
    }
    return true;
}

/* Enumerate every live object on the heap. Returns false if a self-check
 * tripped anywhere, in which case the caller must not trust its results. */
static bool detector_for_each_object(XrCoroHeap *heap, XrHeapObjectFn fn, void *ctx) {
    if (!heap)
        return true;

    /* Retired blocks: alloc_bytes is monotonic within a block's activation and
     * marks how far the bump cursor got. */
    for (XrRegionBlock *b = heap->region.full_blocks; b; b = b->next) {
        const char *end = (const char *) b + (size_t) XR_REGION_FIRST_LINE * XR_REGION_LINE_SIZE +
                          (size_t) b->alloc_bytes;
        if (!detector_walk_block(b, end, fn, ctx))
            return false;
    }

    /* The block being filled: the live cursor is the boundary. */
    if (heap->region.current_block) {
        if (!detector_walk_block(heap->region.current_block, heap->region.cursor, fn, ctx))
            return false;
    }

    /* free_blocks is deliberately skipped: alloc_count / alloc_bytes are not
     * reset until activate_block, so their stale values point into memory that
     * has already been reclaimed. */

    /* Large objects live outside every block. */
    for (uint32_t i = 0; i < heap->large_set.cap; i++) {
        XrObjHeader *lo = heap->large_set.slots[i];
        if (!xr_heap_ptrset_slot_live(lo))
            continue;
        if (!(lo->extra & XR_OBJ_DEAD))
            fn(lo, ctx);
    }
    return true;
}

/* ========== Traversal self-check ========== */

static void detector_count_visitor(XrObjHeader *obj, void *ctx) {
    (void) obj;
    (*(uint32_t *) ctx)++;
}

bool xr_cycle_detector_count_live(XrCoroHeap *heap, uint32_t *out_count) {
    uint32_t n = 0;
    bool ok = detector_for_each_object(heap, detector_count_visitor, &n);
    if (out_count)
        *out_count = n;
    return ok;
}

/* ========== Candidate filtering ==========
 *
 * Class-level, from XrClass.flags — no per-object bit is consulted, which is
 * what lets phase E delete XR_OBJ_CYCLE_CANDIDATE and the store that sets it
 * on every allocation. */
static bool detector_object_is_candidate(const XrObjHeader *obj) {
    if (!xr_obj_graph_child_eligible(obj))
        return false;
    switch (obj->type) {
        case XR_TINSTANCE: {
            const XrClass *cls = ((const XrInstance *) obj)->klass;
            return cls && (cls->flags & XR_CLASS_CYCLE_CANDIDATE);
        }
        case XR_TCELL:
        case XR_TFUNCTION:
            /* A closure captures whatever it was built over; a cell exists to
             * be captured. Neither has a class-level answer, so both are
             * always candidates. */
            return true;
        default:
            return false;
    }
}

/* ========== Trial deletion, reporting variant ==========
 *
 * Bacon-Rajan's decision procedure, with the destruction removed and the
 * colours kept in a side table rather than in the object header (which is what
 * lets phase E reclaim extra bits 8-9).
 *
 *   A. reachable set R from every live candidate
 *   B. trial-decrement R's internal edges
 *   C. scanBlack the subgraphs an external reference keeps alive
 *   D. report what is left WHITE, then restore EVERY decrement
 *
 * Step D restoring in full is what makes this safe to run mid-program: the
 * heap is exactly as it was before the scan.
 */

typedef struct {
    XrObjHeader *obj;
    int32_t trial_rc; /* refcount under trial deletion */
    uint8_t color;
    uint32_t index;
} DetectorNode;

#define DET_COLOR_GRAY 0
#define DET_COLOR_BLACK 1
#define DET_COLOR_WHITE 2

typedef struct {
    DetectorNode *nodes;
    uint32_t count;
    uint32_t cap;
    bool oom;
} DetectorSet;

static DetectorNode *detector_find(DetectorSet *set, const XrObjHeader *obj) {
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->nodes[i].obj == obj)
            return &set->nodes[i];
    }
    return NULL;
}

static DetectorNode *detector_intern(DetectorSet *set, XrObjHeader *obj) {
    DetectorNode *existing = detector_find(set, obj);
    if (existing)
        return existing;
    if (set->count == set->cap) {
        uint32_t nc = set->cap ? set->cap * 2 : 64;
        DetectorNode *grown =
            (DetectorNode *) xr_realloc(set->nodes, (size_t) nc * sizeof(DetectorNode));
        if (!grown) {
            set->oom = true;
            return NULL;
        }
        set->nodes = grown;
        set->cap = nc;
    }
    DetectorNode *n = &set->nodes[set->count];
    n->obj = obj;
    /* 0-based sign-tagged RC: a coro-local object stores N references as N-1.
     * xr_obj_graph_child_eligible already excluded the shared band, so a plain
     * relaxed load is the count this pass works with. */
    n->trial_rc = atomic_load_explicit(&obj->refcount, memory_order_relaxed);
    n->color = DET_COLOR_GRAY;
    n->index = set->count;
    set->count++;
    return n;
}

typedef struct {
    DetectorSet *set;
    bool *changed;
} ReachCtx;

static void detector_reach_visitor(XrObjHeader *child, void *ctx) {
    ReachCtx *rc = (ReachCtx *) ctx;
    if (!xr_obj_graph_child_eligible(child) || (child->extra & XR_OBJ_DEAD))
        return;
    if (detector_find(rc->set, child))
        return;
    if (detector_intern(rc->set, child) && rc->changed)
        *rc->changed = true;
}

static void detector_dec_visitor(XrObjHeader *child, void *ctx) {
    DetectorSet *set = (DetectorSet *) ctx;
    DetectorNode *n = detector_find(set, child);
    if (n)
        n->trial_rc--;
}

static void detector_inc_visitor(XrObjHeader *child, void *ctx) {
    DetectorSet *set = (DetectorSet *) ctx;
    DetectorNode *n = detector_find(set, child);
    if (n)
        n->trial_rc++;
}

typedef struct {
    DetectorSet *set;
    bool *progressed;
} ScanBlackCtx;

static void detector_scan_black_visitor(XrObjHeader *child, void *ctx) {
    ScanBlackCtx *sb = (ScanBlackCtx *) ctx;
    DetectorNode *n = detector_find(sb->set, child);
    if (!n)
        return;
    n->trial_rc++;
    if (n->color != DET_COLOR_BLACK) {
        n->color = DET_COLOR_BLACK;
        if (sb->progressed)
            *sb->progressed = true;
    }
}

/* ========== Reporting ========== */

static const char *detector_type_name(const XrObjHeader *obj) {
    switch (obj->type) {
        case XR_TINSTANCE: {
            const XrClass *cls = ((const XrInstance *) obj)->klass;
            if (!cls || !cls->name)
                return "instance";
            /* A class name is an interned symbol, and the symbol table can be
             * gone by the time a coroutine's heap is torn down — printing the
             * pointer then emits whatever bytes are left there. Report the
             * shape instead of garbage: a name that is not plausibly a name is
             * not worth guessing at. The cycle itself, its size, and the edge
             * kinds are all still accurate. */
            const char *n = cls->name;
            for (int i = 0; i < 64; i++) {
                unsigned char c = (unsigned char) n[i];
                if (c == 0)
                    return i > 0 ? n : "instance";
                if (c < 0x20 || c > 0x7e)
                    return "instance";
            }
            return "instance";
        }
        case XR_TCELL:
            return "cell";
        case XR_TFUNCTION:
            return "closure";
        case XR_TARRAY:
            return "array";
        case XR_TMAP:
            return "map";
        case XR_TSET:
            return "set";
        default:
            /* The three shapes above plus instance/cell/closure are everything
             * that can hold a followable edge (xobj_graph.h), so nothing that
             * reaches a report lands here. */
            return "object";
    }
}

/* Which of `from`'s fields / elements / captures points at `to`?
 *
 * This is what separates a report you can act on from one that only says "you
 * leaked". Listing the edge lets the reader see which one to break. */
typedef struct {
    const XrObjHeader *target;
    const char *label;
    char buf[64];
} EdgeLabelCtx;

static void detector_edge_label_visitor(XrObjHeader *child, void *ctx) {
    EdgeLabelCtx *e = (EdgeLabelCtx *) ctx;
    if (child == e->target && !e->label)
        e->label = "";
}

static const char *detector_edge_label(XrObjHeader *from, const XrObjHeader *to) {
    /* Field / element identity needs per-type walking that xobj_graph.h does
     * not expose. Report the holder's shape, which already narrows it to one
     * declaration in the source. */
    EdgeLabelCtx ctx = {.target = to, .label = NULL, .buf = {0}};
    xr_obj_graph_visit_children(from, detector_edge_label_visitor, &ctx);
    if (!ctx.label)
        return NULL;
    switch (from->type) {
        case XR_TINSTANCE:
            return "field";
        case XR_TCELL:
            return "captured cell";
        case XR_TFUNCTION:
            return "closure capture";
        case XR_TARRAY:
            return "element";
        case XR_TMAP:
            return "map entry";
        case XR_TSET:
            return "set element";
        default:
            return "reference";
    }
}

typedef struct {
    DetectorSet *set;
    XrCycleReport *report;
    bool header_printed;
} EmitCtx;

static void detector_emit_cycle(EmitCtx *emit, DetectorNode **members, uint32_t count) {
    XrCycleReport *r = emit->report;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < count; i++)
        bytes += members[i]->obj->objsize;

    r->cycle_count++;
    r->object_count += count;
    r->byte_count += bytes;

    fprintf(stderr, "\n引用环 (%u 个对象, %llu 字节)\n", count, (unsigned long long) bytes);
    for (uint32_t i = 0; i < count; i++) {
        XrObjHeader *from = members[i]->obj;
        XrObjHeader *to = members[(i + 1) % count]->obj;
        const char *edge = detector_edge_label(from, to);
        fprintf(stderr, "  %-16s @ %p", detector_type_name(from), (void *) from);
        if (edge && count > 1)
            fprintf(stderr, "  ──%s──▶", edge);
        fprintf(stderr, "\n");
    }

    /* Machine-readable alongside, for the LSP code actions in phase H. */
    fprintf(stderr, "  #cycle objects=%u bytes=%llu members=", count, (unsigned long long) bytes);
    for (uint32_t i = 0; i < count; i++)
        fprintf(stderr, "%s%s@%p", i ? "," : "", detector_type_name(members[i]->obj),
                (void *) members[i]->obj);
    fprintf(stderr, "\n");

    fprintf(stderr, "  修复建议:\n");

    /* Which edges can `weak` actually break?
     *
     * It is a FIELD modifier, so it reaches an instance's field and nothing
     * else. An edge whose target is a closure or a cell is a capture edge —
     * no annotation exists for it, and the only fix is to clear the field that
     * holds the closure. A cycle can contain both kinds, so both halves of the
     * advice are printed independently rather than as an either/or. */
    bool has_capture_edge = false;
    bool has_annotatable_edge = false;
    for (uint32_t i = 0; i < count; i++) {
        XrObjHeader *from = members[i]->obj;
        XrObjHeader *to = members[(i + 1) % count]->obj;
        if (to->type == XR_TFUNCTION || to->type == XR_TCELL)
            has_capture_edge = true;
        else if (from->type == XR_TINSTANCE)
            has_annotatable_edge = true;
    }

    if (has_annotatable_edge) {
        /* Every candidate is listed and none is recommended: which edge to
         * break is an ownership decision, and the language does not guess
         * (247 section 2.5 — picking wrong turns a leak into a premature null). */
        fprintf(stderr, "    · 将下列任一引用标注为 weak（选哪条是所有权决定）:\n");
        for (uint32_t i = 0; i < count; i++) {
            XrObjHeader *from = members[i]->obj;
            XrObjHeader *to = members[(i + 1) % count]->obj;
            if (from->type != XR_TINSTANCE)
                continue;
            if (to->type == XR_TFUNCTION || to->type == XR_TCELL)
                continue; /* capture edge: weak cannot reach it */
            fprintf(stderr, "        %s ──▶ %s\n", detector_type_name(from),
                    detector_type_name(to));
        }
    }
    if (has_capture_edge) {
        fprintf(stderr, "    · 环上有闭包捕获边，weak 管不到它。"
                        "在作用域末尾用 defer 清空持有该闭包的字段:\n");
        for (uint32_t i = 0; i < count; i++) {
            XrObjHeader *from = members[i]->obj;
            XrObjHeader *to = members[(i + 1) % count]->obj;
            if (to->type != XR_TFUNCTION && to->type != XR_TCELL)
                continue;
            fprintf(stderr, "        defer <%s 持有该闭包的字段> = ...\n",
                    detector_type_name(from));
        }
    }
}

/* ========== Entry point ========== */

static bool g_detector_found_any = false;

void xr_cycle_detector_accumulate(const XrCycleReport *report) {
    if (report && report->cycle_count > 0)
        g_detector_found_any = true;
}

bool xr_cycle_detector_any_found(void) {
    return g_detector_found_any;
}

void xr_cycle_detector_reset(void) {
    g_detector_found_any = false;
}

typedef struct {
    DetectorSet *set;
} SeedCtx;

static void detector_seed_visitor(XrObjHeader *obj, void *ctx) {
    SeedCtx *s = (SeedCtx *) ctx;
    if (detector_object_is_candidate(obj))
        (void) detector_intern(s->set, obj);
}

bool xr_cycle_detector_scan(XrCoroHeap *heap, XrCycleReport *out) {
    XrCycleReport local = {0};
    XrCycleReport *report = out ? out : &local;
    memset(report, 0, sizeof(*report));
    if (!heap)
        return false;

    DetectorSet set = {0};

    /* Pass A: every live candidate, then everything reachable from them. */
    SeedCtx seed = {.set = &set};
    if (!detector_for_each_object(heap, detector_seed_visitor, &seed)) {
        report->traversal_failed = true;
        xr_free(set.nodes);
        return false;
    }
    bool changed = true;
    while (changed && !set.oom) {
        changed = false;
        uint32_t n = set.count;
        for (uint32_t i = 0; i < n; i++) {
            ReachCtx rc = {.set = &set, .changed = &changed};
            xr_obj_graph_visit_children(set.nodes[i].obj, detector_reach_visitor, &rc);
        }
    }
    if (set.oom) {
        fprintf(stderr, "[cycle-detector] out of memory building the reachable set; "
                        "results incomplete\n");
        report->traversal_failed = true;
        xr_free(set.nodes);
        return false;
    }

    /* Pass B: trial-decrement internal edges. */
    for (uint32_t i = 0; i < set.count; i++)
        xr_obj_graph_visit_children(set.nodes[i].obj, detector_dec_visitor, &set);

    /* Pass C: anything with a surviving external reference is live, and so is
     * everything it reaches. */
    for (uint32_t i = 0; i < set.count; i++) {
        if (set.nodes[i].color == DET_COLOR_GRAY && set.nodes[i].trial_rc >= 0)
            set.nodes[i].color = DET_COLOR_BLACK;
    }
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (uint32_t i = 0; i < set.count; i++) {
            if (set.nodes[i].color != DET_COLOR_BLACK)
                continue;
            ScanBlackCtx sb = {.set = &set, .progressed = &progressed};
            xr_obj_graph_visit_children(set.nodes[i].obj, detector_scan_black_visitor, &sb);
        }
    }

    /* Whatever is still GRAY is unreachable from outside: a dead cycle. */
    for (uint32_t i = 0; i < set.count; i++) {
        if (set.nodes[i].color == DET_COLOR_GRAY)
            set.nodes[i].color = DET_COLOR_WHITE;
    }

    /* Pass D: report, grouping the white set into individual cycles. */
    EmitCtx emit = {.set = &set, .report = report, .header_printed = false};
    bool *grouped = (bool *) xr_calloc(set.count ? set.count : 1, sizeof(bool));
    DetectorNode **members =
        (DetectorNode **) xr_malloc((set.count ? set.count : 1) * sizeof(DetectorNode *));
    if (grouped && members) {
        for (uint32_t i = 0; i < set.count; i++) {
            if (set.nodes[i].color != DET_COLOR_WHITE || grouped[i])
                continue;
            /* Collect this cycle's members: everything white reachable from
             * here. A conservative grouping — two cycles sharing an object are
             * reported as one — which reads better than splitting them. */
            uint32_t count = 0;
            uint32_t queue_head = 0;
            members[count++] = &set.nodes[i];
            grouped[i] = true;
            while (queue_head < count) {
                DetectorNode *cur = members[queue_head++];
                for (uint32_t k = 0; k < set.count; k++) {
                    if (grouped[k] || set.nodes[k].color != DET_COLOR_WHITE)
                        continue;
                    /* Edge test: does cur point at nodes[k]? */
                    EdgeLabelCtx probe = {.target = set.nodes[k].obj, .label = NULL, .buf = {0}};
                    xr_obj_graph_visit_children(cur->obj, detector_edge_label_visitor, &probe);
                    if (probe.label) {
                        members[count++] = &set.nodes[k];
                        grouped[k] = true;
                    }
                }
            }
            detector_emit_cycle(&emit, members, count);
        }
    }
    xr_free(grouped);
    xr_free(members);

    /* Restore EVERY trial decrement, so the heap is byte-identical to before
     * the scan. Without this the detector would be a destructive operation
     * masquerading as an observation. */
    for (uint32_t i = 0; i < set.count; i++)
        xr_obj_graph_visit_children(set.nodes[i].obj, detector_inc_visitor, &set);

    xr_free(set.nodes);

    if (report->cycle_count > 0) {
        fprintf(stderr, "\n[cycle-detector] %u 个引用环, %u 个对象, %llu 字节未回收\n",
                report->cycle_count, report->object_count, (unsigned long long) report->byte_count);
    }
    xr_cycle_detector_accumulate(report);
    return report->cycle_count > 0;
}

#endif /* XR_ENABLE_CYCLE_DETECTOR */
