/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * escape.c - A real owner retained beyond the record producer's call
 *
 * KEY CONCEPT:
 *   Separate compilation exposes actual escape without handicapping inline PODs.
 */
#include "layout.h"

static XrMvlPairObject *mvl_escaped_pair;

XR_FUNCDEF bool xr_mvl_pair_escape(XrMvlPairObject *value) {
    if (!xr_mvl_retain(&value->header))
        return false;
    XrMvlPairObject *old = mvl_escaped_pair;
    mvl_escaped_pair = value;
    if (old)
        xr_mvl_drop(&old->header);
    return true;
}

XR_FUNCDEF XrMvlPairObject *xr_mvl_pair_escaped(void) {
    return mvl_escaped_pair;
}

XR_FUNCDEF void xr_mvl_pair_escape_clear(void) {
    if (mvl_escaped_pair)
        xr_mvl_drop(&mvl_escaped_pair->header);
    mvl_escaped_pair = NULL;
}
