/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_SEMANTIC_TOKENS_H
#define XLSP_SEMANTIC_TOKENS_H
#include "../../base/xjson.h"
#include "xlsp_semantic_legend.h"

/* Complete unchanged protocol legend and full/delta/range advertisement.
 * NULL rejects construction; no partial capability object escapes. */
XR_FUNC XrJsonValue *xlsp_semantic_tokens_provider(void);

#endif // XLSP_SEMANTIC_TOKENS_H
