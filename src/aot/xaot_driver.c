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
 *   1. Bundle discovery (topo-sorted module list)
 *   2. Per-module: parse → analyze → Xi IR lower → optimize → select_rep
 *   3. Cross-module import resolution via XiFunc export_names + import table
 *   4. C code generation via xi_cgen
 *   5. Main() generation calling module inits in topo order
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
#include "../module/xbundle.h"
#include "../base/xmalloc.h"
#include "../ir/xi.h"
#include "../ir/xi_pipeline.h"
#include "xi_cgen.h"
#include "../frontend/parser/xparse.h"
#include "../frontend/analyzer/xanalyzer.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* ========== File reading helper (avoids CLI layer dependency) ========== */

/* Create a full-runtime isolate for AOT compilation.
 * Equivalent to XR_CLI_ISOLATE_RUN profile without CLI dependency. */
static XrayIsolate *create_isolate(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    return xray_isolate_new(&params);
}

static char *read_source_file(const char *path) {
    XR_DCHECK(path != NULL, "read_source_file: NULL path");
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *) xr_malloc((size_t) sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t) sz, f);
    fclose(f);
    buf[nread] = '\0';
    return buf;
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


/* ========== Xi IR Pipeline (Source -> AST -> Xi IR -> C) ========== */

/* Compile one source file through the Xi IR pipeline.
 * Returns the pipeline result (caller frees).  On failure, returns
 * pres.status != XI_PIPE_OK and prints an error message. */
static XiPipelineResult xi_compile_one(const char *path, XrayIsolate *X) {
    XiPipelineResult fail = { .status = XI_PIPE_ERR_INTERNAL };

    char *source = read_source_file(path);
    if (!source) {
        fprintf(stderr, "Error: cannot read '%s'\n", path);
        return fail;
    }

    XaAnalyzer *analyzer = xa_analyzer_new(X);
    if (!analyzer) {
        fprintf(stderr, "Error: failed to create analyzer\n");
        xr_free(source);
        return fail;
    }

    AstNode *program = xr_parse(X, source);
    xr_free(source);
    if (!program) {
        fprintf(stderr, "Error: parse failed for '%s'\n", path);
        xa_analyzer_free(analyzer);
        return fail;
    }

    xa_analyzer_analyze(analyzer, path, program);

    XiPipelineConfig cfg = xi_pipeline_aot_config();

    XiPipelineResult pres = xi_pipeline_compile_program(
        program, analyzer, X, &cfg);

    xa_analyzer_free(analyzer);
    xr_program_destroy(program);

    if (pres.status != XI_PIPE_OK) {
        fprintf(stderr, "Error: Xi pipeline failed for '%s': %s\n",
                path, xi_pipe_status_str(pres.status));
        if (pres.error_msg)
            fprintf(stderr, "  %s\n", pres.error_msg);
    }
    return pres;
}

/* ========== Feature Inference ========== */

/* Map import module name to XaotStdlibSet flag.
 * Import names are bare identifiers (e.g. "math", "crypto");
 * relative paths (starting with "./") are user modules, not stdlib.
 * Json is a builtin type and not an import module. */
static XaotStdlibSet stdlib_flag_for_import(const char *name) {
    if (!name || name[0] == '.') return 0;

    struct { const char *name; XaotStdlibSet flag; } table[] = {
        {"regex",    XAOT_STDLIB_REGEX},
        {"math",     XAOT_STDLIB_MATH},
        {"time",     XAOT_STDLIB_TIME},
        {"datetime", XAOT_STDLIB_TIME},
        {"path",     XAOT_STDLIB_PATH},
        {"io",       XAOT_STDLIB_IO},
        {"os",       XAOT_STDLIB_OS},
        {"net",      XAOT_STDLIB_NET},
        {"http",     XAOT_STDLIB_HTTP},
        {"crypto",   XAOT_STDLIB_CRYPTO},
        {"base64",   XAOT_STDLIB_BASE64},
        {"csv",      XAOT_STDLIB_CSV},
        {"toml",     XAOT_STDLIB_TOML},
        {"yaml",     XAOT_STDLIB_YAML},
        {"xml",      XAOT_STDLIB_XML},
        {"compress", XAOT_STDLIB_COMPRESS},
    };
    for (int i = 0; i < (int)(sizeof(table) / sizeof(table[0])); i++) {
        if (strcmp(name, table[i].name) == 0)
            return table[i].flag;
    }
    return 0;
}

/* Scan a single XiFunc (non-recursive) for feature-indicating ops */
static void scan_func_features(XiFunc *f, XaotFeatureSet *fs) {
    XR_DCHECK(f != NULL, "scan_func_features: NULL func");
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v) continue;
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
                    fs->need_channel = true;
                    fs->need_coro = true;
                    break;
                case XI_SCOPE_ENTER:
                case XI_SCOPE_EXIT:
                    fs->need_scope = true;
                    fs->need_coro = true;
                    break;
                case XI_AWAIT:
                    fs->need_coro = true;
                    break;
                case XI_TRY:
                case XI_THROW:
                    fs->need_exception = true;
                    break;
                case XI_IS:
                    fs->need_instanceof = true;
                    break;
                case XI_IMPORT_REF: {
                    XiImportRef *ref = (XiImportRef *)v->aux;
                    if (ref && ref->module_path) {
                        XaotStdlibSet flag = stdlib_flag_for_import(ref->module_path);
                        if (flag) fs->stdlib |= flag;
                        /* net/http imply netpoll runtime */
                        if (flag & (XAOT_STDLIB_NET | XAOT_STDLIB_HTTP))
                            fs->need_netpoll = true;
                        /* time implies timer subsystem */
                        if (flag & XAOT_STDLIB_TIME)
                            fs->need_timer = true;
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
    if (!f) return;
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

XR_FUNC int xaot_build(const char *input_path, XaotBuildResult *result) {
    XR_DCHECK(input_path != NULL, "xaot_build: NULL input_path");
    XR_DCHECK(result != NULL, "xaot_build: NULL result");
    memset(result, 0, sizeof(*result));

    printf("[xi-native] Building: %s\n", input_path);

    /* --- Discover modules via bundle (topo order, entry last) --- */
    int nmodules = 0;
    int entry_index = -1;
    char **paths = NULL;
    char **mod_names = NULL;
    {
        XrayIsolate *Xb = create_isolate();
        if (!Xb) {
            fprintf(stderr, "Error: failed to create isolate\n");
            return 1;
        }
        XrBundle *bundle = xr_bundle_create_ex(Xb, input_path, XR_BUNDLE_DEFAULT);
        if (!bundle) {
            fprintf(stderr, "Error: bundling failed\n");
            xray_isolate_delete(Xb);
            return 1;
        }
        nmodules = bundle->count;
        paths = (char **)xr_calloc(nmodules, sizeof(char *));
        mod_names = (char **)xr_calloc(nmodules, sizeof(char *));
        if (!paths || !mod_names) {
            xr_bundle_free(bundle);
            xray_isolate_delete(Xb);
            xr_free(paths);
            xr_free(mod_names);
            return 1;
        }
        for (int i = 0; i < nmodules; i++) {
            paths[i] = xr_strdup(bundle->entries[i].path);
            mod_names[i] = derive_module_name(bundle->entries[i].path);
            if (bundle->entry_path &&
                strcmp(bundle->entries[i].path, bundle->entry_path) == 0)
                entry_index = i;
        }
        xr_bundle_free(bundle);
        xray_isolate_delete(Xb);
    }
    XR_DCHECK(entry_index >= 0, "xaot_build: entry not found in bundle");

    if (nmodules > 1) {
        printf("[xi-native] %d modules (topo order):\n", nmodules);
        for (int i = 0; i < nmodules; i++)
            printf("  [%d] %s%s\n", i, paths[i],
                   i == entry_index ? " (entry)" : "");
    }

    /* --- Compile all modules through Xi IR pipeline --- */
    XrayIsolate *X = create_isolate();
    if (!X) {
        fprintf(stderr, "Error: failed to create isolate\n");
        goto fail_free_names;
    }

    XiPipelineResult *pres_arr =
        (XiPipelineResult *)xr_calloc(nmodules, sizeof(XiPipelineResult));
    XiFunc **ir_funcs = (XiFunc **)xr_calloc(nmodules, sizeof(XiFunc *));
    if (!pres_arr || !ir_funcs) {
        xr_free(pres_arr);
        xr_free(ir_funcs);
        xray_isolate_delete(X);
        goto fail_free_names;
    }

    /* Module metadata array (parallel to ir_funcs) */
    XiModule **modules = (XiModule **)xr_calloc(nmodules, sizeof(XiModule *));
    if (!modules) {
        xr_free(pres_arr);
        xr_free(ir_funcs);
        xray_isolate_delete(X);
        goto fail_free_names;
    }

    int total_funcs = 0;
    for (int m = 0; m < nmodules; m++) {
        pres_arr[m] = xi_compile_one(paths[m], X);
        if (pres_arr[m].status != XI_PIPE_OK) {
            /* Clean up already-compiled modules */
            for (int j = 0; j <= m; j++)
                xi_pipeline_result_free(&pres_arr[j]);
            for (int j = 0; j < m; j++)
                xi_module_free(modules[j]);
            xr_free(modules);
            xr_free(pres_arr);
            xr_free(ir_funcs);
            xray_isolate_delete(X);
            goto fail_free_names;
        }
        ir_funcs[m] = pres_arr[m].ir;
        XR_DCHECK(ir_funcs[m] != NULL, "xaot_build: pipeline OK but NULL IR");
        total_funcs += 1 + ir_funcs[m]->nchildren;

        /* Build module metadata with explicit exports/classes */
        modules[m] = xi_module_new(paths[m], mod_names[m], ir_funcs[m]);
        if (modules[m])
            xi_module_populate_exports(modules[m]);
    }
    xray_isolate_delete(X);

    /* --- Resolve cross-module imports from module graph --- */
    xi_cgen_resolve_module_imports(modules, nmodules);

    /* --- Generate combined C source --- */
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *mem = open_memstream(&buf, &bufsz);
    if (!mem) {
        fprintf(stderr, "Error: open_memstream failed\n");
        goto fail_free_ir;
    }

    if (nmodules == 1) {
        /* Single-module fast path: use xi_cgen_program for backward compat */
        xi_cgen_program(mem, ir_funcs[0], mod_names[0]);
    } else {
        /* Multi-module: header + per-module sections + combined main */
        xi_cgen_header(mem);
        for (int m = 0; m < nmodules; m++)
            xi_cgen_module(mem, ir_funcs[m], mod_names[m]);
        xi_cgen_main(mem, (const char **)mod_names, ir_funcs,
                     nmodules, entry_index);
    }
    fclose(mem);

    /* Infer runtime features before freeing IR */
    XaotFeatureSet features;
    infer_features(ir_funcs, nmodules, &features);

    /* Free IR and module metadata (no longer needed after C generation) */
    for (int m = 0; m < nmodules; m++) {
        xi_module_free(modules[m]);
        xi_pipeline_result_free(&pres_arr[m]);
    }
    xr_free(modules);
    xr_free(pres_arr);
    xr_free(ir_funcs);

    printf("[xi-native] Generated %zu bytes of C (%d functions, %d modules)\n",
           bufsz, total_funcs, nmodules);

    /* Copy from system-malloc'd open_memstream buffer to xr_malloc */
    char *owned = (char *)xr_malloc(bufsz + 1);
    if (!owned) {
        free(buf);
        goto fail_free_names;
    }
    memcpy(owned, buf, bufsz + 1);
    free(buf);

    result->c_source = owned;
    result->total_compiled = total_funcs;
    result->total_aot = total_funcs;
    result->nmodules = nmodules;
    result->features = features;

    /* Cleanup module name arrays */
    for (int i = 0; i < nmodules; i++) {
        xr_free(paths[i]);
        xr_free(mod_names[i]);
    }
    xr_free(paths);
    xr_free(mod_names);
    return 0;

fail_free_ir:
    for (int m = 0; m < nmodules; m++) {
        xi_module_free(modules[m]);
        xi_pipeline_result_free(&pres_arr[m]);
    }
    xr_free(modules);
    xr_free(pres_arr);
    xr_free(ir_funcs);
fail_free_names:
    for (int i = 0; i < nmodules; i++) {
        xr_free(paths[i]);
        xr_free(mod_names[i]);
    }
    xr_free(paths);
    xr_free(mod_names);
    return 1;
}
