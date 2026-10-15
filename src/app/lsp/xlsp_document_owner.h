/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_document_owner.h - Document table and temporary document destruction
 */
#ifndef XLSP_DOCUMENT_OWNER_H
#define XLSP_DOCUMENT_OWNER_H
#include "xlsp_server.h"
// The server and its diagnostic queue remain alive through document teardown.
XR_FUNC void xlsp_document_destroy(XrLspDocument *);
#endif // XLSP_DOCUMENT_OWNER_H
