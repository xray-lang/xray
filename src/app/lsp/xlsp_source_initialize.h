/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_initialize.h - Owned initialize-state admission
 */
#ifndef XLSP_SOURCE_INITIALIZE_H
#define XLSP_SOURCE_INITIALIZE_H
#include "xlsp_source_folders.h"
typedef struct XlspSourceInitialize XlspSourceInitialize;
/* Complete root selection, folder ownership and response construction before
 * publishing state. The original ledger and JSON result survive input death. */
XR_FUNC XrXirStatus xlsp_source_initialize_prepare(XrCompileResources *, const XrLspServer *,
    const XrJsonValue *, XlspSourceInitialize **);
/* Caller excludes server mutation and keeps the server alive. Any failure
 * preserves empty result and server fields. Success transfers a complete JSON
 * response and publishes initialized together with the folder transaction. */
XR_FUNC XrXirStatus xlsp_source_initialize_commit(XlspSourceInitialize *, XrLspServer *, XrJsonValue **);
XR_FUNC void xlsp_source_initialize_free(XlspSourceInitialize *);
#endif // XLSP_SOURCE_INITIALIZE_H
