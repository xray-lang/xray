/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_handlers_textdoc.h - LSP text document handler declarations
 */

#ifndef XLSP_HANDLERS_TEXTDOC_H
#define XLSP_HANDLERS_TEXTDOC_H

#include "../../base/xjson.h"

typedef struct XrLspServer XrLspServer;

// Document sync handlers
XR_FUNC void xlsp_handle_td_did_open(XrLspServer *server, XrJsonValue *params);
XR_FUNC void xlsp_handle_td_did_change(XrLspServer *server, XrJsonValue *params);
XR_FUNC void xlsp_handle_td_did_close(XrLspServer *server, XrJsonValue *params);

// Feature handlers
XR_FUNC XrJsonValue *xlsp_handle_td_completion(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_completion_resolve(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_hover(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_document_symbol(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_definition(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_references(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_rename(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_prepare_rename(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_formatting(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_on_type_formatting(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_code_lens(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_signature_help(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_semantic_tokens_full(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_semantic_tokens_delta(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_semantic_tokens_range(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_td_inlay_hint(XrLspServer *server, XrJsonValue *params);

#endif // XLSP_HANDLERS_TEXTDOC_H
