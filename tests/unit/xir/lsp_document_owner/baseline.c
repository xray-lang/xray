/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "alloc.h"
#include "app/lsp/xlsp_document_owner.h"
#include "app/lsp/xlsp_source_open.h"
#include "app/lsp/xlsp_cache.h"
#include "app/lsp/xlsp_imports.h"
#include "app/lsp/xlsp_protocol_output.h"
#include "baseline_destroy.inc.c"
XR_FUNC void baseline_destroy(XrLspDocument *doc) { baseline_document_destroy(doc); }
#include "baseline_clear.inc.c"
