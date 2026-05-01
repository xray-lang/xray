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
#include "../ir/xi_pipeline.h"
#include "../ir/xi_cgen.h"
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

/* Derive the import string to reach target_path from importer_dir.
 * E.g. "/a/b/math.xr" from "/a/b" → "./math".  Caller must free. */
static char *derive_import_string(const char *target_path, const char *importer_dir) {
    XR_DCHECK(target_path != NULL, "derive_import_string: NULL target");
    XR_DCHECK(importer_dir != NULL, "derive_import_string: NULL dir");
    size_t dir_len = strlen(importer_dir);
    if (strncmp(target_path, importer_dir, dir_len) == 0 && target_path[dir_len] == '/') {
        const char *filename = target_path + dir_len + 1;
        size_t flen = strlen(filename);
        if (flen > 3 && strcmp(filename + flen - 3, ".xr") == 0)
            flen -= 3;
        char *result = (char *) xr_malloc(2 + flen + 1);
        if (!result)
            return NULL;
        result[0] = '.';
        result[1] = '/';
        memcpy(result + 2, filename, flen);
        result[2 + flen] = '\0';
        return result;
    }
    const char *base = strrchr(target_path, '/');
    base = base ? base + 1 : target_path;
    size_t blen = strlen(base);
    if (blen > 3 && strcmp(base + blen - 3, ".xr") == 0)
        blen -= 3;
    char *result = (char *) xr_malloc(2 + blen + 1);
    if (!result)
        return NULL;
    result[0] = '.';
    result[1] = '/';
    memcpy(result + 2, base, blen);
    result[2 + blen] = '\0';
    return result;
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

    XiPipelineConfig cfg = xi_pipeline_default_config();
    cfg.run_optimize = true;
    cfg.run_select_rep = true;

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

    int total_funcs = 0;
    for (int m = 0; m < nmodules; m++) {
        pres_arr[m] = xi_compile_one(paths[m], X);
        if (pres_arr[m].status != XI_PIPE_OK) {
            /* Clean up already-compiled modules */
            for (int j = 0; j <= m; j++)
                xi_pipeline_result_free(&pres_arr[j]);
            xr_free(pres_arr);
            xr_free(ir_funcs);
            xray_isolate_delete(X);
            goto fail_free_names;
        }
        ir_funcs[m] = pres_arr[m].ir;
        XR_DCHECK(ir_funcs[m] != NULL, "xaot_build: pipeline OK but NULL IR");
        total_funcs += 1 + ir_funcs[m]->nchildren;
    }
    xray_isolate_delete(X);

    /* --- Build cross-module import resolution table --- */
    xi_cgen_reset_imports();
    if (nmodules > 1) {
        for (int exporter = 0; exporter < nmodules; exporter++) {
            XiFunc *ef = ir_funcs[exporter];
            if (!ef || !ef->export_names || ef->nshared == 0) continue;

            /* Compute the directory of each importer to derive relative paths */
            for (int importer = 0; importer < nmodules; importer++) {
                if (importer == exporter) continue;
                /* Derive the import string that the importer would use to
                 * reference the exporter (e.g. "./math_lib") */
                char importer_dir[1024];
                strncpy(importer_dir, paths[importer], sizeof(importer_dir) - 1);
                importer_dir[sizeof(importer_dir) - 1] = '\0';
                char *last_slash = strrchr(importer_dir, '/');
                if (last_slash) *last_slash = '\0';

                char *import_str = derive_import_string(paths[exporter], importer_dir);
                if (!import_str) continue;

                for (uint16_t slot = 0; slot < ef->nshared; slot++) {
                    if (!ef->export_names[slot]) continue;
                    const char *ename = ef->export_names[slot];

                    /* Find child XiFunc matching export name (functions) */
                    const XiFunc *target_fn = NULL;
                    for (uint16_t ci = 0; ci < ef->nchildren; ci++) {
                        if (ef->children[ci] && ef->children[ci]->name &&
                            strcmp(ef->children[ci]->name, ename) == 0) {
                            target_fn = ef->children[ci];
                            break;
                        }
                    }

                    /* Find XiClassData if this slot holds a class.
                     * Scan exporter IR for SET_SHARED(slot, CLASS_CREATE). */
                    const XiClassData *target_cd = NULL;
                    for (uint32_t bi = 0; bi < ef->nblocks; bi++) {
                        const XiBlock *blk = ef->blocks[bi];
                        if (!blk) continue;
                        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
                            const XiValue *sv = blk->values[vi];
                            if (!sv || sv->op != XI_SET_SHARED) continue;
                            if ((int)sv->aux_int != (int)slot) continue;
                            if (sv->nargs < 1) continue;
                            const XiValue *src = sv->args[0];
                            if (src->op == XI_CLASS_CREATE && src->aux)
                                target_cd = (const XiClassData *)src->aux;
                            break;
                        }
                    }
                    /* For class exports, also set target_fn to the constructor */
                    if (target_cd && !target_fn) {
                        const XiFunc *ctor = NULL;
                        if (target_cd->methods) {
                            for (uint16_t mi = 0; mi < target_cd->nmethod; mi++) {
                                if (target_cd->methods[mi].is_constructor &&
                                    target_cd->child_idx &&
                                    mi < target_cd->ninst + target_cd->nstat) {
                                    uint16_t idx = target_cd->child_idx[mi];
                                    if (idx < ef->nchildren) {
                                        ctor = ef->children[idx];
                                        break;
                                    }
                                }
                            }
                        }
                        target_fn = ctor;
                    }

                    xi_cgen_add_import(import_str, ename,
                                       mod_names[exporter], (int)slot,
                                       target_fn, target_cd, ef);
                }
                /* import_str ownership: cgen stores pointer, keep alive until
                 * after C generation.  Leak is acceptable (short-lived process). */
            }
        }
    }

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

    /* Free IR (no longer needed after C generation) */
    for (int m = 0; m < nmodules; m++)
        xi_pipeline_result_free(&pres_arr[m]);
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
    memset(&result->features, 0, sizeof(result->features));

    /* Cleanup module name arrays */
    for (int i = 0; i < nmodules; i++) {
        xr_free(paths[i]);
        xr_free(mod_names[i]);
    }
    xr_free(paths);
    xr_free(mod_names);
    return 0;

fail_free_ir:
    for (int m = 0; m < nmodules; m++)
        xi_pipeline_result_free(&pres_arr[m]);
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
