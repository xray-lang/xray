/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_format.c - Document formatting for LSP
 */

#include "xlsp_format.h"
#include "xlsp_server.h"
#include "xlsp_json.h"
#include "xlsp_utils.h"
#include "../../frontend/format/xfmt.h"
#include "../../frontend/parser/xparse.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>

// ============================================================================
// Formatting
// ============================================================================

// AST-based formatting with comment preservation
XrJsonValue *xlsp_analyze_format(XrLspDocument *doc) {
    XrJsonValue *edits = xlsp_json_new_array();

    if (!doc || !doc->content || doc->length == 0) {
        return edits;
    }

    // Get isolate from server
    XrayIsolate *X = doc->server ? doc->server->isolate : NULL;
    if (!X) {
        return edits;
    }

    // Parse with trivia collection (preserves comments)
    AstNode *ast = xr_parse_with_trivia(X, doc->content, doc->uri);
    if (!ast) {
        // Parse failed, return empty edits
        return edits;
    }

    // Format AST using server-configured tab size / spaces
    XrFmtConfig config = xfmt_default_config;
    if (doc->server) {
        config.indent_size = doc->server->config.format_tab_size;
        config.use_tabs = doc->server->config.format_insert_spaces ? 0 : 1;
    }
    char *formatted = xfmt_format_ast(ast, &config, X);

    // Free AST
    xr_program_destroy(ast);

    if (!formatted) {
        return edits;
    }

    // Create single edit that replaces entire document
    XrJsonValue *edit = xlsp_json_new_object();
    xlsp_json_object_set(edit, "range",
        xlsp_json_make_range(0, 0, doc->line_count, 0));
    xlsp_json_object_set(edit, "newText", xlsp_json_new_string(formatted));

    xlsp_json_array_push(edits, edit);

    xr_free(formatted);
    return edits;
}

