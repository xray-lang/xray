/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_source_instance.h - Persistent execution ownership and real function calls
 */
#ifndef XRAY_SOURCE_INSTANCE_H
#define XRAY_SOURCE_INSTANCE_H
#include "xray_source_entry.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct XrSourceInstance XrSourceInstance;
/* Serialized owner of one real Instance. Initialization, module slots, callable
 * authority and all runtime/output limits persist across calls. Options and
 * callbacks are fixed here; callback context must live until drop. Artifact and
 * Execution handles may die after success. Output must be NULL and is preserved on error. */
XRAY_API XrSourceStatus xray_source_instance_new(XrSourceExecution *, XrSourceArtifact *,
    const XrSourceCallOptions *, XrSourceInstance **);
/* Exactly one active Call is allowed. It uses the ordinary poll/wait/resume/
 * cancel/take/drop APIs. Dropping it cancels and drains its actual task group,
 * preserving initialized module state. Initialization failure stays sticky;
 * cleanup failure stops the owner. An active Call makes begin/drop return BUSY. */
XRAY_API XrSourceStatus xray_source_instance_begin_main(XrSourceInstance *, XrSourceCall **);
XRAY_API XrSourceStatus xray_source_instance_begin(XrSourceInstance *,
    const XrSourceInvocation *, XrSourceCall **);
XRAY_API XrSourceStatus xray_source_instance_execute(XrSourceInstance *,
    const XrSourceInvocation *, XrSourceResult **, XrSourceEvent *failure);
/* Requires an actual function binding produced by this owner. Copying a Value,
 * knowing an entry name or using a different owner confers no authority.
 * Arguments borrow until begin returns, then may be freed. */
XRAY_API XrSourceStatus xray_source_instance_begin_function(XrSourceInstance *,
    const XrSourceValue *, const XrSourceValue *const *, size_t, XrSourceCall **);
/* Consumes and clears *owner except on BUSY. Reports actual cleanup in event,
 * including failure. A NULL *owner is a successful no-op. */
XRAY_API XrSourceStatus xray_source_instance_drop(XrSourceInstance **, XrSourceEvent *cleanup);
#ifdef __cplusplus
}
#endif
#endif // XRAY_SOURCE_INSTANCE_H
