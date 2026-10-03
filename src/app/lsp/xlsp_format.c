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

    if (!doc->server) return edits;
    XrCompileResourceLimits limits = {UINT64_C(1073741824),UINT64_C(268435456),UINT64_C(8589934592)};
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    AstNode *ast = NULL;
    XrFmtOutput output = {0};
    unsigned stage = 0, failure = 0;
    XrCompileResourceStatus created = xr_compile_resources_new(&limits, &resources);
    if (created != XR_COMPILE_RESOURCE_OK) { stage=1; failure=(unsigned)created; goto cleanup; }
    XrCompilerSessionStatus opened = xr_compile_session_new(resources, &session);
    if (opened != XR_COMPILER_SESSION_OK) { stage=2; failure=(unsigned)opened; goto cleanup; }
    XrParseStatus parsed = xr_compile_parse_with_trivia(session, doc->content, doc->uri, NULL, &ast);
    if (parsed != XR_PARSE_OK) {
        if (parsed != XR_PARSE_SYNTAX) { stage=3; failure=(unsigned)parsed; }
        goto cleanup;
    }

    // Format AST using server-configured tab size / spaces
    XrFmtConfig config = xfmt_default_config;
    if (doc->server) {
        XlspConfig *sc = &doc->server->config;
        config.indent_size = sc->format_tab_size;
        config.max_line_length = sc->format_max_line_length;
        config.use_tabs = sc->format_insert_spaces ? 0 : 1;
        config.align_branch_arrows = sc->format_align_branch_arrows ? 1 : 0;
        config.align_enum_values = sc->format_align_enum_values ? 1 : 0;
        config.align_struct_fields = sc->format_align_struct_fields ? 1 : 0;
        config.align_trailing_comments = sc->format_align_trailing_comments ? 1 : 0;
        config.wrap_long_lines = sc->format_wrap_long_lines ? 1 : 0;
        config.multiline_trailing_comma = sc->format_multiline_trailing_comma ? 1 : 0;
    }
    XrFmtStatus formatted = xr_compile_format_ast(xr_compile_session_compile_state(session), ast, &config, &output);
    if (formatted != XR_FMT_OK) { stage=4; failure=(unsigned)formatted; goto cleanup; }

    // Create single edit that replaces entire document
    XrJsonValue *edit = xjson_new_object();
    xjson_object_set(edit, "range", xjson_make_range(0, 0, doc->line_count, 0));
    xjson_object_set(edit, "newText", xjson_new_string(output.text));

    xjson_array_push(edits, edit);

cleanup:
    xr_program_destroy(ast);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    xr_compile_format_output_free(&output);
    if (stage) {
        doc->server->formatting_failure.stage = stage;
        doc->server->formatting_failure.status = failure;
        xjson_free(edits);
        return NULL;
    }
    return edits;
}
