/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_settings.h - Validated scalar configuration notifications
 *
 * KEY CONCEPT:
 *   An independent scalar patch owns no JSON or configuration pointers.
 *   Validation and application share one explicit cumulative work ledger.
 */
#ifndef XLSP_SOURCE_SETTINGS_H
#define XLSP_SOURCE_SETTINGS_H
#include "xlsp_server.h"
#include "../../xir/xxir.h"

typedef struct XlspSourceSettings XlspSourceSettings;
/* Caller excludes DOM mutation. Failure preserves output. Missing xray settings
 * return UNRESOLVED. Unknown fields are ignored; known duplicates/types/ranges
 * are rejected. An empty output receives an owner that pins the original ledger
 * after the DOM and external ledger lease die. No JSON pointers are retained. */
XR_FUNC XrXirStatus xlsp_source_settings_read(XrCompileResources *, const XrJsonValue *, XlspSourceSettings **);
XR_FUNC void xlsp_source_settings_free(XlspSourceSettings *);
/* Caller excludes server mutation. Failure preserves config, pending
 * queue/documents and clear_diagnostics.
 * Success updates only present fields and clears the pending queue when enabled
 * diagnostics become disabled. The caller then emits empty diagnostic messages;
 * transport/JSON publication is a separate operation after the state commit. */
XR_FUNC XrXirStatus xlsp_source_settings_apply(const XlspSourceSettings *, XrLspServer *, bool *clear_diagnostics);
#endif // XLSP_SOURCE_SETTINGS_H
