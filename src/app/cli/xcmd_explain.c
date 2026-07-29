/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_explain.c - Evidence/provenance explanation command
 */

#include "xcli.h"
#include "xcli_fs.h"
#include "xcli_spec.h"
#include "../../module/xproject.h"
#include "../../module/xmodule.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_resolver.h"
#include "../../api/xisolate_profile.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_mono.h"
#include "../../frontend/analyzer/xa_effect_db.h"
#include "../../frontend/analyzer/xa_memory_effect_db.h"
#include "../../runtime/xisolate_api.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../base/xfileio.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../aot/xaot_driver.h"
#include <stdio.h>
#include <string.h>

static int explain_native(const XrCliInvocation *inv, const char *input) {
    char root[XR_CLI_PATH_MAX];
    if (!xr_cli_find_project_root(input ? input : ".", root, sizeof(root))) {
        xr_cli_error("explain", "no xray.toml found from '%s'", input ? input : ".");
        return XR_CLI_EXIT_USAGE;
    }
    XrProject *project = xr_project_load(NULL, root);
    if (!project) {
        xr_cli_error("explain", "cannot load project at '%s'", root);
        return XR_CLI_EXIT_FAIL;
    }
    if (!project->native_plan) {
        printf("native-plan: none\n");
        xr_project_free(project);
        return XR_CLI_EXIT_OK;
    }
    if (xr_cli_opt_bool(&inv->options, "json")) {
        const XrNativePackagePlan *plan = project->native_plan;
        printf("{\"package\":\"%s\",\"version\":\"%s\",\"audit\":\"%s\","
               "\"valid\":%s,\"fingerprint\":\"%016llx\",\"units\":%u,\"symbols\":%u}\n",
               plan->name ? plan->name : "", plan->version ? plan->version : "",
               xr_native_audit_mode_name(plan->audit_mode), plan->valid ? "true" : "false",
               (unsigned long long) plan->fingerprint, plan->unit_count, plan->symbol_count);
    } else {
        xr_native_package_explain(project->native_plan, stdout);
    }
    int result = project->native_plan->valid ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;
    xr_project_free(project);
    return result;
}

static const char *explain_final_component(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot ? dot + 1 : name;
}

static bool explain_symbol_name_matches(const char *subject, const char *candidate) {
    const char *component;
    size_t subject_len;
    if (!subject || !candidate)
        return false;
    if (strcmp(subject, candidate) == 0)
        return true;
    component = explain_final_component(candidate);
    if (strcmp(subject, component) == 0)
        return true;
    subject_len = strlen(subject);
    return strncmp(component, subject, subject_len) == 0 &&
           (component[subject_len] == '$' || component[subject_len] == '#');
}

static bool explain_line_has_prefix(const char *line, size_t line_len, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    return line_len >= prefix_len && memcmp(line, prefix, prefix_len) == 0;
}

static bool explain_line_field(const char *line, size_t line_len, const char *field, char *out,
                               size_t out_size) {
    size_t field_len = strlen(field);
    const char *limit = line + line_len;
    const char *match = line;
    if (!out || out_size < 2)
        return false;
    while (match + field_len <= limit) {
        match = strstr(match, field);
        if (!match || match + field_len > limit)
            return false;
        if (match == line || match[-1] == ' ') {
            const char *start = match + field_len;
            const char *end = start;
            while (end < limit && *end != ' ' && *end != '\t' && *end != '\r')
                end++;
            size_t len = (size_t) (end - start);
            if (len == 0 || len >= out_size)
                return false;
            memcpy(out, start, len);
            out[len] = '\0';
            return true;
        }
        match += field_len;
    }
    return false;
}

static bool explain_dump_has_function(const char *dump, const char *subject) {
    const char *line = dump;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t) (end - line) : strlen(line);
        char function[256];
        if (explain_line_has_prefix(line, line_len, "xi-evidence ") &&
            explain_line_field(line, line_len, "function=", function, sizeof(function)) &&
            explain_symbol_name_matches(subject, function)) {
            return true;
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

static bool explain_record_prefix_matches(const char *topic, const char *line, size_t line_len) {
    if (strcmp(topic, "codegen") == 0)
        return explain_line_has_prefix(line, line_len, "codegen-control ") ||
               explain_line_has_prefix(line, line_len, "codegen-edge ");
    if (strcmp(topic, "view") == 0)
        return explain_line_has_prefix(line, line_len, "view-param ") ||
               explain_line_has_prefix(line, line_len, "view-return ") ||
               explain_line_has_prefix(line, line_len, "view-evidence ");
    if (strcmp(topic, "bounds") == 0) {
        return explain_line_has_prefix(line, line_len, "bounds ") ||
               explain_line_has_prefix(line, line_len, "bounds-unproven ") ||
               explain_line_has_prefix(line, line_len, "span-access ") ||
               explain_line_has_prefix(line, line_len, "span-access-unproven ");
    }
    return strcmp(topic, "alias") == 0 && explain_line_has_prefix(line, line_len, "alias ");
}

static bool explain_codegen_record_matches_subject(const char *subject, const char *line,
                                                    size_t line_len) {
    static const char *fields[] = {"function=", "caller=", "callee="};
    char candidate[256];
    if (!explain_record_prefix_matches("codegen", line, line_len))
        return false;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (explain_line_field(line, line_len, fields[i], candidate, sizeof(candidate)) &&
            explain_symbol_name_matches(subject, candidate))
            return true;
    }
    return false;
}

static bool explain_dump_has_codegen_subject(const char *dump, const char *subject) {
    const char *line = dump;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t) (end - line) : strlen(line);
        if (explain_codegen_record_matches_subject(subject, line, line_len))
            return true;
        line = end ? end + 1 : NULL;
    }
    return false;
}

static bool explain_codegen_record_seen_before(const char *dump, const char *current,
                                               size_t current_len) {
    const char *line = dump;
    while (line && line < current) {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t) (end - line) : strlen(line);
        if (line_len == current_len && memcmp(line, current, current_len) == 0)
            return true;
        line = end ? end + 1 : NULL;
    }
    return false;
}

static uint32_t explain_print_records(const char *topic, const char *subject, const char *dump) {
    const char *line = dump;
    uint32_t count = 0;
    const char *field =
        strcmp(topic, "view") == 0 || strcmp(topic, "codegen") == 0 ? "function=" : "func=";
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t) (end - line) : strlen(line);
        char function[256];
        bool matches = strcmp(topic, "codegen") == 0
                           ? explain_codegen_record_matches_subject(subject, line, line_len)
                           : explain_record_prefix_matches(topic, line, line_len) &&
                                 explain_line_field(line, line_len, field, function,
                                                    sizeof(function)) &&
                                 explain_symbol_name_matches(subject, function);
        if (matches && strcmp(topic, "codegen") == 0 &&
            explain_codegen_record_seen_before(dump, line, line_len))
            matches = false;
        if (matches) {
            fwrite(line, 1, line_len, stdout);
            fputc('\n', stdout);
            count++;
        }
        line = end ? end + 1 : NULL;
    }
    return count;
}

static void explain_print_json_string(const char *text, size_t len) {
    fputc('"', stdout);
    for (size_t i = 0; text && i < len; i++) {
        unsigned char ch = (unsigned char) text[i];
        switch (ch) {
            case '"':
                fputs("\\\"", stdout);
                break;
            case '\\':
                fputs("\\\\", stdout);
                break;
            case '\b':
                fputs("\\b", stdout);
                break;
            case '\f':
                fputs("\\f", stdout);
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
                if (ch < 0x20)
                    printf("\\u%04x", (unsigned) ch);
                else
                    fputc(ch, stdout);
                break;
        }
    }
    fputc('"', stdout);
}

static bool explain_codegen_json_key(const char *key, size_t key_len, const char **json_key,
                                     bool *is_number) {
    static const struct {
        const char *ledger;
        const char *json;
        bool number;
    } fields[] = {
        {"function", "function", false},
        {"caller", "caller", false},
        {"callee", "callee", false},
        {"kind", "kind", false},
        {"value", "value", false},
        {"call", "call", false},
        {"source-line", "sourceLine", true},
        {"stage", "stage", false},
        {"backend", "backend", false},
        {"provider-capability", "providerCapability", false},
        {"lowering", "lowering", false},
        {"reason", "reason", false},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (strlen(fields[i].ledger) == key_len &&
            strncmp(key, fields[i].ledger, key_len) == 0) {
            *json_key = fields[i].json;
            *is_number = fields[i].number;
            return true;
        }
    }
    return false;
}

static void explain_print_codegen_json_record(const char *line, size_t line_len) {
    const char *end = line + line_len;
    const char *cursor = (const char *) memchr(line, ' ', line_len);
    const bool is_edge = explain_line_has_prefix(line, line_len, "codegen-edge ");
    fputs("{\"type\":", stdout);
    explain_print_json_string(is_edge ? "edge" : "control", is_edge ? 4 : 7);
    if (!cursor) {
        fputc('}', stdout);
        return;
    }
    cursor++;
    while (cursor < end) {
        while (cursor < end && *cursor == ' ')
            cursor++;
        const char *token_end = cursor;
        while (token_end < end && *token_end != ' ')
            token_end++;
        const char *equal =
            (const char *) memchr(cursor, '=', (size_t) (token_end - cursor));
        if (equal) {
            const char *json_key = NULL;
            bool is_number = false;
            if (explain_codegen_json_key(cursor, (size_t) (equal - cursor), &json_key,
                                         &is_number)) {
                fputc(',', stdout);
                explain_print_json_string(json_key, strlen(json_key));
                fputc(':', stdout);
                if (is_number) {
                    fwrite(equal + 1, 1, (size_t) (token_end - equal - 1), stdout);
                } else {
                    explain_print_json_string(equal + 1, (size_t) (token_end - equal - 1));
                }
            }
        }
        cursor = token_end;
    }
    fputc('}', stdout);
}

static uint32_t explain_print_codegen_json(const char *subject, const char *dump) {
    const char *line = dump;
    uint32_t count = 0;
    fputs("{\"schemaVersion\":1,\"topic\":\"codegen\",\"subject\":", stdout);
    explain_print_json_string(subject, strlen(subject));
    fputs(",\"records\":[", stdout);
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t line_len = end ? (size_t) (end - line) : strlen(line);
        if (explain_codegen_record_matches_subject(subject, line, line_len) &&
            !explain_codegen_record_seen_before(dump, line, line_len)) {
            if (count)
                fputc(',', stdout);
            explain_print_codegen_json_record(line, line_len);
            count++;
        }
        line = end ? end + 1 : NULL;
    }
    printf("],\"count\":%u}\n", count);
    return count;
}

static int explain_backend_topic(const XrCliInvocation *inv, const char *topic,
                                 const char *subject) {
    char root[XR_CLI_PATH_MAX];
    XrProject *project = NULL;
    char *entry = NULL;
    XaotTarget target = {0};
    XaotBuildOptions options = {0};
    XaotBuildResult result = {0};
    const char *records = NULL;
    uint32_t count;
    int rc = XR_CLI_EXIT_FAIL;
    if (!xr_cli_find_project_root(".", root, sizeof(root))) {
        xr_cli_error("explain", "%s explanation requires a project xray.toml", topic);
        return XR_CLI_EXIT_USAGE;
    }
    project = xr_project_load(NULL, root);
    if (!project || !project->initialized || !project->main) {
        xr_cli_error("explain", "project has no valid main entry");
        goto cleanup;
    }
    entry = xr_path_join(root, project->main);
    if (!entry || !xaot_target_init(&target, "native-c90")) {
        rc = XR_CLI_EXIT_INTERNAL;
        goto cleanup;
    }
    options.target = &target;
    options.native_package_plan = project->native_plan;
    options.profile = XAOT_BUILD_PROFILE_HOSTED;
    options.type_name_profile = XI_CGEN_TYPE_NAMES_ALL;
    options.emit_plan_dump = true;
    options.emit_local_evidence_dump = true;
    options.quiet = true;
    if (xaot_build(entry, &options, &result) != 0) {
        xr_cli_error("explain", "could not build evidence for '%s'", subject);
        goto cleanup;
    }
    if (!explain_dump_has_function(result.local_evidence_dump, subject) &&
        !(strcmp(topic, "codegen") == 0 &&
          explain_dump_has_codegen_subject(result.local_evidence_dump, subject))) {
        xr_cli_error("explain", "function '%s' is missing", subject);
        goto cleanup_result;
    }
    records = strcmp(topic, "view") == 0 || strcmp(topic, "codegen") == 0
                  ? result.local_evidence_dump
                  : result.plan_dump;
    if (strcmp(topic, "codegen") == 0 && xr_cli_opt_bool(&inv->options, "json")) {
        count = explain_print_codegen_json(subject, records);
    } else {
        printf("%s subject=%s\n", topic, subject);
        count = explain_print_records(topic, subject, records);
        if (count == 0)
            printf("%s-evidence: none\n", topic);
    }
    rc = XR_CLI_EXIT_OK;

cleanup_result:
    xaot_build_result_free(&result);
cleanup:
    if (target.name)
        xaot_target_free(&target);
    xr_free(entry);
    xr_project_free(project);
    return rc;
}

static XaSymbol *explain_find_export(XrModuleGraph *graph, const char *subject) {
    XaSymbol *short_match = NULL;
    if (!graph || !subject)
        return NULL;
    for (int i = 0; i < graph->spec_count; i++) {
        XrHashMap *exports = graph->specs[i].export_symbols;
        if (!exports)
            continue;
        XaSymbol *exact = (XaSymbol *) xr_hashmap_get(exports, subject);
        if (exact)
            return exact;
        for (uint32_t slot = 0; slot < exports->capacity; slot++) {
            XrHashMapEntry *entry = &exports->entries[slot];
            if (!entry->key || entry->value == XR_HASHMAP_TOMBSTONE)
                continue;
            if (strcmp(explain_final_component(entry->key), subject) == 0) {
                if (short_match && short_match != (XaSymbol *) entry->value)
                    return NULL;
                short_match = (XaSymbol *) entry->value;
            }
        }
    }
    return short_match;
}

static void explain_print_memory_effect(const XaMemoryEffectSummary *memory) {
    if (!memory) {
        printf("memory-effect: missing\n");
        return;
    }
    printf("memory-effect complete=%s unknown=0x%x fingerprint=%016llx roots=%u\n",
           xa_memory_effect_summary_is_complete(memory) ? "yes" : "no", memory->unknown_reasons,
           (unsigned long long) xa_memory_effect_summary_fingerprint(memory), memory->root_count);
    for (uint32_t i = 0; i < memory->root_count; i++) {
        const XaMemoryRootEffect *root = &memory->roots[i];
        printf("  root kind=%u index=%u writes=%u rebind=%u relocate=%u shorten=%u "
               "invalidate=%u\n",
               (unsigned) root->root.kind, root->root.index, root->write_count,
               root->descriptor_rebind, (unsigned) root->relocation, (unsigned) root->shortening,
               (unsigned) root->invalidation);
    }
}

static int explain_effect(const char *subject) {
    char root[XR_CLI_PATH_MAX];
    if (!xr_cli_find_project_root(".", root, sizeof(root))) {
        xr_cli_error("explain", "effect explanation requires a project xray.toml");
        return XR_CLI_EXIT_USAGE;
    }
    XrProject *project = xr_project_load(NULL, root);
    if (!project || !project->initialized || !project->main) {
        xr_cli_error("explain", "%s",
                     project && project->native_plan && project->native_plan->error
                         ? project->native_plan->error
                         : "project has no valid main entry");
        xr_project_free(project);
        return XR_CLI_EXIT_FAIL;
    }
    char *entry = xr_path_join(root, project->main);
    XrVMRuntime *isolate = xr_isolate_profile_new(XR_ISOLATE_PROFILE_ANALYZE);
    if (!entry || !isolate) {
        xr_free(entry);
        xr_project_free(project);
        if (isolate)
            xray_vm_delete(isolate);
        return XR_CLI_EXIT_INTERNAL;
    }
    xr_module_system_init_with_script(isolate, entry);
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    xr_compiler_session_set_native_package_plan(session, project->native_plan);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(isolate);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    XrModuleGraph *graph = resolver ? xr_module_graph_new(session, resolver) : NULL;
    char *graph_error = NULL;
    if (!graph || xr_module_graph_build(graph, entry, &graph_error) != 0 ||
        xr_module_graph_topological_sort(graph) != 0 || graph->has_cycle) {
        xr_cli_error("explain", "%s", graph_error ? graph_error : "module graph build failed");
        xr_free(graph_error);
        if (graph)
            xr_module_graph_free(graph);
        xray_vm_delete(isolate);
        xr_free(entry);
        xr_project_free(project);
        return XR_CLI_EXIT_FAIL;
    }
    xr_free(graph_error);
    XaAnalyzer *analyzer = xa_analyzer_new(session);
    if (!analyzer) {
        xr_module_graph_free(graph);
        xray_vm_delete(isolate);
        xr_free(entry);
        xr_project_free(project);
        return XR_CLI_EXIT_INTERNAL;
    }
    xa_analyzer_set_graph(analyzer, graph);
    int errors = 0;
    for (int ti = 0; ti < graph->topo_count; ti++) {
        XrModuleSpec *spec = &graph->specs[graph->topo_order[ti]];
        xa_analyzer_analyze(analyzer, spec->source_path, (XrAstNode *) spec->ast);
        spec->export_symbols =
            xa_analyzer_collect_export_symbols(analyzer, (XrAstNode *) spec->ast);
        int diagnostic_count = 0;
        for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &diagnostic_count); diag;
             diag = diag->next) {
            if (diag->severity == XR_DIAG_SEV_ERROR)
                errors++;
        }
        xa_analyzer_clear_diagnostics(analyzer);
    }
    AstNode **mono_roots = (AstNode **) xr_calloc((size_t) graph->topo_count, sizeof(AstNode *));
    if (mono_roots) {
        for (int ti = 0; ti < graph->topo_count; ti++)
            mono_roots[ti] = graph->specs[graph->topo_order[ti]].ast;
        for (int ti = 0; ti < graph->topo_count; ti++) {
            XrModuleSpec *spec = &graph->specs[graph->topo_order[ti]];
            xa_mono_pass_with_external_structs_and_analyzer(spec->ast, mono_roots,
                                                            graph->topo_count, isolate, analyzer);
        }
        for (int ti = 0; ti < graph->topo_count; ti++) {
            XrModuleSpec *spec = &graph->specs[graph->topo_order[ti]];
            xa_analyzer_analyze(analyzer, spec->source_path, (XrAstNode *) spec->ast);
            if (spec->export_symbols)
                xr_hashmap_free(spec->export_symbols);
            spec->export_symbols =
                xa_analyzer_collect_export_symbols(analyzer, (XrAstNode *) spec->ast);
            int diagnostic_count = 0;
            for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &diagnostic_count);
                 diag; diag = diag->next) {
                if (diag->severity == XR_DIAG_SEV_ERROR)
                    errors++;
            }
            xa_analyzer_clear_diagnostics(analyzer);
        }
        xr_free(mono_roots);
    }
    XaSymbol *symbol = explain_find_export(graph, subject);
    if (!symbol) {
        xr_cli_error("explain", "exported symbol '%s' is missing or ambiguous", subject);
        errors++;
    } else {
        const XaEffectSummary *effect =
            xa_effect_db_get(analyzer->effect_db, symbol->links.effect_id);
        const XaMemoryEffectSummary *memory =
            xa_memory_effect_db_get(analyzer->memory_effect_db, symbol->links.memory_effect_id);
        printf("symbol %s file=%s\n", symbol->name ? symbol->name : subject,
               symbol->links.file_path ? symbol->links.file_path : "?");
        if (effect) {
            printf("effect complete=%s semantic=0x%x unknown-semantic=0x%x unknown=0x%x "
                   "unsafe=%u fingerprint=%016llx\n",
                   xa_effect_summary_is_complete(effect) ? "yes" : "no", effect->semantic_effects,
                   effect->unknown_semantic_effects, effect->unknown_reasons,
                   effect->contains_unsafe_op,
                   (unsigned long long) xa_effect_summary_fingerprint(analyzer->effect_db, effect));
        } else {
            printf("effect: missing\n");
        }
        explain_print_memory_effect(memory);
    }
    xa_analyzer_set_graph(analyzer, NULL);
    xa_analyzer_free(analyzer);
    xr_module_graph_free(graph);
    xray_vm_delete(isolate);
    xr_free(entry);
    xr_project_free(project);
    return errors ? XR_CLI_EXIT_FAIL : XR_CLI_EXIT_OK;
}

XR_FUNC int cmd_explain(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    const char *topic = inv->positionals[0];
    const char *input = inv->positional_count > 1 ? inv->positionals[1] : ".";
    if (strcmp(topic, "native") == 0)
        return explain_native(inv, input);
    if (strcmp(topic, "effect") == 0) {
        if (inv->positional_count < 2) {
            xr_cli_error("explain", "effect requires a symbol");
            return XR_CLI_EXIT_USAGE;
        }
        return explain_effect(input);
    }
    if (strcmp(topic, "view") == 0 || strcmp(topic, "bounds") == 0 ||
        strcmp(topic, "alias") == 0 || strcmp(topic, "codegen") == 0) {
        if (inv->positional_count < 2) {
            xr_cli_error("explain", "%s requires a function symbol", topic);
            return XR_CLI_EXIT_USAGE;
        }
        return explain_backend_topic(inv, topic, input);
    }
    xr_cli_error("explain",
                 "unknown evidence topic '%s' (expected native, storage, transfer, ownership, "
                 "view, bounds, alias, codegen, effect, or residue)",
                 topic);
    return XR_CLI_EXIT_USAGE;
}
