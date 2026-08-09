/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_gen_iter.c - AOT generator iterator pull helpers
 */

#include "xrt_coll.h"
#include "xrt_class.h"
#include "xrt_exception.h"
#include "../coro/xaot_coro.h"

static void xrt_gen_iter_finish(xrt_iterator_t *it) {
    if (!it || it->cursor == 2)
        return;
    it->cursor = 2;
    if (it->gen) {
        xr_coro_destroy(it->gen);
        it->gen = NULL;
    }
}

int xrt_gen_iter_has_next(xrt_iterator_t *it) {
    if (!it || !it->gen)
        return 0;
    if (it->cursor == 2)
        return 0;
    if (it->cursor == 1)
        return 1;

    XrValue out = XR_NULL_VAL;
    bool error_is_value = false;
    XrAotGenDriveKind kind = xr_aot_gen_drive(it->gen, &out, &error_is_value);
    if (kind == XR_AOT_GEN_DRIVE_YIELD) {
        it->coll = out;
        it->cursor = 1;
        return 1;
    }
    xrt_gen_iter_finish(it);
    if (kind == XR_AOT_GEN_DRIVE_ERROR && !XR_IS_NULL(out)) {
        if (error_is_value)
            xrt_pending_error = out;
        else
            xrt_throw_exc(out);
    }
    return 0;
}

XrValue xrt_gen_iter_next(xrt_iterator_t *it) {
    /* The same test hasNext() performs: it drives the producer one step when
     * nothing is buffered, so an in-contract next() behaves exactly as the
     * unguarded pull did, and surfaces a producer failure as that failure. */
    if (it && xrt_gen_iter_has_next(it)) {
        XrValue yielded = it->coll;
        it->coll = XR_NULL_VAL;
        it->cursor = 0;
        return yielded;
    }
    /* A producer that ended cleanly leaves nothing to hand back, and next() is
     * typed T: pulling past the end is a protocol violation (LANGUAGE_SPEC
     * 5.3.6), not a zero value. A producer that failed already reported it. */
    if (!xrt_has_pending_error())
        xrt_throw_error(XR_ERR_ITERATOR_EXHAUSTED, XR_ERROR_CORE_ITERATOR_EXHAUSTED_NEXT_MSG);
    return XR_NULL_VAL;
}

void xrt_gen_iter_destroy(xrt_iterator_t *it) {
    if (!it)
        return;
    XrValue buffered = it->coll;
    it->coll = XR_NULL_VAL;
    xrt_release(buffered);
    xrt_gen_iter_finish(it);
}

XrValue xr_aot_gen_iterator_new(const XrAotContext *ctx, const XrAotCoroDesc *desc, void *frame) {
    if (!ctx || !ctx->runtime || !desc || !desc->resume || !frame)
        return XR_NULL_VAL;

    const char *name = desc->name ? desc->name : "generator";
    XrCoroutine *gen = xr_coro_create_aot(ctx->runtime, desc, frame, name);
    if (!gen)
        return XR_NULL_VAL;

    xrt_iterator_t *it = (xrt_iterator_t *) xrt_arc_alloc(sizeof(xrt_iterator_t));
    xrt_arc_mark_builtin(it, XRT_ARC_KIND_ITERATOR);
    it->coll = XR_NULL_VAL;
    it->cursor = 0;
    it->index = 0;
    it->kind = XRT_ITER_GENERATOR;
    it->gen = gen;
    return xr_mkptr(it, XR_TAG_ITERATOR);
}
