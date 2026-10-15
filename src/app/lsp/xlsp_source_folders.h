/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_folders.h - Owned workspace-folder transactions
 */
#ifndef XLSP_SOURCE_FOLDERS_H
#define XLSP_SOURCE_FOLDERS_H
#include "xlsp_server.h"
#include "../../base/xjson.h"

typedef struct XlspSourceFolders XlspSourceFolders;
/* Capture the original folder state and validate the entire notification.
 * Failure preserves empty output; the owner retains the original ledger. */
XR_FUNC XrXirStatus xlsp_source_folders_read(XrCompileResources *, const XrLspServer *,
    const XrJsonValue *, XlspSourceFolders **);
XR_FUNC XrXirStatus xlsp_source_folders_single(XrCompileResources *, const XrLspServer *,
    const char *uri, const char *name, bool add, XlspSourceFolders **);
/* The event thread excludes concurrent server mutation. Stale state, resource
 * failure and receiving-heap failure preserve all server fields and owners.
 * A changed set advances revision once, joins the retired pool and discards
 * derived syntax/semantic indexes. Caller then restarts retained roots and
 * updates external progress/export notifications. No-op sets keep all state. */
XR_FUNC XrXirStatus xlsp_source_folders_commit(const XlspSourceFolders *, XrLspServer *, bool *changed);
XR_FUNC void xlsp_source_folders_free(XlspSourceFolders *);
#endif // XLSP_SOURCE_FOLDERS_H
