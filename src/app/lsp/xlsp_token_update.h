/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_token_update.h - Owned semantic cache proposal for terminal publication
 */
#ifndef XLSP_TOKEN_UPDATE_H
#define XLSP_TOKEN_UPDATE_H
#include "xlsp_server.h"
typedef struct XlspTokenUpdate XlspTokenUpdate;
typedef struct XlspTokenUpdateInput {
    XrCompileResources *resources;
    const char *uri;
    size_t uri_length;
    int version;
    uint32_t previous_result_id;
    XlspSourceTokens **tokens;
} XlspTokenUpdateInput;
/* Failure preserves the caller's tokens and output. Success adopts tokens.
 * A proposal owns its URI and token stream after every producer dies.
 * Match and commit run exclusively on the document dispatch thread; commit
 * cannot allocate. The caller publishes successfully before committing. */
XR_FUNC XrXirStatus xlsp_token_update_new(const XlspTokenUpdateInput *, XlspTokenUpdate **);
XR_FUNC bool xlsp_token_update_matches(XrLspServer *, const XlspTokenUpdate *);
XR_FUNC bool xlsp_token_update_commit(XrLspServer *, XlspTokenUpdate *);
XR_FUNC void xlsp_token_update_free(XlspTokenUpdate *);
#endif // XLSP_TOKEN_UPDATE_H
