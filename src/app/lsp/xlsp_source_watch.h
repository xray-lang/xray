/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_watch.h - Owned validated filesystem notifications
 *
 * KEY CONCEPT:
 *   Decode the complete ordered event batch before invalidating state. Exact
 *   client URIs and canonical index URIs have distinct owned representations.
 */
#ifndef XLSP_SOURCE_WATCH_H
#define XLSP_SOURCE_WATCH_H
#include "xlsp_source_query.h"
#include "xlsp_server.h"
#include "../../base/xjson.h"
typedef struct XlspSourceWatch XlspSourceWatch;
typedef struct XlspSourceWatchEvent {
    const char *uri, *path, *index_uri;
    uint32_t kind;
} XlspSourceWatchEvent;
/* Failure preserves empty output. Keys/types/URI bytes are validated before
 * publication. The original ledger covers decoding, copies and later revision
 * admission; producer JSON and the external ledger lease may die first. */
XR_FUNC XrXirStatus xlsp_source_watch_read(XrCompileResources *, const XrJsonValue *, XlspSourceWatch **);
XR_FUNC const XlspSourceWatchEvent *xlsp_source_watch_items(const XlspSourceWatch *, size_t *count);
/* Caller excludes server mutation. Failure preserves revision and the cached
 * semantic workspace. Success advances by the full batch length and drops the
 * old semantic workspace for a nonempty batch, before index/diagnostic effects. */
XR_FUNC XrXirStatus xlsp_source_watch_commit(const XlspSourceWatch *, XrLspServer *);
XR_FUNC void xlsp_source_watch_free(XlspSourceWatch *);
#endif // XLSP_SOURCE_WATCH_H
