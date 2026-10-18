/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_diagnostic_owner.h - Exact publication ownership across editor states
 */
#ifndef XLSP_DIAGNOSTIC_OWNER_H
#define XLSP_DIAGNOSTIC_OWNER_H
#include "xlsp_source_open.h"
#include "xlsp_server.h"
/* Event-thread operations. The borrowed clear object lives until owner rebind,
 * publication commit or free. Rebind and commit allocate and charge no work. */
XR_FUNC const XrJsonValue *xlsp_source_open_published_clear(const XlspSourceOpen *);
XR_FUNC uint64_t xlsp_source_open_diagnostic_revision(const XlspSourceOpen *);
XR_FUNC XrXirStatus xlsp_source_open_diagnostic_work(const XlspSourceOpen *,uint64_t);
XR_FUNC void xlsp_source_open_rebind_publication(XlspSourceOpen *,XlspSourceOpen *,uint64_t);
XR_FUNC void xlsp_source_open_publication_commit(XlspSourceOpen *,XrJsonValue **);
XR_FUNC XrXirStatus xlsp_diagnostic_document_close(XrLspServer *,XrLspDocument *);
#endif // XLSP_DIAGNOSTIC_OWNER_H
