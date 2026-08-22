/*
 * xray - Lightweight typed scripting with native concurrency
 * xcmd_language.c - Public language-surface inventory
 */

#include "xcli_spec.h"
#include "xcli_diag.h"
#include "../../api/xisolate_profile.h"
#include "../../base/xmalloc.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_ast_visitor.h"
#include "../../frontend/parser/xast_api.h"
#include "../../frontend/parser/xattribute_registry.h"
#include "../../module/xmodule.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_resolver.h"
#include "../../runtime/xisolate_api.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../base/xchecks.h"
#include "xray_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_json_string(const char *value) {
    const unsigned char *p = (const unsigned char *) (value ? value : "");
    putchar('"');
    while (*p) {
        switch (*p) {
            case '"':
                fputs("\\\"", stdout);
                break;
            case '\\':
                fputs("\\\\", stdout);
                break;
            case '\n':
                fputs("\\n", stdout);
                break;
            case '\r':
                fputs("\\r", stdout);
                break;
            case '\t':
                fputs("\\t", stdout);
                break;
            default:
                if (*p < 0x20)
                    printf("\\u%04x", (unsigned) *p);
                else
                    putchar((int) *p);
                break;
        }
        p++;
    }
    putchar('"');
}

static int language_attributes(bool json) {
    size_t count = xr_public_attribute_count();
    if (json) {
        fputs("{\"schema_version\":1,\"count\":", stdout);
        printf("%zu,\"attributes\":[", count);
        for (size_t i = 0; i < count; i++) {
            const XrPublicAttributeInfo *info = xr_public_attribute_at(i);
            if (i)
                putchar(',');
            fputs("{\"spelling\":", stdout);
            print_json_string(info->spelling);
            fputs(",\"targets\":", stdout);
            print_json_string(info->targets);
            fputs(",\"arguments\":", stdout);
            print_json_string(info->arguments);
            fputs(",\"category\":", stdout);
            print_json_string(info->category);
            fputs(",\"phase\":", stdout);
            print_json_string(info->phase);
            printf(",\"production\":%s,\"impact\":", info->production ? "true" : "false");
            print_json_string(info->impact);
            fputs(",\"stability\":", stdout);
            print_json_string(info->stability);
            putchar('}');
        }
        fputs("]}\n", stdout);
        return XR_CLI_EXIT_OK;
    }

    printf("Public attributes (%zu):\n", count);
    for (size_t i = 0; i < count; i++) {
        const XrPublicAttributeInfo *info = xr_public_attribute_at(i);
        printf("  @%-12s targets=%-23s args=%s\n", info->spelling, info->targets, info->arguments);
        printf("                 category=%s phase=%s production=%s stability=%s\n", info->category,
               info->phase,
               info->production ? "yes" : "no", info->stability);
        printf("                 impact=%s\n", info->impact);
    }
    return XR_CLI_EXIT_OK;
}

typedef struct LanguageConversionRecord {
    const char *file; /* borrowed from XrModuleGraph through emission */
    const AstNode *node;
    XrConversionWitness witness;
} LanguageConversionRecord;

typedef struct LanguageConversionCollector {
    XaAnalyzer *analyzer;
    const char *file;
    LanguageConversionRecord *records;
    const char **files;
    size_t count;
    size_t capacity;
    size_t file_count;
    const char *unresolved_file;
    const AstNode *unresolved_node;
    XrConversionWitness unresolved_witness;
    bool failed;
} LanguageConversionCollector;

static void language_collect_conversion(AstNode *node, void *opaque) {
    LanguageConversionCollector *collector = (LanguageConversionCollector *) opaque;
    XrConversionWitness witness = {0};
    if (!collector || collector->failed || !node ||
        !xa_analyzer_get_node_conversion(collector->analyzer, node, &witness))
        return;
    if (witness.kind == XR_CONVERSION_NONE || witness.kind == XR_CONVERSION_DISALLOWED) {
        collector->unresolved_file = collector->file;
        collector->unresolved_node = node;
        collector->unresolved_witness = witness;
        collector->failed = true;
        return;
    }
    if (collector->count == collector->capacity) {
        size_t next_capacity = collector->capacity ? collector->capacity * 2 : 64;
        LanguageConversionRecord *next = (LanguageConversionRecord *) xr_realloc(
            collector->records, next_capacity * sizeof(*collector->records));
        if (!next) {
            collector->failed = true;
            return;
        }
        collector->records = next;
        collector->capacity = next_capacity;
    }
    collector->records[collector->count++] = (LanguageConversionRecord) {
        .file = collector->file,
        .node = node,
        .witness = witness,
    };
}

static int language_compare_conversion(const void *left, const void *right) {
    const LanguageConversionRecord *a = (const LanguageConversionRecord *) left;
    const LanguageConversionRecord *b = (const LanguageConversionRecord *) right;
    int path_order = strcmp(a->file ? a->file : "", b->file ? b->file : "");
    if (path_order != 0)
        return path_order;
    if (a->node->line != b->node->line)
        return a->node->line < b->node->line ? -1 : 1;
    if (a->node->column != b->node->column)
        return a->node->column < b->node->column ? -1 : 1;
    return a->node->node_id < b->node->node_id ? -1 : a->node->node_id > b->node->node_id ? 1 : 0;
}

static int language_compare_path(const void *left, const void *right) {
    const char *const *a = (const char *const *) left;
    const char *const *b = (const char *const *) right;
    return strcmp(*a ? *a : "", *b ? *b : "");
}

static const char *language_scalar_name(uint8_t scalar_rep) {
    const char *name = xr_scalar_rep_name(scalar_rep);
    return name ? name : "dynamic";
}

static void language_emit_conversions_json(const LanguageConversionCollector *collector) {
    size_t kind_counts[XR_CONVERSION_DISALLOWED + 1] = {0};
    for (size_t i = 0; i < collector->count; i++)
        kind_counts[collector->records[i].witness.kind]++;

    printf("{\"schema_version\":1,\"file_count\":%zu,\"files\":[", collector->file_count);
    for (size_t i = 0; i < collector->file_count; i++) {
        if (i)
            putchar(',');
        print_json_string(collector->files[i]);
    }
    printf("],\"count\":%zu,\"unresolved\":0,\"kinds\":{", collector->count);
    bool first_kind = true;
    for (int kind = XR_CONVERSION_IDENTITY; kind < XR_CONVERSION_DISALLOWED; kind++) {
        if (kind_counts[kind] == 0)
            continue;
        if (!first_kind)
            putchar(',');
        first_kind = false;
        print_json_string(xr_conversion_kind_name((XrConversionKind) kind));
        printf(":%zu", kind_counts[kind]);
    }
    fputs("},\"conversions\":[", stdout);
    for (size_t i = 0; i < collector->count; i++) {
        const LanguageConversionRecord *record = &collector->records[i];
        const XrConversionWitness *witness = &record->witness;
        if (i)
            putchar(',');
        fputs("{\"file\":", stdout);
        print_json_string(record->file);
        printf(",\"line\":%d,\"column\":%d,\"node_id\":%u,\"syntax\":",
               record->node->line, record->node->column, record->node->node_id);
        print_json_string(xr_ast_typename(record->node->type));
        fputs(",\"source\":", stdout);
        print_json_string(language_scalar_name(witness->source_scalar_rep));
        fputs(",\"target\":", stdout);
        print_json_string(language_scalar_name(witness->target_scalar_rep));
        fputs(",\"kind\":", stdout);
        print_json_string(xr_conversion_kind_name(witness->kind));
        printf(",\"implicit\":%s,\"compile_time\":%s}",
               witness->is_implicit ? "true" : "false",
               witness->is_compile_time ? "true" : "false");
    }
    fputs("]}\n", stdout);
}

static void language_emit_conversions_text(const LanguageConversionCollector *collector) {
    printf("Source conversions (%zu files, %zu records, unresolved=0):\n",
           collector->file_count, collector->count);
    for (size_t i = 0; i < collector->count; i++) {
        const LanguageConversionRecord *record = &collector->records[i];
        const XrConversionWitness *witness = &record->witness;
        printf("  %s:%d:%d %s %s -> %s %s%s\n", record->file ? record->file : "?",
               record->node->line, record->node->column,
               xr_conversion_kind_name(witness->kind),
               language_scalar_name(witness->source_scalar_rep),
               language_scalar_name(witness->target_scalar_rep),
               witness->is_implicit ? "implicit" : "explicit",
               witness->is_compile_time ? " compile-time" : "");
    }
}

static int language_conversions(const char *input, bool json) {
    int rc = XR_CLI_EXIT_FAIL;
    XrVMRuntime *isolate = xr_isolate_profile_new(XR_ISOLATE_PROFILE_ANALYZE);
    XrModuleGraph *graph = NULL;
    XaAnalyzer *analyzer = NULL;
    LanguageConversionCollector collector = {0};
    char *graph_error = NULL;
    if (!isolate)
        return XR_CLI_EXIT_INTERNAL;

    xr_module_system_init_with_script(isolate, input);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(isolate);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    graph = resolver ? xr_module_graph_new(session, resolver) : NULL;
    XrModuleIdentityAuthority authority = {0};
    char *authority_root = NULL;
    int graph_rc = graph && xr_module_identity_script_authority_from_source(
                                input, &authority, &authority_root)
                       ? xr_module_graph_build(graph, input, &authority, &graph_error)
                       : -1;
    xr_free(authority_root);
    if (graph_rc != 0 ||
        xr_module_graph_topological_sort(graph) != 0 || graph->has_cycle) {
        xr_cli_error("language", "%s", graph_error ? graph_error : "module graph build failed");
        goto cleanup;
    }
    analyzer = xa_analyzer_new(session);
    if (!analyzer) {
        rc = XR_CLI_EXIT_INTERNAL;
        goto cleanup;
    }
    xa_analyzer_set_graph(analyzer, graph);
    collector.analyzer = analyzer;

    for (int ti = 0; ti < graph->topo_count; ti++) {
        XrModuleSpec *spec = &graph->specs[graph->topo_order[ti]];
        xa_analyzer_analyze(analyzer, spec->source_path, (XrAstNode *) spec->ast);
        spec->export_symbols =
            xa_analyzer_collect_export_symbols(analyzer, (XrAstNode *) spec->ast);
        int diagnostic_count = 0;
        for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &diagnostic_count); diag;
             diag = diag->next) {
            const char *severity = diag->severity == XR_DIAG_SEV_WARNING ? "warning" : "error";
            fprintf(stderr, "%s:%u:%u: %s: %s\n", spec->source_path,
                    diag->location.line, diag->location.column, severity,
                    diag->message ? diag->message : "analysis failed");
            if (diag->severity == XR_DIAG_SEV_ERROR)
                collector.failed = true;
        }
        xa_analyzer_clear_diagnostics(analyzer);
    }
    if (collector.failed)
        goto cleanup;

    for (int i = 0; i < graph->spec_count; i++) {
        XrModuleSpec *spec = &graph->specs[i];
        collector.file = spec->source_path;
        xa_ast_walk(spec->ast, language_collect_conversion, NULL, &collector);
    }
    if (collector.failed) {
        if (collector.unresolved_node) {
            xr_cli_error("language",
                         "unresolved conversion at %s:%d:%d (%s %s -> %s, kind=%s)",
                         collector.unresolved_file ? collector.unresolved_file : "?",
                         collector.unresolved_node->line,
                         collector.unresolved_node->column,
                         xr_ast_typename(collector.unresolved_node->type),
                         language_scalar_name(collector.unresolved_witness.source_scalar_rep),
                         language_scalar_name(collector.unresolved_witness.target_scalar_rep),
                         xr_conversion_kind_name(collector.unresolved_witness.kind));
            rc = XR_CLI_EXIT_FAIL;
        } else {
            xr_cli_error("language", "conversion inventory allocation failed");
            rc = XR_CLI_EXIT_INTERNAL;
        }
        goto cleanup;
    }
    collector.file_count = (size_t) graph->spec_count;
    collector.files = (const char **) xr_calloc(collector.file_count, sizeof(*collector.files));
    if (collector.file_count != 0 && !collector.files) {
        rc = XR_CLI_EXIT_INTERNAL;
        goto cleanup;
    }
    for (size_t i = 0; i < collector.file_count; i++)
        collector.files[i] = graph->specs[i].source_path;
    qsort(collector.files, collector.file_count, sizeof(*collector.files), language_compare_path);
    qsort(collector.records, collector.count, sizeof(*collector.records),
          language_compare_conversion);
    if (json)
        language_emit_conversions_json(&collector);
    else
        language_emit_conversions_text(&collector);
    rc = XR_CLI_EXIT_OK;

cleanup:
    xr_free(graph_error);
    xr_free(collector.files);
    xr_free(collector.records);
    if (analyzer) {
        xa_analyzer_set_graph(analyzer, NULL);
        xa_analyzer_free(analyzer);
    }
    if (graph)
        xr_module_graph_free(graph);
    xray_vm_delete(isolate);
    return rc;
}

XR_FUNC int cmd_language(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    if (inv->positional_count < 1) {
        xr_cli_error("language", "expected subcommand 'attributes' or 'conversions'");
        return XR_CLI_EXIT_USAGE;
    }
    bool json = inv->ctx->json_output || xr_cli_opt_bool(&inv->options, "json");
    if (strcmp(inv->positionals[0], "attributes") == 0 && inv->positional_count == 1)
        return language_attributes(json);
    if (strcmp(inv->positionals[0], "conversions") == 0 && inv->positional_count == 2)
        return language_conversions(inv->positionals[1], json);
    xr_cli_error("language", "invalid subcommand or arguments for '%s'", inv->positionals[0]);
    return XR_CLI_EXIT_USAGE;
}
