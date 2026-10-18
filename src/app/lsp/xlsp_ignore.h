/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_ignore.h - Shared bounded workspace ignore matching
 *
 * KEY CONCEPT:
 *   Editor configuration and resource-bounded scans share one name policy.
 */
#ifndef XLSP_IGNORE_H
#define XLSP_IGNORE_H
#include "xlsp_server.h"
XR_FUNC XrXirStatus xlsp_source_ignore(XrCompileResources *,const XlspConfig *,const char *,bool,bool *);
#endif // XLSP_IGNORE_H
