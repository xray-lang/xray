/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_driver.c - AOT native compilation driver (Xi IR pipeline)
 *
 * KEY CONCEPT:
 *   Full pipeline from source file to generated C program:
 *   1. Module graph discovery (topo-sorted via XrModuleGraph)
 *   2. Cross-module analysis with shared XaAnalyzer (typed imports)
 *   3. Per-module: canonicalize → Xi IR lower → optimize → select_rep
 *   4. Cross-module import resolution via export table
 *   5. C code generation via xi_cgen
 *   6. Main() generation calling module inits in topo order
 *
 * RELATED MODULES:
 *   - xi_cgen.h: Xi IR → C code generation
 *   - xaot_driver.h: public API
 *   - xcmd_build.c: CLI entry that invokes xaot_build + CC
 */

#include "xaot_driver.h"
#include "../../include/xray.h"
#include "../../include/xray_isolate.h"
#include "../runtime/xisolate_api.h"
#include "../module/xmodule_graph.h"
#include "../module/xmodule_resolver.h"
#include "../module/xmodule.h"
#include "../base/xmalloc.h"
#include "../base/xmemstream.h"
#include "../base/xglobal_indices.h"
#include "../ir/xi.h"
#include "../ir/xi_pipeline.h"
#include "../ir/xi_import_resolve.h"
#include "xi_cgen.h"
#include "xi_lto.h"
#include "xaot_bundle.h"
#include "xaot_link.h"
#include "xaot_prepare.h"
#include "xaot_verify.h"
#include "../frontend/analyzer/xanalyzer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#include <stdlib.h>
#define realpath(path, resolved) _fullpath((resolved), (path), PATH_MAX)
#endif

/* Create a full-runtime isolate for AOT compilation.
 * Equivalent to XR_ISOLATE_PROFILE_RUN without depending on the
 * isolate-profile factory in src/api/. */
static XrayIsolate *create_isolate(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    return xray_isolate_new(&params);
}

/* ========== Module Name Helpers ========== */

/* Derive a C-safe module name from absolute path.  Caller must free. */
static char *derive_module_name(const char *path) {
    XR_DCHECK(path != NULL, "derive_module_name: NULL path");
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t len = strlen(base);
    if (len > 3 && strcmp(base + len - 3, ".xr") == 0)
        len -= 3;
    char *name = (char *) xr_malloc(len + 1);
    if (!name)
        return NULL;
    for (size_t i = 0; i < len; i++)
        name[i] = (base[i] == '-' || base[i] == '.') ? '_' : base[i];
    name[len] = '\0';
    return name;
}

/* Graph-based import resolution: delegate to shared xi_import_resolve utility */

/* ========== Feature Inference ========== */

/* Map import module name to XaotStdlibSet flag.
 * Import names are bare identifiers (e.g. "math", "crypto");
 * relative paths (starting with "./") are user modules, not stdlib.
 * Json is a builtin type and not an import module. */
static XaotStdlibSet stdlib_flag_for_import(const char *name) {
    if (!name || name[0] == '.')
        return 0;

    struct {
        const char *name;
        XaotStdlibSet flag;
    } table[] = {
        {"regex", XAOT_STDLIB_REGEX},   {"math", XAOT_STDLIB_MATH},
        {"time", XAOT_STDLIB_TIME},     {"datetime", XAOT_STDLIB_TIME},
        {"path", XAOT_STDLIB_PATH},     {"io", XAOT_STDLIB_IO},
        {"os", XAOT_STDLIB_OS},         {"net", XAOT_STDLIB_NET},
        {"http", XAOT_STDLIB_HTTP},     {"crypto", XAOT_STDLIB_CRYPTO},
        {"base64", XAOT_STDLIB_BASE64}, {"encoding", XAOT_STDLIB_ENCODING},
        {"url", XAOT_STDLIB_URL},       {"csv", XAOT_STDLIB_CSV},
        {"toml", XAOT_STDLIB_TOML},     {"yaml", XAOT_STDLIB_YAML},
        {"xml", XAOT_STDLIB_XML},       {"compress", XAOT_STDLIB_COMPRESS},
    };
    for (int i = 0; i < (int) (sizeof(table) / sizeof(table[0])); i++) {
        if (strcmp(name, table[i].name) == 0)
            return table[i].flag;
    }
    return 0;
}

static void features_add_stdlib_symbol(XaotFeatureSet *fs, const char *symbol) {
    if (!fs || !symbol || !symbol[0] || strlen(symbol) >= XAOT_STDLIB_SYMBOL_NAME_MAX)
        return;
    for (uint16_t i = 0; i < fs->n_stdlib_symbols; i++) {
        if (strcmp(fs->stdlib_symbols[i], symbol) == 0)
            return;
    }
    if (fs->n_stdlib_symbols >= XAOT_MAX_STDLIB_SYMBOLS)
        return;
    memcpy(fs->stdlib_symbols[fs->n_stdlib_symbols], symbol, strlen(symbol) + 1);
    fs->n_stdlib_symbols++;
}

/* Record "module.member" into the referenced stdlib-symbol closure. */
static void features_add_stdlib_member(XaotFeatureSet *fs, const char *module, const char *member) {
    if (!fs || !module || !module[0] || !member || !member[0])
        return;
    char symbol[XAOT_STDLIB_SYMBOL_NAME_MAX];
    int n = snprintf(symbol, sizeof(symbol), "%s.%s", module, member);
    if (n <= 0 || n >= (int) sizeof(symbol))
        return;
    features_add_stdlib_symbol(fs, symbol);
}

/* Unwrap value-identity ops (box/unbox/copy/move) to the underlying value,
 * mirroring cg_unwrap_identity_value so feature inference sees the same module
 * identity the emitter resolves at the call site. */
static const XiValue *stdlib_unwrap_value(const XiValue *v) {
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX ||
            (v->op == XI_COPY && !xi_copy_is_value_clone(v)) || v->op == XI_MOVE) &&
           v->nargs >= 1)
        v = v->args[0];
    return v;
}

/* Extract the import-ref carried by a value, if it is an XI_IMPORT_REF. */
static const XiImportRef *stdlib_value_import_ref(const XiValue *v) {
    v = stdlib_unwrap_value(v);
    if (!v || v->op != XI_IMPORT_REF || !v->aux)
        return NULL;
    return (const XiImportRef *) v->aux;
}

/* Find the import-ref stored into a shared slot within f (SET_SHARED scan),
 * mirroring cg_shared_slot_import_ref. */
static const XiImportRef *stdlib_shared_slot_import_ref(const XiFunc *f, int slot) {
    if (!f || slot < 0)
        return NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            const XiImportRef *ref = stdlib_value_import_ref(v->args[0]);
            if (ref)
                return ref;
        }
    }
    return NULL;
}

/* Resolve the module import-ref a value refers to, following shared-slot
 * indirection (e.g. `import time` stored into a shared slot then read back at
 * the `time.now()` call site). Mirrors cg_value_is_module_import resolution. */
static const XiImportRef *stdlib_module_ref_of_value(const XiFunc *f, const XiValue *v) {
    v = stdlib_unwrap_value(v);
    const XiImportRef *ref = stdlib_value_import_ref(v);
    if (ref)
        return ref;
    if (v && v->op == XI_GET_SHARED) {
        ref = stdlib_shared_slot_import_ref(f, (int) v->aux_int);
        if (!ref && f && f->module && f->module->init != f)
            ref = stdlib_shared_slot_import_ref(f->module->init, (int) v->aux_int);
    }
    return ref;
}

/* If v refers to a stdlib module with symbol-level tracking (bare module
 * identifier in the stdlib table, excluding core math which is tracked via
 * XI_CALL_BUILTIN), return that module name; else NULL. Used to record the
 * referenced-symbol closure for method-form stdlib calls. */
static const char *stdlib_symbol_module_of_value(const XiFunc *f, const XiValue *v) {
    const XiImportRef *ref = stdlib_module_ref_of_value(f, v);
    if (!ref || !ref->module_path || ref->member_name)
        return NULL;
    XaotStdlibSet flag = stdlib_flag_for_import(ref->module_path);
    if (flag == 0 || flag == XAOT_STDLIB_MATH)
        return NULL;
    return ref->module_path;
}

static bool feature_copy_needs_deep_clone(const XiValue *v) {
    if (!v || v->op != XI_COPY || v->nargs < 1 || !v->args[0])
        return false;
    return xi_copy_is_value_clone(v);
}

/* Scan a single XiFunc (non-recursive) for feature-indicating ops */
static void scan_func_features(XiFunc *f, XaotFeatureSet *fs) {
    XR_DCHECK(f != NULL, "scan_func_features: NULL func");
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            switch (v->op) {
                case XI_YIELD:
                    fs->need_coro = true;
                    break;
                case XI_GO:
                    fs->need_coro = true;
                    fs->need_netpoll = true;
                    break;
                case XI_CHAN_NEW:
                case XI_CHAN_SEND:
                case XI_CHAN_RECV:
                case XI_CHAN_TRY_SEND:
                case XI_CHAN_TRY_RECV:
                case XI_CHAN_IS_CLOSED:
                case XI_TIME_AFTER:
                case XI_CHAN_TIMER_DISPOSE:
                case XI_SELECT_BLOCK:
                    fs->need_channel = true;
                    fs->need_coro = true;
                    if (v->op == XI_TIME_AFTER || v->op == XI_CHAN_TIMER_DISPOSE)
                        fs->need_timer = true;
                    break;
                case XI_SCOPE_ENTER:
                case XI_SCOPE_EXIT:
                    fs->need_scope = true;
                    fs->need_coro = true;
                    break;
                case XI_AWAIT:
                    fs->need_coro = true;
                    break;
                case XI_GET_BUILTIN:
                    /* WorkQueue lowers through a generic builtin constructor call
                     * rather than a dedicated XI op (unlike channels), so it must
                     * be detected here. Any WorkQueue use pulls in the isolate /
                     * scheduler runtime that codegen's xray_isolate_setup_full
                     * (emitted for WorkQueue-bearing entries) depends on. */
                    if (v->aux_int == XR_GLOBAL_VAR_WORKQUEUE ||
                        v->aux_int == XR_GLOBAL_VAR_RESULTGROUP)
                        fs->need_coro = true;
                    break;
                case XI_CALL_METHOD:
                    if (v->aux && strcmp((const char *) v->aux, "sleep") == 0 && v->nargs == 2) {
                        fs->need_coro = true;
                        fs->need_timer = true;
                    }
                    /* Module-form call (e.g. `time.now()`): the receiver is an
                     * import-ref naming the stdlib module, the method name is in
                     * aux. Record the precise referenced symbol. */
                    if (v->aux && v->nargs >= 1) {
                        const char *mod = stdlib_symbol_module_of_value(f, v->args[0]);
                        if (mod)
                            features_add_stdlib_member(fs, mod, (const char *) v->aux);
                    }
                    break;
                case XI_CALL_BUILTIN: {
                    const char *name = (const char *) v->aux;
                    if (name && strncmp(name, "math.", 5) == 0) {
                        fs->stdlib |= XAOT_STDLIB_MATH;
                        features_add_stdlib_symbol(fs, name);
                    }
                    break;
                }
                case XI_COPY:
                    if (feature_copy_needs_deep_clone(v))
                        fs->need_deep_copy = true;
                    break;
                case XI_TRY:
                case XI_THROW:
                    fs->need_exception = true;
                    break;
                case XI_IS:
                    fs->need_instanceof = true;
                    break;
                case XI_IMPORT_REF: {
                    XiImportRef *ref = (XiImportRef *) v->aux;
                    if (ref && ref->module_path) {
                        XaotStdlibSet flag = stdlib_flag_for_import(ref->module_path);
                        if (flag)
                            fs->stdlib |= flag;
                        /* net/http imply netpoll runtime */
                        if (flag & (XAOT_STDLIB_NET | XAOT_STDLIB_HTTP))
                            fs->need_netpoll = true;
                        /* time implies timer subsystem */
                        if (flag & XAOT_STDLIB_TIME)
                            fs->need_timer = true;
                        /* Member-import form (e.g. `import { now } from "time"`):
                         * the import-ref carries the member name directly. Core
                         * math is excluded; it is tracked via XI_CALL_BUILTIN. */
                        if (flag && flag != XAOT_STDLIB_MATH && ref->member_name)
                            features_add_stdlib_member(fs, ref->module_path, ref->member_name);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

/* Recursively infer features from an Xi IR function tree */
static void infer_features_recursive(XiFunc *f, XaotFeatureSet *fs) {
    if (!f)
        return;
    scan_func_features(f, fs);
    for (uint16_t c = 0; c < f->nchildren; c++)
        infer_features_recursive(f->children[c], fs);
}

/* Infer XaotFeatureSet for the entire compiled bundle */
static void infer_features(XiFunc **ir_funcs, int nmodules, XaotFeatureSet *fs) {
    memset(fs, 0, sizeof(*fs));
    for (int m = 0; m < nmodules; m++)
        infer_features_recursive(ir_funcs[m], fs);
}

static bool add_stdlib_manifest_entries(XaotLinkManifest *manifest, XaotStdlibSet stdlib) {
    struct {
        XaotStdlibSet flag;
        const char *name;
    } table[] = {
        {XAOT_STDLIB_JSON, "json"}, {XAOT_STDLIB_REGEX, "regex"}, {XAOT_STDLIB_TIME, "time"},
        {XAOT_STDLIB_IO, "io"},     {XAOT_STDLIB_OS, "os"},       {XAOT_STDLIB_NET, "net"},
        {XAOT_STDLIB_HTTP, "http"}, {XAOT_STDLIB_CSV, "csv"},     {XAOT_STDLIB_TOML, "toml"},
        {XAOT_STDLIB_YAML, "yaml"}, {XAOT_STDLIB_XML, "xml"},
    };

    for (uint32_t i = 0; i < (uint32_t) (sizeof(table) / sizeof(table[0])); i++) {
        if ((stdlib & table[i].flag) &&
            !xaot_link_manifest_add_unique(manifest, XAOT_LINK_STDLIB_OBJECT, table[i].name))
            return false;
    }
    return true;
}

static bool add_stdlib_symbol_manifest_entries(XaotLinkManifest *manifest,
                                               const XaotFeatureSet *features) {
    for (uint16_t i = 0; i < features->n_stdlib_symbols; i++) {
        if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_STDLIB_SYMBOL,
                                           features->stdlib_symbols[i]))
            return false;
    }
    return true;
}

static bool stdlib_set_needs_runtime_provider(XaotStdlibSet stdlib) {
    return (stdlib &
            ~(XAOT_STDLIB_MATH | XAOT_STDLIB_PATH | XAOT_STDLIB_ENCODING | XAOT_STDLIB_BASE64 |
              XAOT_STDLIB_URL | XAOT_STDLIB_COMPRESS | XAOT_STDLIB_CRYPTO)) != 0;
}

static bool build_link_manifest(const XaotFeatureSet *features, XaotLinkManifest *manifest) {
    XaotTarget target;
    bool ok = false;

    if (!features || !manifest)
        return false;
    if (!xaot_target_init(&target, "native-c90"))
        return false;
    if (!xaot_link_manifest_init(manifest, &target)) {
        xaot_target_free(&target);
        return false;
    }
    xaot_target_free(&target);

    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_GENERATED_C_FILE, "<aot-generated-c>"))
        goto done;
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_DEFINE, "XRT_IMPL"))
        goto done;
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "m"))
        goto done;
#ifdef XR_OS_MACOS
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-Wl,-dead_strip"))
        goto done;
#else
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-ffunction-sections"))
        goto done;
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_CC_FLAG, "-fdata-sections"))
        goto done;
    if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, "-Wl,--gc-sections"))
        goto done;
#endif

    if (features->need_coro || features->need_channel || features->need_scope ||
        features->need_timer || features->need_netpoll || features->need_deep_copy ||
        features->need_exception || features->need_reflection || features->need_stacktrace ||
        features->need_instanceof || stdlib_set_needs_runtime_provider(features->stdlib)) {
        if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_RUNTIME_OBJECT, "xray_core"))
            goto done;
        if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "pthread"))
            goto done;
        if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "z"))
            goto done;
#ifdef XRAY_HAVE_LIBFFI
        /* xray_core embeds the VM's libffi-based @extern invoker (xi_ffi.c), so
         * a native program that links the runtime must resolve libffi too. */
        if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "ffi"))
            goto done;
#endif
#ifdef XR_HAS_IO_URING
        /* xray_core embeds the per-worker io_uring completion rings, so a native
         * program that links the runtime must resolve liburing too. */
        if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "uring"))
            goto done;
#endif
#ifdef XR_OS_MACOS
        {
            /* Default to the Apple-Silicon Homebrew prefix, but allow an
             * override for Intel Homebrew (/usr/local) or custom prefixes. */
            const char *ssl_libdir = getenv("XRAY_OPENSSL_LIBDIR");
            char ssl_flag[512];
            snprintf(ssl_flag, sizeof(ssl_flag), "-L%s",
                     ssl_libdir && ssl_libdir[0] ? ssl_libdir : "/opt/homebrew/opt/openssl@3/lib");
            if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_LD_FLAG, ssl_flag))
                goto done;
        }
#endif
#ifdef XR_ENABLE_TLS
        if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "ssl"))
            goto done;
        if (!xaot_link_manifest_add_unique(manifest, XAOT_LINK_SYSTEM_LIB, "crypto"))
            goto done;
#endif
    }

    if (!add_stdlib_manifest_entries(manifest, features->stdlib))
        goto done;
    if (!add_stdlib_symbol_manifest_entries(manifest, features))
        goto done;
    ok = true;

done:
    if (!ok)
        xaot_link_manifest_free(manifest);
    return ok;
}

static int report_analyzer_diagnostics(XaAnalyzer *analyzer, const char *fallback_file) {
    int diag_count = 0;
    int error_count = 0;
    XaDiagnostic *diags = xa_analyzer_get_diagnostics(analyzer, &diag_count);
    (void) diag_count;
    for (XaDiagnostic *d = diags; d; d = d->next) {
        if (d->severity == XR_DIAG_SEV_ERROR)
            error_count++;
        const char *sev = "error";
        if (d->severity == XR_DIAG_SEV_WARNING)
            sev = "warning";
        else if (d->severity == XR_DIAG_SEV_INFO)
            sev = "info";
        else if (d->severity == XR_DIAG_SEV_HINT)
            sev = "hint";
        const char *file =
            d->location.file ? d->location.file : (fallback_file ? fallback_file : "?");
        fprintf(stderr, "%s:%u:%u: %s: %s\n", file, (unsigned) d->location.line,
                (unsigned) d->location.column, sev, d->message ? d->message : "");
    }
    return error_count;
}

XR_FUNC int xaot_build(const char *input_path, bool emit_plan_dump, XaotBuildResult *result) {
    XR_DCHECK(input_path != NULL, "xaot_build: NULL input_path");
    XR_DCHECK(result != NULL, "xaot_build: NULL result");
    memset(result, 0, sizeof(*result));

    printf("[xi-native] Building: %s\n", input_path);

    /* --- Build module graph (topo order, entry last) --- */
    XrayIsolate *X = create_isolate();
    if (!X) {
        fprintf(stderr, "Error: failed to create isolate\n");
        return 1;
    }

    xr_module_system_init_with_script(X, input_path);
    XrModuleRegistry *registry = xr_isolate_get_module_registry(X);
    XrModuleResolver *resolver = xr_module_registry_get_resolver(registry);
    XrModuleGraph *graph = xr_module_graph_new(X, resolver);
    if (!graph) {
        fprintf(stderr, "Error: failed to create module graph\n");
        xray_isolate_delete(X);
        return 1;
    }

    char *build_err = NULL;
    if (xr_module_graph_build(graph, input_path, &build_err) != 0) {
        fprintf(stderr, "Error: module graph build failed: %s\n", build_err ? build_err : "?");
        xr_free(build_err);
        xr_module_graph_free(graph);
        xray_isolate_delete(X);
        return 1;
    }
    xr_free(build_err);

    xr_module_graph_topological_sort(graph);
    if (graph->has_cycle) {
        fprintf(stderr, "Error: %s\n",
                graph->cycle_desc ? graph->cycle_desc : "circular dependency detected");
        xr_module_graph_free(graph);
        xray_isolate_delete(X);
        return 1;
    }

    int nmodules = graph->topo_count;
    int entry_index = -1;

    /* Build parallel arrays for paths/names (graph topo order) */
    char **paths = (char **) xr_calloc(nmodules, sizeof(char *));
    char **mod_names = (char **) xr_calloc(nmodules, sizeof(char *));
    if (!paths || !mod_names) {
        xr_free(paths);
        xr_free(mod_names);
        xr_module_graph_free(graph);
        xray_isolate_delete(X);
        return 1;
    }
    /* Resolve input_path to canonical form (handles symlinks like /tmp -> /private/tmp) */
    char real_input[PATH_MAX];
    if (!realpath(input_path, real_input))
        strncpy(real_input, input_path, PATH_MAX - 1);

    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        paths[ti] = xr_strdup(spec->source_path);
        mod_names[ti] = derive_module_name(spec->source_path);
        if (strcmp(spec->source_path, real_input) == 0)
            entry_index = ti;
    }
    /* Entry is the last module in topo order (all deps come first) */
    if (entry_index < 0)
        entry_index = nmodules - 1;

    if (nmodules > 1) {
        printf("[xi-native] %d modules (topo order):\n", nmodules);
        for (int i = 0; i < nmodules; i++)
            printf("  [%d] %s%s\n", i, paths[i], i == entry_index ? " (entry)" : "");
    }

    /* --- Analyze all modules with shared analyzer (cross-module types) --- */
    XaAnalyzer *shared_analyzer = xa_analyzer_new(X);
    if (!shared_analyzer) {
        fprintf(stderr, "Error: failed to create shared analyzer\n");
        goto fail_free_graph;
    }
    xa_analyzer_set_graph(shared_analyzer, graph);

    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (!spec->ast || !spec->source_path)
            continue;
        xa_analyzer_analyze(shared_analyzer, spec->source_path, (XrAstNode *) spec->ast);
        int file_errors = report_analyzer_diagnostics(shared_analyzer, spec->source_path);
        if (file_errors > 0) {
            fprintf(stderr, "Error: semantic analysis failed for '%s'\n", spec->source_path);
            goto fail_free_analyzer;
        }
        spec->exports = xa_analyzer_collect_exports(shared_analyzer, (XrAstNode *) spec->ast);
        xa_analyzer_clear_diagnostics(shared_analyzer);
    }

    /* --- Compile all modules through Xi IR pipeline --- */
    XiPipelineConfig cfg = xi_pipeline_aot_config();
    XiPipelineResult *pres_arr = (XiPipelineResult *) xr_calloc(nmodules, sizeof(XiPipelineResult));
    XiFunc **ir_funcs = (XiFunc **) xr_calloc(nmodules, sizeof(XiFunc *));
    XiModule **modules = (XiModule **) xr_calloc(nmodules, sizeof(XiModule *));
    XaotBundle aot_bundle;
    bool aot_bundle_initialized = false;
    XaotPrepareStats prepare_stats;
    char *plan_dump = NULL;
    XaotLinkManifest link_manifest;
    bool link_manifest_initialized = false;
    memset(&aot_bundle, 0, sizeof(aot_bundle));
    memset(&prepare_stats, 0, sizeof(prepare_stats));
    memset(&link_manifest, 0, sizeof(link_manifest));
    if (!pres_arr || !ir_funcs || !modules) {
        xr_free(pres_arr);
        xr_free(ir_funcs);
        xr_free(modules);
        xa_analyzer_set_graph(shared_analyzer, NULL);
        xa_analyzer_free(shared_analyzer);
        goto fail_free_graph;
    }

    int total_funcs = 0;
    for (int ti = 0; ti < nmodules; ti++) {
        int idx = graph->topo_order[ti];
        XrModuleSpec *spec = &graph->specs[idx];
        if (!spec->ast || !spec->source_path) {
            pres_arr[ti].status = XI_PIPE_ERR_INTERNAL;
            fprintf(stderr, "Error: no AST for module '%s'\n", paths[ti]);
            goto fail_free_ir;
        }

        /* Compile using the shared analyzer (has cross-module type info) */
        pres_arr[ti] = xi_pipeline_compile_program((AstNode *) spec->ast, shared_analyzer, X, &cfg);
        if (pres_arr[ti].status != XI_PIPE_OK) {
            fprintf(stderr, "Error: Xi pipeline failed for '%s': %s\n", paths[ti],
                    xi_pipe_status_str(pres_arr[ti].status));
            if (pres_arr[ti].error_msg)
                fprintf(stderr, "  %s\n", pres_arr[ti].error_msg);
            goto fail_free_ir;
        }
        ir_funcs[ti] = pres_arr[ti].ir;
        XR_DCHECK(ir_funcs[ti] != NULL, "xaot_build: pipeline OK but NULL IR");
        total_funcs += 1 + ir_funcs[ti]->nchildren;

        XR_DCHECK(ir_funcs[ti]->module != NULL, "xaot_build: pipeline produced no module metadata");
        modules[ti] = ir_funcs[ti]->module;
        modules[ti]->path = paths[ti];
        modules[ti]->name = mod_names[ti];
        ir_funcs[ti]->module = NULL;
    }
    xa_analyzer_set_graph(shared_analyzer, NULL);
    xa_analyzer_free(shared_analyzer);
    shared_analyzer = NULL;

    /* --- Resolve XI_IMPORT_REF using graph (before graph is freed) --- */
    for (int ti = 0; ti < nmodules; ti++) {
        xi_resolve_imports(ir_funcs[ti], graph, paths[ti], modules, nmodules);
    }

    /* --- Cross-module LTO: direct-bind imported callees --- */
    {
        XiLtoContext lto;
        if (xi_lto_context_init(&lto, modules, (uint32_t) nmodules))
            (void) xi_lto_link_modules(&lto);
        xi_lto_context_free(&lto);
    }

    /* --- AOT target prepare: build sidecar rep/ABI plan before C emission --- */
    if (!xaot_bundle_init(&aot_bundle, modules, (uint32_t) nmodules, (uint32_t) entry_index)) {
        fprintf(stderr, "Error: failed to initialize AOT bundle plan\n");
        goto fail_free_ir;
    }
    aot_bundle_initialized = true;
    if (!xaot_prepare_bundle(&aot_bundle, &prepare_stats)) {
        fprintf(stderr, "Error: AOT prepare failed: %s\n",
                aot_bundle.error_msg ? aot_bundle.error_msg : "?");
        goto fail_free_ir;
    }
    {
        char verify_err[512];
        if (!xaot_verify_bundle(&aot_bundle, XAOT_VERIFY_AOT_READY, verify_err,
                                sizeof(verify_err))) {
            fprintf(stderr, "Error: AOT verifier failed: %s\n", verify_err);
            goto fail_free_ir;
        }
    }
    /* The plan dump is O(functions x values) diagnostics; only build it when
     * the caller actually wants it (--dump-xaot-plan). */
    if (emit_plan_dump) {
        plan_dump = xaot_bundle_dump_plan(&aot_bundle);
        if (!plan_dump) {
            fprintf(stderr, "Error: failed to dump AOT prepare plan\n");
            goto fail_free_ir;
        }
    }

    /* Graph ASTs must not be freed before compilation is done.
     * Now that pipeline is complete, free the graph (frees ASTs too). */
    xr_module_graph_free(graph);
    graph = NULL;
    xray_isolate_delete(X);
    X = NULL;

    /* --- Create codegen context (no global state) --- */
    XiCgenCtx *cg_ctx = xi_cgen_ctx_new();
    if (!cg_ctx) {
        fprintf(stderr, "Error: failed to create codegen context\n");
        goto fail_free_ir;
    }
    xi_cgen_ctx_set_aot_bundle(cg_ctx, &aot_bundle);

    /* --- Resolve cross-module imports for C codegen --- */
    xi_cgen_resolve_module_imports(cg_ctx, modules, nmodules);

    /* --- Generate C: one translation unit per module --- */
    XaotModuleSource *sources =
        (XaotModuleSource *) xr_calloc((size_t) nmodules, sizeof(XaotModuleSource));
    if (!sources) {
        xi_cgen_ctx_free(cg_ctx);
        goto fail_free_ir;
    }
    int n_sources = 0;
    bool emit_ok = true;
    size_t total_c_bytes = 0;

    for (int m = 0; m < nmodules && emit_ok; m++) {
        char *buf = NULL;
        size_t bufsz = 0;
        FILE *mem = xr_open_memstream(&buf, &bufsz);
        if (!mem) {
            fprintf(stderr, "Error: xr_open_memstream failed\n");
            emit_ok = false;
            break;
        }
        if (nmodules == 1) {
            /* Single-module bundle stays a single self-contained unit (no
             * cross-module symbols, so it keeps file-static linkage). */
            xi_cgen_program(cg_ctx, mem, modules[m]);
        } else {
            /* Multi-module: emit module m as an independently compilable unit
             * (external cross-module symbols; entry unit carries main). */
            xi_cgen_module_tu(cg_ctx, mem, modules, nmodules, m, entry_index);
        }
        if (xr_close_memstream(mem, &buf, &bufsz) != 0) {
            fprintf(stderr, "Error: xr_close_memstream failed\n");
            xr_free(buf);
            emit_ok = false;
            break;
        }
        sources[m].name = xr_strdup(mod_names[m] ? mod_names[m] : "module");
        sources[m].c_source = buf;
        n_sources++;
        total_c_bytes += bufsz;
    }

    if (!emit_ok || xi_cgen_has_error(cg_ctx)) {
        fprintf(stderr, "Error: AOT C code generation failed\n");
        xi_cgen_ctx_free(cg_ctx);
        for (int m = 0; m < n_sources; m++) {
            xr_free(sources[m].name);
            xr_free(sources[m].c_source);
        }
        xr_free(sources);
        goto fail_free_ir;
    }
    XiCgenStats cgen_stats = xi_cgen_stats(cg_ctx);
    XiCgenCoroFrameStats coro_frame_stats = xi_cgen_coro_frame_stats(cg_ctx);
    xi_cgen_ctx_free(cg_ctx);

    /* Infer runtime features before freeing IR */
    XaotFeatureSet features;
    infer_features(ir_funcs, nmodules, &features);
    if (!build_link_manifest(&features, &link_manifest)) {
        fprintf(stderr, "Error: failed to build AOT link manifest\n");
        goto fail_free_ir;
    }
    link_manifest_initialized = true;

    xaot_bundle_free(&aot_bundle);
    aot_bundle_initialized = false;

    /* Free IR and module metadata (no longer needed after C generation) */
    for (int m = 0; m < nmodules; m++) {
        xi_module_free(modules[m]);
        xi_pipeline_result_free(&pres_arr[m]);
    }
    xr_free(modules);
    xr_free(pres_arr);
    xr_free(ir_funcs);

    printf("[xi-native] Generated %zu bytes of C (%d functions, %d modules in %d unit%s)\n",
           total_c_bytes, total_funcs, nmodules, n_sources, n_sources == 1 ? "" : "s");

    /* Each source buffer is xr_malloc-owned (xr_close_memstream guarantees this
     * on every platform); ownership transfers into the result. */
    result->sources = sources;
    result->n_sources = n_sources;
    result->plan_dump = plan_dump;
    plan_dump = NULL;
    result->link_manifest = link_manifest;
    memset(&link_manifest, 0, sizeof(link_manifest));
    link_manifest_initialized = false;
    result->total_compiled = total_funcs;
    result->total_aot = total_funcs;
    result->nmodules = nmodules;
    result->features = features;
    result->prepare_stats = prepare_stats;
    result->cgen_stats = cgen_stats;
    result->coro_frame_stats = coro_frame_stats;

    /* Cleanup module name arrays */
    for (int i = 0; i < nmodules; i++) {
        xr_free(paths[i]);
        xr_free(mod_names[i]);
    }
    xr_free(paths);
    xr_free(mod_names);
    return 0;

fail_free_ir:
    xr_free(plan_dump);
    if (link_manifest_initialized)
        xaot_link_manifest_free(&link_manifest);
    if (aot_bundle_initialized)
        xaot_bundle_free(&aot_bundle);
    for (int m = 0; m < nmodules; m++) {
        if (modules)
            xi_module_free(modules[m]);
        xi_pipeline_result_free(&pres_arr[m]);
    }
    xr_free(modules);
    xr_free(pres_arr);
    xr_free(ir_funcs);
    if (shared_analyzer) {
        xa_analyzer_set_graph(shared_analyzer, NULL);
        xa_analyzer_free(shared_analyzer);
        shared_analyzer = NULL;
    }
fail_free_analyzer:
    if (shared_analyzer) {
        xa_analyzer_set_graph(shared_analyzer, NULL);
        xa_analyzer_free(shared_analyzer);
        shared_analyzer = NULL;
    }
fail_free_graph:
    if (graph)
        xr_module_graph_free(graph);
    if (X)
        xray_isolate_delete(X);
    for (int i = 0; i < nmodules; i++) {
        xr_free(paths[i]);
        xr_free(mod_names[i]);
    }
    xr_free(paths);
    xr_free(mod_names);
    return 1;
}

XR_FUNC void xaot_build_result_free(XaotBuildResult *result) {
    if (!result)
        return;
    if (result->sources) {
        for (int i = 0; i < result->n_sources; i++) {
            xr_free(result->sources[i].name);
            xr_free(result->sources[i].c_source);
        }
        xr_free(result->sources);
    }
    xr_free(result->plan_dump);
    xaot_link_manifest_free(&result->link_manifest);
    memset(result, 0, sizeof(*result));
}
