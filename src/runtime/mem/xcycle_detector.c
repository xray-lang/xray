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
#include "../xshared.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../coro/xchannel.h"
#include "../../coro/xtask.h"
#include "../../os/os_thread.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Domain mode ==========
 *
 * One pipeline serves two graphs. The per-heap scan walks the coro-local band;
 * the shared scan walks the atomic band, where the extra edge kinds live
 * (channel buffers, task payloads). Scans run only at quiescent points, one at
 * a time, so a file-static mode flag is race-free by construction. */
static bool g_scan_shared_domain = false;

/* The shared-domain graph spans BOTH system-heap bands. A value sent into a
 * channel is materialised as a TRANSFER object; the canonical shared cycle
 * (closure captures a channel, gets sent into that channel) is therefore
 * shared -> transfer -> shared. Transfer objects alone cannot form a cycle —
 * unique ownership admits no second owner — so every system-heap cycle passes
 * through at least one atomic-band object, and seeding from the shared
 * registry is complete. */
static bool detector_shared_child_eligible(const XrObjHeader *obj) {
    if (obj->extra & (XR_OBJ_MANAGED | XR_OBJ_STORAGE_BUMP))
        return false;
    if (XR_OBJ_IS_SHARED(obj))
        return true;
    return XR_OBJ_GET_STORAGE(obj) == XR_OBJ_STORAGE_TRANSFER;
}

static bool detector_child_eligible(const XrObjHeader *obj) {
    return g_scan_shared_domain ? detector_shared_child_eligible(obj)
                                : xr_obj_graph_child_eligible(obj);
}

/* The logical reference count on the trial-deletion basis (0-based: an object
 * with N live references reads N-1). Coro-local and transfer objects store
 * exactly that; the shared band stores a live count of N as -N. Dispatch is
 * per object, not per scan: a shared-domain component mixes both bands. */
static int32_t detector_trial_basis_rc(XrObjHeader *obj) {
    if (g_scan_shared_domain && XR_OBJ_IS_SHARED(obj))
        return (int32_t) xr_shared_get_refc(obj) - 1;
    return atomic_load_explicit(&obj->refcount, memory_order_relaxed);
}

/* Graph traversal, extended in shared mode with the edges that only exist in
 * that domain. Only edges that correspond to a counted reference may appear
 * here: a buffered channel value was retained by send, a task holds its
 * result/error. Task-tree links (parent/child/sibling) are runtime-managed
 * bookkeeping, not counted references — traversing them would trial-decrement
 * a count that was never incremented and could fabricate a cycle. Leaving an
 * uncounted edge out can only under-report, never invent. */
static void detector_visit_children(XrObjHeader *obj, XrObjGraphVisitor visitor, void *ctx) {
    xr_obj_graph_visit_children(obj, visitor, ctx);
    if (!g_scan_shared_domain)
        return;
    switch (obj->type) {
        case XR_TCHANNEL: {
            XrChannel *ch = (XrChannel *) obj;
            if (!ch->buffer || ch->buf_size == 0)
                break;
            for (uint32_t i = 0; i < ch->buf_count; i++) {
                XrValue v = ch->buffer[(ch->recv_idx + i) % ch->buf_size];
                if (XR_IS_PTR(v) && !XR_IS_STRING(v)) {
                    XrObjHeader *child = XR_VALUE_GCPTR(v);
                    if (child)
                        visitor(child, XR_OBJ_GRAPH_SLOT_NONE, ctx);
                }
            }
            break;
        }
        case XR_TTASK: {
            XrTask *task = (XrTask *) obj;
            if (XR_IS_PTR(task->result) && !XR_IS_STRING(task->result)) {
                XrObjHeader *child = XR_VALUE_GCPTR(task->result);
                if (child)
                    visitor(child, XR_OBJ_GRAPH_SLOT_NONE, ctx);
            }
            if (XR_IS_PTR(task->error) && !XR_IS_STRING(task->error)) {
                XrObjHeader *child = XR_VALUE_GCPTR(task->error);
                if (child)
                    visitor(child, XR_OBJ_GRAPH_SLOT_NONE, ctx);
            }
            break;
        }
        default:
            break;
    }
}

/* ========== Whole-heap object enumeration ==========
 *
 * The removed collector found its starting points in a candidate-root buffer
 * maintained on the RC hot path. The detector walks the heap directly, so the
 * buffer, the _rsv root-index invariant, and the per-object candidate bit are
 * unnecessary.
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

        /* A zero step would spin forever. XR_OBJ_IMMORTAL objects carry
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
 * Class-level, from XrClass.flags. No per-object bit is consulted, so
 * XR_OBJ_CYCLE_CANDIDATE and its per-allocation store are unnecessary. */
static bool detector_object_is_candidate(const XrObjHeader *obj) {
    if (!detector_child_eligible(obj))
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
 * colours kept in a side table rather than in the object header, leaving extra
 * bits 8-9 available.
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
    /* 0-based trial basis: a coro-local object stores N references as N-1 and
     * is read directly; the shared band stores -N and is normalised by
     * detector_trial_basis_rc. Which band applies follows the scan mode. */
    n->trial_rc = detector_trial_basis_rc(obj);
    n->color = DET_COLOR_GRAY;
    n->index = set->count;
    set->count++;
    return n;
}

typedef struct {
    DetectorSet *set;
    bool *changed;
} ReachCtx;

static void detector_reach_visitor(XrObjHeader *child, uint32_t slot, void *ctx) {
    (void) slot;
    ReachCtx *rc = (ReachCtx *) ctx;
    if (!detector_child_eligible(child) || (child->extra & XR_OBJ_DEAD))
        return;
    if (detector_find(rc->set, child))
        return;
    if (detector_intern(rc->set, child) && rc->changed)
        *rc->changed = true;
}

static void detector_dec_visitor(XrObjHeader *child, uint32_t slot, void *ctx) {
    (void) slot;
    DetectorSet *set = (DetectorSet *) ctx;
    DetectorNode *n = detector_find(set, child);
    if (n)
        n->trial_rc--;
}

static void detector_inc_visitor(XrObjHeader *child, uint32_t slot, void *ctx) {
    (void) slot;
    DetectorSet *set = (DetectorSet *) ctx;
    DetectorNode *n = detector_find(set, child);
    if (n)
        n->trial_rc++;
}

typedef struct {
    DetectorSet *set;
    bool *progressed;
} ScanBlackCtx;

static void detector_scan_black_visitor(XrObjHeader *child, uint32_t slot, void *ctx) {
    (void) slot;
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
            if (!cls)
                return "instance";
            /* The snapshot taken at class construction, when the name was
             * still valid. Falls through to the live pointer only for classes
             * that were never registered. */
            const char *snap = xr_cycle_detector_class_name(cls);
            if (snap)
                return snap;
            if (!cls->name)
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
        case XR_TCHANNEL:
            return "channel";
        case XR_TTASK:
            return "task";
        default:
            /* The three shapes above plus instance/cell/closure are everything
             * that can hold a followable edge (xobj_graph.h), so nothing that
             * reaches a report lands here. */
            return "object";
    }
}

/* Does `from` reference `to` at all? Used while splitting a component, where
 * only the existence of the edge matters — naming it is detector_collect_edges'
 * job, after the component's membership is final. */
typedef struct {
    const XrObjHeader *target;
    const char *label;
    uint32_t slot;
} EdgeLabelCtx;

static void detector_edge_label_visitor(XrObjHeader *child, uint32_t slot, void *ctx) {
    EdgeLabelCtx *e = (EdgeLabelCtx *) ctx;
    if (child == e->target && !e->label) {
        e->label = "";
        e->slot = slot;
    }
}

/* What kind of reference the edge is, in the holder's own vocabulary. */
static const char *detector_edge_kind(const XrObjHeader *from) {
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
        case XR_TCHANNEL:
            return "buffered value";
        case XR_TTASK:
            return "task payload";
        default:
            return "reference";
    }
}

/* ========== Sidecar report ==========
 *
 * The cycle happens in the process that ran the program; the fix happens in an
 * editor attached to a different one. The stderr report is for a human reading
 * a terminal, and an editor cannot act on it — so the same findings are also
 * written out as class+field identities the LSP can resolve back to a field
 * declaration and offer as a code action.
 *
 * Accumulated across every scan (each coroutine heap plus the root heap runs
 * its own) and flushed once at exit, because a cycle spanning two heaps must
 * not land in two files that overwrite each other. */
static char *g_sidecar_buf;
static size_t g_sidecar_len;
static size_t g_sidecar_cap;
static bool g_sidecar_hooked;
static uint32_t g_sidecar_cycles;

static void detector_sidecar_put(const char *fmt, ...) {
    for (int attempt = 0; attempt < 2; attempt++) {
        size_t room = g_sidecar_cap - g_sidecar_len;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(g_sidecar_buf ? g_sidecar_buf + g_sidecar_len : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if ((size_t) n < room) {
            g_sidecar_len += (size_t) n;
            return;
        }
        size_t want = g_sidecar_cap ? g_sidecar_cap : 4096;
        while (want < g_sidecar_len + (size_t) n + 1)
            want *= 2;
        char *grown = (char *) xr_realloc(g_sidecar_buf, want);
        if (!grown)
            return; /* Out of memory writing a diagnostic: drop it, do not abort. */
        g_sidecar_buf = grown;
        g_sidecar_cap = want;
    }
}

/* Class and field names are identifiers, so this only ever has to handle the
 * degenerate cases — but a report that emits invalid JSON is worse than none. */
static void detector_sidecar_put_json_string(const char *s) {
    detector_sidecar_put("\"");
    for (const char *p = s; p && *p; p++) {
        unsigned char c = (unsigned char) *p;
        if (c == '"' || c == '\\')
            detector_sidecar_put("\\%c", c);
        else if (c < 0x20)
            detector_sidecar_put("\\u%04x", c);
        else
            detector_sidecar_put("%c", c);
    }
    detector_sidecar_put("\"");
}

static const char *detector_sidecar_path(void) {
    const char *env = getenv("XRAY_CYCLE_REPORT");
    return (env && *env) ? env : ".xray-cycles.json";
}

static void detector_sidecar_flush(void) {
    if (!g_sidecar_buf || g_sidecar_cycles == 0)
        return;
    const char *path = detector_sidecar_path();
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[cycle-detector] unable to write sidecar report %s\n", path);
        return;
    }
    fprintf(f, "{\"version\":1,\"source\":\"cycle-detector\",\"cycles\":[%s]}\n", g_sidecar_buf);
    fclose(f);
    fprintf(stderr, "[cycle-detector] sidecar report written: %s\n", path);
}

typedef struct {
    DetectorSet *set;
    XrCycleReport *report;
    bool header_printed;
} EmitCtx;

/* ---------- Edges inside one component ----------
 *
 * `members` is the set of objects in a strongly connected component, NOT a
 * walk around a cycle: consecutive entries need not reference each other, and
 * a component with several interlocking cycles has no single path through it.
 * Assuming otherwise silently drops real candidate edges and invents ones that
 * do not exist — missing a candidate would hide a valid ownership repair.
 *
 * So enumerate: for every member, every child that lands back inside the
 * component is one edge. Components are small; the linear membership test is
 * not worth an index. */
typedef struct {
    uint32_t from;
    uint32_t to;
    uint32_t slot;
} CycleEdge;

typedef struct {
    DetectorNode **members;
    uint32_t count;
    uint32_t current;
    CycleEdge *edges;
    uint32_t edge_count;
    uint32_t edge_cap;
    bool overflow;
} CycleEdgeCtx;

static void cycle_edge_visitor(XrObjHeader *child, uint32_t slot, void *ctx) {
    CycleEdgeCtx *c = (CycleEdgeCtx *) ctx;
    uint32_t to = UINT32_MAX;
    for (uint32_t i = 0; i < c->count; i++) {
        if (c->members[i]->obj == child) {
            to = i;
            break;
        }
    }
    if (to == UINT32_MAX)
        return; /* leaves the component: not part of what keeps it alive */
    if (c->edge_count == c->edge_cap) {
        uint32_t cap = c->edge_cap ? c->edge_cap * 2 : 16;
        CycleEdge *grown = (CycleEdge *) xr_realloc(c->edges, cap * sizeof(*grown));
        if (!grown) {
            c->overflow = true;
            return;
        }
        c->edges = grown;
        c->edge_cap = cap;
    }
    c->edges[c->edge_count++] = (CycleEdge) {.from = c->current, .to = to, .slot = slot};
}

static void detector_collect_edges(CycleEdgeCtx *c, DetectorNode **members, uint32_t count) {
    memset(c, 0, sizeof(*c));
    c->members = members;
    c->count = count;
    for (uint32_t i = 0; i < count; i++) {
        c->current = i;
        detector_visit_children(members[i]->obj, cycle_edge_visitor, c);
    }
}

/* `weak` is a field modifier: it reaches an instance field and nothing else.
 * An edge into a closure or a cell is a capture edge, which no annotation can
 * break — the only fix is clearing the field that holds the closure.
 *
 * Necessary, not sufficient. Whether the modifier actually COMPILES on a given
 * field depends on its declared type (nullable reference, per E0394/W2), and
 * that is invisible here: a runtime edge out of `T?` and out of
 * `int | T | null` are the same pointer. The LSP has the source and makes the
 * final call; this flag only rules out what the object graph alone can. */
static bool cycle_edge_is_capture(const CycleEdgeCtx *c, const CycleEdge *e) {
    uint8_t t = c->members[e->to]->obj->type;
    return t == XR_TFUNCTION || t == XR_TCELL;
}

static const char *cycle_edge_field(const CycleEdgeCtx *c, const CycleEdge *e) {
    XrObjHeader *from = c->members[e->from]->obj;
    if (from->type != XR_TINSTANCE || e->slot == XR_OBJ_GRAPH_SLOT_NONE)
        return NULL;
    return xr_cycle_detector_field_name(((const XrInstance *) from)->klass, e->slot);
}

/* Class name when the object is an instance of a registered class, else the
 * runtime shape — either way something the reader can match to source. */
static const char *cycle_obj_name(const XrObjHeader *o) {
    if (o->type == XR_TINSTANCE) {
        const char *n = xr_cycle_detector_class_name(((const XrInstance *) o)->klass);
        if (n)
            return n;
    }
    return detector_type_name(o);
}

/* Two objects of the same class holding the same field produce two runtime
 * edges but only ONE thing to change in the source. Collapsing them keeps the
 * candidate list a list of decisions rather than a list of instances — the LSP
 * would otherwise offer the identical code action several times. */
static bool cycle_edge_is_duplicate(const CycleEdgeCtx *c, uint32_t index) {
    const CycleEdge *e = &c->edges[index];
    const char *from = cycle_obj_name(c->members[e->from]->obj);
    const char *to = cycle_obj_name(c->members[e->to]->obj);
    const char *field = cycle_edge_field(c, e);
    for (uint32_t i = 0; i < index; i++) {
        const CycleEdge *prev = &c->edges[i];
        const char *pfield = cycle_edge_field(c, prev);
        if ((field == NULL) != (pfield == NULL))
            continue;
        if (field && pfield && strcmp(field, pfield) != 0)
            continue;
        if (strcmp(from, cycle_obj_name(c->members[prev->from]->obj)) != 0)
            continue;
        if (strcmp(to, cycle_obj_name(c->members[prev->to]->obj)) == 0)
            return true;
    }
    return false;
}

static void detector_emit_cycle(EmitCtx *emit, DetectorNode **members, uint32_t count) {
    XrCycleReport *r = emit->report;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < count; i++)
        bytes += members[i]->obj->objsize;

    r->cycle_count++;
    r->object_count += count;
    r->byte_count += bytes;

    CycleEdgeCtx edges;
    detector_collect_edges(&edges, members, count);

    fprintf(stderr, "\n%sreference cycle (%u objects, %llu bytes)\n", g_scan_shared_domain ? "shared domain " : "",
            count, (unsigned long long) bytes);
    for (uint32_t i = 0; i < count; i++)
        fprintf(stderr, "  %-16s @ %p\n", cycle_obj_name(members[i]->obj),
                (void *) members[i]->obj);
    for (uint32_t i = 0; i < edges.edge_count; i++) {
        const CycleEdge *e = &edges.edges[i];
        if (cycle_edge_is_duplicate(&edges, i))
            continue;
        const char *field = cycle_edge_field(&edges, e);
        fprintf(stderr, "    %s.%s ──▶ %s\n", cycle_obj_name(members[e->from]->obj),
                field ? field : detector_edge_kind(members[e->from]->obj),
                cycle_obj_name(members[e->to]->obj));
    }

    /* Machine-readable alongside, for the LSP code actions. */
    fprintf(stderr, "  #cycle objects=%u bytes=%llu members=", count, (unsigned long long) bytes);
    for (uint32_t i = 0; i < count; i++)
        fprintf(stderr, "%s%s@%p", i ? "," : "", detector_type_name(members[i]->obj),
                (void *) members[i]->obj);
    fprintf(stderr, "\n");

    /* Same findings, machine-side: class + field identities the LSP resolves
     * back to a declaration. Hooked lazily so a run with no cycle leaves no
     * file behind. */
    if (!g_sidecar_hooked) {
        atexit(detector_sidecar_flush);
        g_sidecar_hooked = true;
    }
    detector_sidecar_put("%s\n  {\"objects\":%u,\"bytes\":%llu,\"edges\":[",
                         g_sidecar_cycles ? "," : "", count, (unsigned long long) bytes);
    g_sidecar_cycles++;
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < edges.edge_count; i++) {
        const CycleEdge *e = &edges.edges[i];
        if (cycle_edge_is_duplicate(&edges, i))
            continue;
        XrObjHeader *from = members[e->from]->obj;
        const char *field = cycle_edge_field(&edges, e);
        detector_sidecar_put("%s\n    {\"from\":", emitted++ ? "," : "");
        detector_sidecar_put_json_string(cycle_obj_name(from));
        detector_sidecar_put(",\"to\":");
        detector_sidecar_put_json_string(cycle_obj_name(members[e->to]->obj));
        detector_sidecar_put(",\"field\":");
        if (field)
            detector_sidecar_put_json_string(field);
        else
            detector_sidecar_put("null");
        detector_sidecar_put(",\"kind\":");
        detector_sidecar_put_json_string(detector_edge_kind(from));
        detector_sidecar_put(",\"weak_annotatable\":%s}",
                             (field && !cycle_edge_is_capture(&edges, e) && !g_scan_shared_domain)
                                 ? "true"
                                 : "false");
    }
    detector_sidecar_put("\n  ]}");

    fprintf(stderr, "  Suggested fixes:\n");

    /* W4 forbids `weak` on a shared-domain object, so the coro-local advice
     * would be a lie here. What actually breaks a shared cycle is changing the
     * structure or clearing the reference explicitly before release. */
    if (g_scan_shared_domain) {
        fprintf(stderr, "    - Shared-domain objects forbid weak fields (W4). Valid fixes:\n");
        fprintf(stderr, "        - Replace the back-reference with an ID resolved by an external table.\n");
        fprintf(stderr, "        - Clear the channel-holding field after send completes, "
                        "or drain the buffer after close.\n");
        if (edges.overflow)
            fprintf(stderr, "    (out of memory; candidate edge list is incomplete)\n");
        xr_free(edges.edges);
        return;
    }

    /* A component can hold both kinds of edge, so both halves of the advice are
     * printed independently rather than as an either/or. */
    bool has_capture_edge = false;
    bool has_annotatable_edge = false;
    for (uint32_t i = 0; i < edges.edge_count; i++) {
        if (cycle_edge_is_capture(&edges, &edges.edges[i]))
            has_capture_edge = true;
        else if (cycle_edge_field(&edges, &edges.edges[i]))
            has_annotatable_edge = true;
    }

    if (has_annotatable_edge) {
        /* Every candidate is listed and none is recommended. Choosing which
         * edge to break is an ownership decision, and guessing incorrectly can
         * turn a leak into a premature null. */
        fprintf(stderr, "    - Mark one of these references weak; ownership determines which one:\n");
        for (uint32_t i = 0; i < edges.edge_count; i++) {
            const CycleEdge *e = &edges.edges[i];
            const char *field = cycle_edge_field(&edges, e);
            if (!field || cycle_edge_is_capture(&edges, e) || cycle_edge_is_duplicate(&edges, i))
                continue;
            fprintf(stderr, "        weak %s.%s  (weak means %s does not own %s)\n",
                    cycle_obj_name(members[e->from]->obj), field,
                    cycle_obj_name(members[e->from]->obj), cycle_obj_name(members[e->to]->obj));
        }
    }
    if (has_capture_edge) {
        fprintf(stderr, "    - The cycle contains a closure-capture edge that weak cannot break. "
                        "Use defer to clear the field holding that closure at scope exit:\n");
        for (uint32_t i = 0; i < edges.edge_count; i++) {
            const CycleEdge *e = &edges.edges[i];
            if (!cycle_edge_is_capture(&edges, e) || cycle_edge_is_duplicate(&edges, i))
                continue;
            const char *field = cycle_edge_field(&edges, e);
            if (field)
                fprintf(stderr, "        defer { <owner>.%s = null }\n", field);
            else
                fprintf(stderr, "        defer { <%s field holding the closure> = null }\n",
                        cycle_obj_name(members[e->from]->obj));
        }
    }
    if (edges.overflow)
        fprintf(stderr, "    (out of memory; candidate edge list is incomplete)\n");
    xr_free(edges.edges);
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

/* ========== Class-name snapshots ========== */

/* A small append-only table, owned by the detector and never freed: it must
 * outlive every heap it describes, and a development build's process exit is
 * the only correct point to drop it. */
typedef struct {
    const struct XrClass *cls;
    char *name;
    char **field_names; /* parallel to the class's instance fields */
    uint32_t field_count;
} DetectorClassName;

static DetectorClassName *g_class_names;
static uint32_t g_class_name_count;
static uint32_t g_class_name_cap;

void xr_cycle_detector_register_class(const struct XrClass *cls, const char *name) {
    if (!cls || !name || !*name)
        return;
    for (uint32_t i = 0; i < g_class_name_count; i++) {
        if (g_class_names[i].cls == cls)
            return;
    }
    if (g_class_name_count == g_class_name_cap) {
        uint32_t cap = g_class_name_cap ? g_class_name_cap * 2 : 16;
        DetectorClassName *grown =
            (DetectorClassName *) xr_realloc(g_class_names, cap * sizeof(*grown));
        if (!grown)
            return; /* best effort: a missing name degrades to "instance" */
        g_class_names = grown;
        g_class_name_cap = cap;
    }
    size_t len = strlen(name);
    char *copy = (char *) xr_malloc(len + 1);
    if (!copy)
        return;
    memcpy(copy, name, len + 1);
    DetectorClassName *slot = &g_class_names[g_class_name_count];
    slot->cls = cls;
    slot->name = copy;
    slot->field_names = NULL;
    slot->field_count = 0;

    /* Field names travel with the class name, and for the same reason: they are
     * interned symbols whose storage does not outlive the scan. */
    uint32_t fc = xr_class_instance_field_count(cls);
    if (fc > 0 && cls->fields) {
        slot->field_names = (char **) xr_calloc(fc, sizeof(char *));
        if (slot->field_names) {
            slot->field_count = fc;
            for (uint32_t i = 0; i < fc; i++) {
                const char *fn = cls->fields[i].name;
                if (!fn || !*fn) {
                    continue;
                }
                size_t flen = strlen(fn);
                char *fcopy = (char *) xr_malloc(flen + 1);
                if (!fcopy)
                    continue;
                memcpy(fcopy, fn, flen + 1);
                slot->field_names[i] = fcopy;
            }
        }
    }
    g_class_name_count++;
}

const char *xr_cycle_detector_field_name(const struct XrClass *cls, uint32_t index) {
    if (!cls)
        return NULL;
    for (uint32_t i = 0; i < g_class_name_count; i++) {
        if (g_class_names[i].cls != cls)
            continue;
        if (!g_class_names[i].field_names || index >= g_class_names[i].field_count)
            return NULL;
        return g_class_names[i].field_names[index];
    }
    return NULL;
}

const char *xr_cycle_detector_class_name(const struct XrClass *cls) {
    if (!cls)
        return NULL;
    for (uint32_t i = 0; i < g_class_name_count; i++) {
        if (g_class_names[i].cls == cls)
            return g_class_names[i].name;
    }
    return NULL;
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

/* Passes B..D over an already-seeded set: trial-decrement, scan-black, group
 * the white remainder into cycles, report, restore. Shared between the
 * per-heap scan and the shared-domain scan; `g_scan_shared_domain` selects the
 * band, the traversal, and the advice text. Consumes set->nodes. */
static bool detector_run_passes(DetectorSet *set, XrCycleReport *report) {
    bool changed = true;
    while (changed && !set->oom) {
        changed = false;
        uint32_t n = set->count;
        for (uint32_t i = 0; i < n; i++) {
            ReachCtx rc = {.set = set, .changed = &changed};
            detector_visit_children(set->nodes[i].obj, detector_reach_visitor, &rc);
        }
    }
    if (set->oom) {
        fprintf(stderr, "[cycle-detector] out of memory building the reachable set; "
                        "results incomplete\n");
        report->traversal_failed = true;
        xr_free(set->nodes);
        return false;
    }

    /* Pass B: trial-decrement internal edges. */
    for (uint32_t i = 0; i < set->count; i++)
        detector_visit_children(set->nodes[i].obj, detector_dec_visitor, set);

    /* Pass C: anything with a surviving external reference is live, and so is
     * everything it reaches. */
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->nodes[i].color == DET_COLOR_GRAY && set->nodes[i].trial_rc >= 0)
            set->nodes[i].color = DET_COLOR_BLACK;
    }
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (uint32_t i = 0; i < set->count; i++) {
            if (set->nodes[i].color != DET_COLOR_BLACK)
                continue;
            ScanBlackCtx sb = {.set = set, .progressed = &progressed};
            detector_visit_children(set->nodes[i].obj, detector_scan_black_visitor, &sb);
        }
    }

    /* Whatever is still GRAY is unreachable from outside: a dead cycle. */
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->nodes[i].color == DET_COLOR_GRAY)
            set->nodes[i].color = DET_COLOR_WHITE;
    }

    /* Pass D: report, grouping the white set into individual cycles. */
    EmitCtx emit = {.set = set, .report = report, .header_printed = false};
    bool *grouped = (bool *) xr_calloc(set->count ? set->count : 1, sizeof(bool));
    DetectorNode **members =
        (DetectorNode **) xr_malloc((set->count ? set->count : 1) * sizeof(DetectorNode *));
    if (grouped && members) {
        for (uint32_t i = 0; i < set->count; i++) {
            if (set->nodes[i].color != DET_COLOR_WHITE || grouped[i])
                continue;
            /* Collect this cycle's members: everything white reachable from
             * here. A conservative grouping — two cycles sharing an object are
             * reported as one — which reads better than splitting them. */
            uint32_t count = 0;
            uint32_t queue_head = 0;
            members[count++] = &set->nodes[i];
            grouped[i] = true;
            while (queue_head < count) {
                DetectorNode *cur = members[queue_head++];
                for (uint32_t k = 0; k < set->count; k++) {
                    if (grouped[k] || set->nodes[k].color != DET_COLOR_WHITE)
                        continue;
                    /* Edge test: does cur point at nodes[k]? */
                    EdgeLabelCtx probe = {
                        .target = set->nodes[k].obj, .label = NULL, .slot = XR_OBJ_GRAPH_SLOT_NONE};
                    detector_visit_children(cur->obj, detector_edge_label_visitor, &probe);
                    if (probe.label) {
                        members[count++] = &set->nodes[k];
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
    for (uint32_t i = 0; i < set->count; i++)
        detector_visit_children(set->nodes[i].obj, detector_inc_visitor, set);

    xr_free(set->nodes);

    if (report->cycle_count > 0) {
        fprintf(stderr, "\n[cycle-detector] %s%u reference cycles, %u objects, %llu unreclaimed bytes\n",
                g_scan_shared_domain ? "shared domain " : "", report->cycle_count, report->object_count,
                (unsigned long long) report->byte_count);
    }
    xr_cycle_detector_accumulate(report);
    return report->cycle_count > 0;
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
    return detector_run_passes(&set, report);
}

/* ========== Shared-domain registry and scan ==========
 *
 * The system heap has no walkable block structure, so the allocator registers
 * every live shared object here. Registration is mutex-protected: shared
 * objects are released from whichever worker drops the last reference. The
 * scan itself runs at main-execution exit, when workers are quiescent. */

static struct {
    XrObjHeader **slots;
    uint32_t count;
    uint32_t cap;
    xr_mutex_t mu;
    bool mu_init;
} g_shared_registry;

static void shared_registry_lock(void) {
    if (!g_shared_registry.mu_init) {
        /* First registration happens on the main thread during isolate
         * bring-up, before any worker allocates shared objects. */
        xr_mutex_init(&g_shared_registry.mu);
        g_shared_registry.mu_init = true;
    }
    xr_mutex_lock(&g_shared_registry.mu);
}

void xr_cycle_detector_shared_register(XrObjHeader *obj) {
    if (!obj)
        return;
    shared_registry_lock();
    if (g_shared_registry.count == g_shared_registry.cap) {
        uint32_t cap = g_shared_registry.cap ? g_shared_registry.cap * 2 : 64;
        XrObjHeader **grown =
            (XrObjHeader **) xr_realloc(g_shared_registry.slots, cap * sizeof(*grown));
        if (!grown) {
            /* Best effort: an unregistered object degrades coverage, never
             * correctness — a cycle through it goes unreported. */
            xr_mutex_unlock(&g_shared_registry.mu);
            return;
        }
        g_shared_registry.slots = grown;
        g_shared_registry.cap = cap;
    }
    g_shared_registry.slots[g_shared_registry.count++] = obj;
    xr_mutex_unlock(&g_shared_registry.mu);
}

void xr_cycle_detector_shared_unregister(XrObjHeader *obj) {
    if (!obj || !g_shared_registry.mu_init)
        return;
    shared_registry_lock();
    for (uint32_t i = 0; i < g_shared_registry.count; i++) {
        if (g_shared_registry.slots[i] == obj) {
            g_shared_registry.slots[i] = g_shared_registry.slots[--g_shared_registry.count];
            break;
        }
    }
    xr_mutex_unlock(&g_shared_registry.mu);
}

bool xr_cycle_detector_scan_shared(XrCycleReport *out) {
    XrCycleReport local = {0};
    XrCycleReport *report = out ? out : &local;
    memset(report, 0, sizeof(*report));

    DetectorSet set = {0};
    g_scan_shared_domain = true;

    /* Pass A: every live registered shared object is a seed. The registry is
     * complete for the band (every alloc_shared registers), so the reach pass
     * inside detector_run_passes only re-confirms membership. */
    shared_registry_lock();
    for (uint32_t i = 0; i < g_shared_registry.count && !set.oom; i++) {
        XrObjHeader *obj = g_shared_registry.slots[i];
        if (obj && !(obj->extra & XR_OBJ_DEAD))
            (void) detector_intern(&set, obj);
    }
    xr_mutex_unlock(&g_shared_registry.mu);

    bool found = detector_run_passes(&set, report);
    g_scan_shared_domain = false;
    return found;
}

#endif /* XR_ENABLE_CYCLE_DETECTOR */
