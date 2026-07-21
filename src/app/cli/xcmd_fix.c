/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_fix.c - Optional, analysis-backed source rewrites
 */

#include "xcli.h"
#include "xcli_fs.h"
#include "xcli_spec.h"
#include "../../api/xisolate_profile.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_ast_visitor.h"
#include "../../frontend/analyzer/xanalyzer_symbol.h"
#include "../../frontend/parser/xa_assertion_attr.h"
#include "../../frontend/parser/xast.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/parser/xparse.h"
#include "../../os/os_dir.h"
#include "../../os/os_fs.h"
#include "../../toolchain/xcompiler_session.h"
#include "xray_vm.h"

#include <stdio.h>
#include <string.h>

typedef struct NoThrowAnnotationScan {
    XaAnalyzer *analyzer;
    bool *lines;
    int line_count;
    int annotation_count;
} NoThrowAnnotationScan;

static XaSymbol *fix_function_symbol(NoThrowAnnotationScan *scan, AstNode *node) {
    if (!scan || !scan->analyzer || !node)
        return NULL;
    XaScope *scope = xa_scope_find_by_node(scan->analyzer->global_scope, node);
    if (scope && scope->function_symbol)
        return scope->function_symbol;
    if (node->type == AST_FUNCTION_DECL && node->as.function_decl.symbol_id)
        return xa_scope_lookup_by_id(scan->analyzer->global_scope,
                                     node->as.function_decl.symbol_id);
    return NULL;
}

static void collect_no_throw_annotation(AstNode *node, void *userdata) {
    NoThrowAnnotationScan *scan = (NoThrowAnnotationScan *) userdata;
    if (!scan || !node || (node->type != AST_FUNCTION_DECL && node->type != AST_METHOD_DECL))
        return;
    if (xa_decl_has_attribute(node, ATTR_NO_THROW))
        return;
    AstNode *body =
        node->type == AST_FUNCTION_DECL ? node->as.function_decl.body : node->as.method_decl.body;
    if (!body || (node->type == AST_METHOD_DECL && node->as.method_decl.is_constructor))
        return;
    XaSymbol *symbol = fix_function_symbol(scan, node);
    if (!symbol || symbol->links.throw_effect != XR_FN_EFFECT_NO_THROW)
        return;
    int line = node->line;
    if (line <= 0 || line > scan->line_count || scan->lines[line])
        return;
    scan->lines[line] = true;
    scan->annotation_count++;
}

static int source_line_count(const char *source) {
    int count = 1;
    if (!source)
        return count;
    for (const char *p = source; *p; p++) {
        if (*p == '\n')
            count++;
    }
    return count;
}

static char *insert_no_throw_annotations(const char *source, const bool *lines, int line_count,
                                         int annotation_count) {
    if (!source || !lines || annotation_count <= 0)
        return NULL;
    size_t source_length = strlen(source);
    size_t capacity = source_length + (size_t) annotation_count * 64 + 1;
    char *result = (char *) xr_malloc(capacity);
    if (!result)
        return NULL;
    size_t output = 0;
    int line = 1;
    const char *cursor = source;
    const char *newline = strstr(source, "\r\n") ? "\r\n" : "\n";
    size_t newline_length = strlen(newline);
    while (*cursor && line <= line_count) {
        const char *line_end = strchr(cursor, '\n');
        const char *content_end = line_end ? line_end : cursor + strlen(cursor);
        const char *indent_end = cursor;
        while (indent_end < content_end && (*indent_end == ' ' || *indent_end == '\t'))
            indent_end++;
        if (lines[line]) {
            size_t indent_length = (size_t) (indent_end - cursor);
            size_t required = output + indent_length + strlen("@no_throw") + newline_length + 1;
            if (required > capacity) {
                capacity = required * 2;
                char *grown = (char *) xr_realloc(result, capacity);
                if (!grown) {
                    xr_free(result);
                    return NULL;
                }
                result = grown;
            }
            memcpy(result + output, cursor, indent_length);
            output += indent_length;
            memcpy(result + output, "@no_throw", strlen("@no_throw"));
            output += strlen("@no_throw");
            memcpy(result + output, newline, newline_length);
            output += newline_length;
        }
        size_t line_length =
            line_end ? (size_t) (line_end + 1 - cursor) : (size_t) (content_end - cursor);
        if (output + line_length + 1 > capacity) {
            capacity = (output + line_length + 1) * 2;
            char *grown = (char *) xr_realloc(result, capacity);
            if (!grown) {
                xr_free(result);
                return NULL;
            }
            result = grown;
        }
        memcpy(result + output, cursor, line_length);
        output += line_length;
        cursor += line_length;
        line++;
    }
    result[output] = '\0';
    return result;
}

static int annotate_no_throw_file(XrVMRuntime *runtime, const char *path, int *changed_functions) {
    char *source = xr_cli_read_file(path);
    if (!source) {
        xr_cli_error("fix", "cannot read file '%s'", path);
        return 1;
    }
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(runtime);
    AstNode *ast = xr_parse_with_source(session, source, path);
    if (!ast) {
        xr_cli_error("fix", "refusing to edit syntactically invalid file '%s'", path);
        xr_free(source);
        return 1;
    }
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    xa_analyzer_analyze(analyzer, path, (XrAstNode *) ast);
    int diagnostic_count = 0;
    XaDiagnostic *diagnostic = xa_analyzer_get_diagnostics(analyzer, &diagnostic_count);
    bool has_error = false;
    for (XaDiagnostic *item = diagnostic; item; item = item->next) {
        if (item->severity == XR_DIAG_SEV_ERROR) {
            has_error = true;
            fprintf(stderr, "%s:%u:%u: error: %s\n", path, item->location.line,
                    item->location.column, item->message ? item->message : "analysis failed");
        }
    }
    if (has_error) {
        xr_cli_error("fix", "refusing to edit semantically invalid file '%s'", path);
        xa_analyzer_free(analyzer);
        xr_program_destroy(ast);
        xr_free(source);
        return 1;
    }

    int line_count = source_line_count(source);
    bool *lines = (bool *) xr_calloc((size_t) line_count + 1, sizeof(bool));
    NoThrowAnnotationScan scan = {
        .analyzer = analyzer, .lines = lines, .line_count = line_count, .annotation_count = 0};
    if (lines)
        xa_ast_walk(ast, collect_no_throw_annotation, NULL, &scan);
    int result = 0;
    if (scan.annotation_count > 0) {
        char *updated =
            insert_no_throw_annotations(source, lines, line_count, scan.annotation_count);
        if (!updated || xr_cli_write_file_atomic(path, updated) != 0) {
            xr_cli_error("fix", "cannot atomically update file '%s'", path);
            result = 1;
        } else {
            printf("Annotated %d function%s: %s\n", scan.annotation_count,
                   scan.annotation_count == 1 ? "" : "s", path);
            if (changed_functions)
                *changed_functions += scan.annotation_count;
        }
        xr_free(updated);
    }
    xr_free(lines);
    xa_analyzer_free(analyzer);
    xr_program_destroy(ast);
    xr_free(source);
    return result;
}

static int annotate_no_throw_directory(XrVMRuntime *runtime, const char *path,
                                       int *changed_functions) {
    XrDirIter *iterator = xr_dir_open(path);
    if (!iterator) {
        xr_cli_error("fix", "cannot open directory '%s'", path);
        return 1;
    }
    int errors = 0;
    XrDirEntry entry;
    char child[XR_CLI_PATH_MAX];
    while (xr_dir_next(iterator, &entry)) {
        if (entry.name[0] == '.')
            continue;
        int written = snprintf(child, sizeof(child), "%s/%s", path, entry.name);
        if (written < 0 || (size_t) written >= sizeof(child)) {
            errors++;
            continue;
        }
        if (entry.is_dir) {
            if (strcmp(entry.name, "build") == 0 || strcmp(entry.name, "build-asan") == 0 ||
                strcmp(entry.name, "node_modules") == 0)
                continue;
            errors += annotate_no_throw_directory(runtime, child, changed_functions);
        } else if (xr_cli_is_xr_file(entry.name)) {
            errors += annotate_no_throw_file(runtime, child, changed_functions);
        }
    }
    xr_dir_close(iterator);
    return errors;
}

int cmd_fix(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    if (!xr_cli_opt_bool(&inv->options, "annotate-no-throw")) {
        xr_cli_error("fix", "select a fix; currently supported: --annotate-no-throw");
        return XR_CLI_EXIT_USAGE;
    }
    XrVMRuntime *runtime = xr_isolate_profile_new(XR_ISOLATE_PROFILE_ANALYZE);
    if (!runtime) {
        xr_cli_error("fix", "failed to create analysis isolate");
        return XR_CLI_EXIT_INTERNAL;
    }
    int errors = 0;
    int changed_functions = 0;
    for (int i = 0; i < inv->positional_count; i++) {
        const char *path = inv->positionals[i];
        XrFsStat stat;
        if (xr_fs_stat(path, &stat) != 0) {
            xr_cli_error("fix", "path does not exist '%s'", path);
            errors++;
        } else if (stat.kind == XR_FS_DIR) {
            errors += annotate_no_throw_directory(runtime, path, &changed_functions);
        } else if (stat.kind == XR_FS_FILE && xr_cli_is_xr_file(path)) {
            errors += annotate_no_throw_file(runtime, path, &changed_functions);
        } else {
            xr_cli_error("fix", "expected an .xr file or directory, got '%s'", path);
            errors++;
        }
    }
    if (!errors && changed_functions == 0)
        printf("No provably no-throw functions require annotation.\n");
    xray_vm_delete(runtime);
    return errors ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}
