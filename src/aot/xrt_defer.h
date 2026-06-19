/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_defer.h - AOT deferred-cleanup runtime
 *
 * KEY CONCEPT:
 *   `defer` is function-scoped LIFO cleanup that must run on every exit:
 *   normal return, value-return error propagation, and panic unwind. The IR
 *   lowers each `defer` into a zero-argument closure that captures its operands
 *   eagerly (see the parser's defer desugar), so at runtime a pending defer is
 *   simply a closure value to invoke at scope exit.
 *
 *   Each generated function with defers owns a stack-local XrtDeferScope and
 *   links it onto the global defer chain (xrt_defer_top) at entry. Registering a
 *   defer pushes the closure onto that scope. Every exit pops the scope off the
 *   chain and runs it LIFO. A panic (longjmp) unwinds the chain down to the
 *   catching try's recorded mark BEFORE jumping (see xrt_throw_exc), so defers
 *   in skipped frames still run — mirroring the VM's per-frame defer marks.
 *
 *   The chain is a plain global, matching xrt_exc_top / xrt_pending_error: the
 *   AOT runtime's non-local control flow is single-threaded today and all three
 *   move to _Thread_local together when concurrency lands.
 */

#ifndef XRT_DEFER_H
#define XRT_DEFER_H

#include "xrt_value.h"     /* XrValue, XR_NULL_VAL */
#include "xrt_arc.h"       /* XRT_MALLOC/REALLOC/FREE, xrt_release */
#include "xrt_coll.h"      /* xrt_closure_t */
#include "xrt_exception.h" /* xrt_pending_error, xrt_has_pending_error */

/* Most functions register only a handful of defers; spill to the heap only when
 * a loop accumulates more than this many pending closures. */
#define XRT_DEFER_INLINE_CAP 4

typedef struct XrtDeferScope {
    XrValue *items;             /* -> inline_buf, or heap when spilled */
    int count;                  /* pending closures */
    int cap;                    /* capacity of items */
    struct XrtDeferScope *prev; /* enclosing scope on the global chain */
    XrValue inline_buf[XRT_DEFER_INLINE_CAP];
} XrtDeferScope;

#ifdef XRT_IMPL
XrtDeferScope *xrt_defer_top = NULL;
#else
extern XrtDeferScope *xrt_defer_top;
#endif

/* Initialize a scope WITHOUT linking it onto the global chain. Used by AOT
 * coroutine frames, whose defer scope lives in the heap frame (persisting across
 * suspensions) and is run directly at the coroutine's exit/release points rather
 * than via the C-stack unwind chain. */
static inline void xrt_defer_init(XrtDeferScope *s) {
    s->items = s->inline_buf;
    s->count = 0;
    s->cap = XRT_DEFER_INLINE_CAP;
    s->prev = NULL;
}

/* Initialize and push a scope onto the chain (sync function entry). */
static inline void xrt_defer_enter(XrtDeferScope *s) {
    xrt_defer_init(s);
    s->prev = xrt_defer_top;
    xrt_defer_top = s;
}

/* Register one deferred closure (eager-captured, zero-arg). The defer consumes
 * the closure (XI_DEFER ownership = CONSUME), so the scope now owns this
 * reference and releases it after invocation. */
static inline void xrt_defer_push(XrtDeferScope *s, XrValue closure) {
    if (XR_UNLIKELY(s->count >= s->cap)) {
        int ncap = s->cap * 2;
        XrValue *grown;
        if (s->items == s->inline_buf) {
            grown = (XrValue *) XRT_MALLOC((size_t) ncap * sizeof(XrValue));
            if (XR_UNLIKELY(!grown)) {
                fprintf(stderr, "xrt_defer_push: out of memory\n");
                abort();
            }
            memcpy(grown, s->items, (size_t) s->count * sizeof(XrValue));
        } else {
            grown = (XrValue *) XRT_REALLOC(s->items, (size_t) ncap * sizeof(XrValue));
            if (XR_UNLIKELY(!grown)) {
                fprintf(stderr, "xrt_defer_push: out of memory\n");
                abort();
            }
        }
        s->items = grown;
        s->cap = ncap;
    }
    s->items[s->count++] = closure;
}

/* Invoke one pending defer closure with Go-style error semantics: an error
 * thrown inside the defer replaces any in-flight value-return error; otherwise
 * the in-flight error is preserved across the call.
 *
 * The closure itself is not released here: AOT heap objects currently leak to
 * process exit (see known_bugs "AOT 集合现状 leak", task 108), and a defer
 * closure can capture upvalues that were borrowed (not retained) at capture, so
 * running its destructor would over-release them. Deterministic reclamation of
 * defer closures comes with the broader AOT memory model work (108). */
static inline void xrt_defer_invoke_one(XrValue closure) {
    XrValue saved_error = xrt_pending_error;
    int had_error = xrt_has_pending_error();
    if (had_error)
        xrt_pending_error = XR_NULL_VAL;

    xrt_closure_t *c = (xrt_closure_t *) closure.ptr;
    if (c)
        ((XrValue (*)(xrt_closure_t *)) c->fn)(c);

    if (had_error) {
        if (xrt_has_pending_error())
            xrt_release(saved_error); /* defer threw: its error wins */
        else
            xrt_pending_error = saved_error; /* restore the in-flight error */
    }
}

/* Run a scope's pending defers LIFO and reset it (release any heap spill). Does
 * NOT unlink from the chain; callers handle chain maintenance. */
static inline void xrt_defer_run(XrtDeferScope *s) {
    for (int i = s->count - 1; i >= 0; i--)
        xrt_defer_invoke_one(s->items[i]);
    s->count = 0;
    if (s->items != s->inline_buf) {
        XRT_FREE(s->items);
        s->items = s->inline_buf;
        s->cap = XRT_DEFER_INLINE_CAP;
    }
}

/* Normal / value-error exit: unlink this function's scope and run it. */
static inline void xrt_defer_leave(XrtDeferScope *s) {
    xrt_defer_top = s->prev;
    xrt_defer_run(s);
}

/* Panic unwind: run and unlink every scope above `mark` (a XrtDeferScope* or
 * NULL for "run everything"), so defers in frames skipped by longjmp still
 * execute. Called by xrt_throw_exc before transferring control. */
static inline void xrt_defer_unwind_to(void *mark) {
    while (xrt_defer_top && xrt_defer_top != (XrtDeferScope *) mark) {
        XrtDeferScope *s = xrt_defer_top;
        xrt_defer_top = s->prev;
        xrt_defer_run(s);
    }
}

#endif  // XRT_DEFER_H
