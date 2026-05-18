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
#include "../../base/xjson.h"
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
    XrJsonValue *edits = xjson_new_array();

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
        XlspConfig *sc = &doc->server->config;
        config.indent_size = sc->format_tab_size;
        config.max_line_length = sc->format_max_line_length;
        config.use_tabs = sc->format_insert_spaces ? 0 : 1;
        config.align_match_arms = sc->format_align_match_arms ? 1 : 0;
        config.align_enum_values = sc->format_align_enum_values ? 1 : 0;
        config.align_struct_fields = sc->format_align_struct_fields ? 1 : 0;
        config.align_trailing_comments = sc->format_align_trailing_comments ? 1 : 0;
        config.wrap_long_lines = sc->format_wrap_long_lines ? 1 : 0;
        config.multiline_trailing_comma = sc->format_multiline_trailing_comma ? 1 : 0;
    }
    char *formatted = xfmt_format_ast(ast, &config, X);

    // Free AST
    xr_program_destroy(ast);

    if (!formatted) {
        return edits;
    }

    // Create single edit that replaces entire document
    XrJsonValue *edit = xjson_new_object();
    xjson_object_set(edit, "range", xjson_make_range(0, 0, doc->line_count, 0));
    xjson_object_set(edit, "newText", xjson_new_string(formatted));

    xjson_array_push(edits, edit);

    xr_free(formatted);
    return edits;
}
