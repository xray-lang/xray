/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_index_pool.h - Bounded parallel Source syntax indexing
 */
#ifndef XLSP_INDEX_POOL_H
#define XLSP_INDEX_POOL_H
#include "xlsp_source_index.h"
#include <stdatomic.h>
typedef struct XrLspIndexPool XrLspIndexPool;
typedef struct XlspIndexPoolOptions {
    XrCompileResources *resources;
    size_t maximum_file_bytes;
    unsigned workers;
} XlspIndexPoolOptions;
typedef struct XrLspIndexResult {
    char *path,*uri;
    XlspSourceIndex *index;
    XrXirStatus status;
    uint64_t revision;
    _Atomic bool cancel_requested;
    struct XrLspIndexResult *next;
} XrLspIndexResult;
/* The original ledger covers queue/capture/I/O/syntax/result owners. Native
 * thread starters/stacks use the existing OS provider's separate domain.
 * At most64 workers, joined before release. Submit borrows synchronously;
 * poll transfers owners. Cancellation checks operation boundaries, not inner
 * Source parser work. Public operations require caller exclusion with free. */
XR_FUNC XrXirStatus xlsp_index_pool_new(const XlspIndexPoolOptions *,XrLspIndexPool **);
XR_FUNC void xlsp_index_pool_free(XrLspIndexPool *);
XR_FUNC XrXirStatus xlsp_index_pool_submit(XrLspIndexPool *,const char *,const char *,uint64_t);
/* One batch publishes every captured node or none. */
XR_FUNC XrXirStatus xlsp_index_pool_submit_batch(XrLspIndexPool *,char *const *,size_t,uint64_t);
XR_FUNC void xlsp_index_pool_cancel_all(XrLspIndexPool *);
/* Exact decoded path spelling, with only Windows separator normalization.
 * Mark every queued/running/completed match without fresh resource admission. */
XR_FUNC void xlsp_index_pool_cancel_file(XrLspIndexPool *,const char *);
XR_FUNC XrLspIndexResult *xlsp_index_pool_poll(XrLspIndexPool *);
XR_FUNC bool xlsp_index_pool_is_idle(XrLspIndexPool *);
XR_FUNC void xlsp_index_pool_get_progress(XrLspIndexPool *,uint64_t *,uint64_t *);
XR_FUNC void xlsp_index_result_free(XrLspIndexResult *);
XR_FUNC void xlsp_index_result_free_list(XrLspIndexResult *);
#endif // XLSP_INDEX_POOL_H
