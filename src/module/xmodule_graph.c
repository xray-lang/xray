/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_graph.c - Module dependency graph: BFS build + topological sort
 *
 * KEY CONCEPT:
 *   Starting from an entry file, discover all reachable modules by parsing
 *   each file and resolving its import specifiers.  Then run Tarjan SCC to
 *   produce a valid initialization order or detect cycles.
 */

#include "xmodule_graph.h"
#include "xsemver.h"
#include "xmodule.h"
#include "xstdlib_embedded.h"
#include "../base/xchecks.h"
#include "../base/xfileio.h"
#include "../os/os_file_read.h"
#include "../base/xforward_decl.h"
#include "../base/xhash.h"
#include "../base/xlog.h"
#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../os/os_fs.h"
#include "../toolchain/xcompiler_session.h"

#include <stdio.h>
#include <string.h>

/* Forward declaration for AST destruction */
extern void xr_program_destroy(struct AstNode *ast);

/* ========== Internal Constants ========== */

#define GRAPH_INITIAL_CAP 16
#define GRAPH_MAX_MODULES 1024
#define GRAPH_EMBEDDED_STDLIB_PREFIX "<embedded stdlib>/"

void xr_module_source_fingerprint(const char *source, XrFingerprint *out) {
    if (!out)
        return;
    static const uint8_t domain[] = "xray-module-source-v1\0";
    XrSHA256Context context;
    uint64_t length = source ? (uint64_t) strlen(source) : 0;
    uint8_t length_bytes[8];
    for (uint32_t i = 0; i < sizeof(length_bytes); i++)
        length_bytes[i] = (uint8_t) (length >> (i * 8u));
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    xr_sha256_update(&context, length_bytes, sizeof(length_bytes));
    if (length)
        xr_sha256_update(&context, (const uint8_t *) source, (size_t) length);
    xr_sha256_final(&context, out->bytes);
}

bool xr_module_spec_owns_top_level_decl(const XrModuleSpec *spec, const AstNode *decl) {
    if (!spec || !decl || !spec->ast || spec->ast->type != AST_PROGRAM)
        return false;
    uint32_t matches = 0u;
    const ProgramNode *program = &spec->ast->as.program;
    for (int index = 0; index < program->count; ++index) {
        if (program->statements[index] == decl)
            ++matches;
    }
    return matches == 1u;
}

/* ========== Lifecycle ========== */

XR_FUNC XrModuleGraph *xr_module_graph_new(XrCompilerSession *compiler_session,
                                           XrModuleResolver *resolver) {
    XR_DCHECK(compiler_session != NULL, "xr_module_graph_new: NULL compiler session");
    XR_DCHECK(resolver != NULL, "xr_module_graph_new: NULL resolver");
    if (!compiler_session || !resolver)
        return NULL;
    XrModuleGraph *g = xr_calloc(1, sizeof(XrModuleGraph));
    if (!g)
        return NULL;

    g->specs = xr_calloc(GRAPH_INITIAL_CAP, sizeof(XrModuleSpec));
    if (!g->specs) {
        xr_free(g);
        return NULL;
    }
    g->spec_capacity = GRAPH_INITIAL_CAP;
    g->id_index = xr_hashmap_new();
    if (!g->id_index) {
        xr_free(g->specs);
        xr_free(g);
        return NULL;
    }
    g->resolver = resolver;
    g->compiler_session = compiler_session;
    g->X = xr_compiler_session_vm_host(compiler_session);
    g->entry_index = -1;
    return g;
}

XR_FUNC void xr_module_graph_free(XrModuleGraph *g) {
    if (!g)
        return;

    for (int i = 0; i < g->spec_count; i++) {
        XrModuleSpec *s = &g->specs[i];
        xr_free(s->canonical);
        xr_free(s->logical_path);
        xr_free(s->source_path);
        xr_free((char *) s->authority.namespace_id);
        xr_free((char *) s->authority.physical_root);
        if (s->ast)
            xr_program_destroy(s->ast);
        xr_free(s->dep_indices);
        if (s->export_symbols)
            xr_hashmap_free(s->export_symbols);
    }
    xr_free(g->specs);
    xr_hashmap_free(g->id_index);
    xr_free(g->topo_order);
    xr_free(g->cycle_desc);
    xr_free(g->unresolved_error);
    xr_free(g);
}

/* ========== Internal Helpers ========== */

/* Add a new spec to the graph.  Returns the index, or -1 on OOM. */
static int graph_add_spec(XrModuleGraph *g, const char *canonical, const char *logical_path,
                          const char *source_path, XrModuleKind kind,
                          const XrModuleIdentityAuthority *authority) {
    XR_DCHECK(g != NULL, "graph_add_spec: NULL graph");
    XR_DCHECK(canonical != NULL, "graph_add_spec: NULL canonical");

    if (g->spec_count >= GRAPH_MAX_MODULES) {
        xr_log_warning("module_graph", "module limit (%d) reached", GRAPH_MAX_MODULES);
        return -1;
    }

    if (g->spec_count >= g->spec_capacity) {
        int new_cap = g->spec_capacity * 2;
        XrModuleSpec *tmp = xr_realloc(g->specs, (size_t) new_cap * sizeof(XrModuleSpec));
        if (!tmp)
            return -1;
        g->specs = tmp;
        memset(&g->specs[g->spec_count], 0,
               (size_t) (new_cap - g->spec_capacity) * sizeof(XrModuleSpec));
        g->spec_capacity = new_cap;
    }

    int idx = g->spec_count++;
    XrModuleSpec *s = &g->specs[idx];
    s->canonical = xr_strdup(canonical);
    s->logical_path = logical_path ? xr_strdup(logical_path) : NULL;
    s->source_path = source_path ? xr_strdup(source_path) : NULL;
    s->kind = kind;
    if (authority) {
        s->authority.kind = authority->kind;
        s->authority.namespace_id =
            authority->namespace_id ? xr_strdup(authority->namespace_id) : NULL;
        s->authority.physical_root =
            authority->physical_root ? xr_strdup(authority->physical_root) : NULL;
    }
    s->status = XR_MODSPEC_PENDING;
    s->topo_index = -1;
    s->scc_id = -1;

    /* An unindexed spec would let the same canonical id be added twice,
     * so roll the slot back instead of leaving a half-registered entry. */
    if (!s->canonical || (logical_path && !s->logical_path) || (source_path && !s->source_path) ||
        (authority && authority->namespace_id && !s->authority.namespace_id) ||
        (authority && authority->physical_root && !s->authority.physical_root) ||
        !xr_hashmap_set(g->id_index, s->canonical, (void *) (intptr_t) (idx + 1))) {
        xr_free(s->canonical);
        xr_free(s->logical_path);
        xr_free(s->source_path);
        xr_free((char *) s->authority.namespace_id);
        xr_free((char *) s->authority.physical_root);
        memset(s, 0, sizeof(*s));
        g->spec_count--;
        return -1;
    }
    return idx;
}

/* Add a dependency edge from spec at `from_idx` to spec at `to_idx`. */
static bool spec_add_dep(XrModuleSpec *s, int to_idx) {
    XR_DCHECK(s != NULL, "spec_add_dep: NULL spec");

    /* Deduplicate */
    for (int i = 0; i < s->dep_count; i++) {
        if (s->dep_indices[i] == to_idx)
            return true;
    }

    if (s->dep_count >= s->dep_capacity) {
        int new_cap = s->dep_capacity * 2;
        if (new_cap < 4)
            new_cap = 4;
        int *tmp = xr_realloc(s->dep_indices, (size_t) new_cap * sizeof(int));
        if (!tmp)
            return false;
        s->dep_indices = tmp;
        s->dep_capacity = new_cap;
    }
    s->dep_indices[s->dep_count++] = to_idx;
    return true;
}

static bool graph_stdlib_embedded_path(const char *module_name, char *buf, size_t buf_size) {
    if (!module_name || !xr_get_embedded_stdlib(module_name))
        return false;
    snprintf(buf, buf_size, GRAPH_EMBEDDED_STDLIB_PREFIX "%s/%s.xr", module_name, module_name);
    return true;
}

XR_FUNC int xr_module_graph_find(const XrModuleGraph *g, const char *canonical) {
    if (!g || !canonical)
        return -1;
    void *val = xr_hashmap_get(g->id_index, canonical);
    if (!val)
        return -1;
    return (int) (intptr_t) val - 1;
}

XR_FUNC int xr_module_graph_find_source(const XrModuleGraph *g, const char *source_path) {
    if (!g || !source_path)
        return -1;
    for (int i = 0; i < g->spec_count; i++) {
        if (g->specs[i].source_path && strcmp(g->specs[i].source_path, source_path) == 0)
            return i;
    }
    return -1;
}

XR_FUNC int xr_module_graph_find_named_dependency(const XrModuleGraph *g, const char *importer_path,
                                                  const char *specifier) {
    if (!g || !importer_path || !specifier || !specifier[0])
        return -1;
    int importer = xr_module_graph_find_source(g, importer_path);
    if (importer < 0)
        importer = xr_module_graph_find(g, importer_path);
    if (importer < 0 || importer >= g->spec_count)
        return -1;

    const XrModuleSpec *owner = &g->specs[importer];
    int match = -1;
    size_t specifier_length = strlen(specifier);
    for (int edge = 0; edge < owner->dep_count; edge++) {
        int dependency = owner->dep_indices[edge];
        if (dependency < 0 || dependency >= g->spec_count)
            return -1;
        const XrModuleSpec *candidate = &g->specs[dependency];
        const char *coordinate = candidate->authority.namespace_id;
        bool matches =
            candidate->kind == XR_MOD_STDLIB && coordinate && strcmp(coordinate, specifier) == 0;
        if (!matches && candidate->kind == XR_MOD_PACKAGE && coordinate) {
            size_t coordinate_length = strlen(coordinate);
            matches = coordinate_length > specifier_length + 1u &&
                      strncmp(coordinate, specifier, specifier_length) == 0 &&
                      coordinate[specifier_length] == '@' &&
                      xr_semver_is_valid(coordinate + specifier_length + 1u);
        }
        if (!matches)
            continue;
        XrModuleIdentityKind identity_kind = 0;
        char *expected_canonical = NULL;
        bool canonical_matches =
            candidate->logical_path &&
            xr_module_identity_from_logical(&candidate->authority, candidate->logical_path,
                                            &expected_canonical) &&
            candidate->canonical && strcmp(expected_canonical, candidate->canonical) == 0;
        xr_free(expected_canonical);
        if (!canonical_matches || !xr_module_identity_authority_valid(&candidate->authority) ||
            !xr_module_identity_valid(candidate->canonical, &identity_kind) ||
            (candidate->kind == XR_MOD_STDLIB &&
             (candidate->authority.kind != XR_MODULE_IDENTITY_STDLIB ||
              identity_kind != XR_MODULE_IDENTITY_STDLIB)) ||
            (candidate->kind == XR_MOD_PACKAGE &&
             (candidate->authority.kind != XR_MODULE_IDENTITY_PACKAGE ||
              identity_kind != XR_MODULE_IDENTITY_PACKAGE)))
            return -1;
        if (match >= 0)
            return -1;
        match = dependency;
    }
    return match;
}

/* ========== BFS Build ========== */

static char *make_unresolved_message(const char *specifier) {
    size_t n = strlen(specifier) + 40;
    char *msg = (char *) xr_malloc(n);
    if (msg)
        snprintf(msg, n, "module '%s' not found", specifier);
    return msg;
}

static void graph_resolve_and_add_dep(XrModuleGraph *g, int spec_idx, const char *specifier) {
    if (!g)
        return;
    if (spec_idx < 0 || spec_idx >= g->spec_count || !specifier) {
        g->resolution_failed = true;
        return;
    }

    XrModuleSpec *from_spec = &g->specs[spec_idx];
    if (!xr_module_identity_authority_valid(&from_spec->authority)) {
        g->resolution_failed = true;
        if (!g->unresolved_error)
            g->unresolved_error = xr_strdup("importer module identity authority is invalid");
        return;
    }
    XrModuleId mid;
    char *err = NULL;
    int rc = xr_module_resolver_resolve(g->resolver, specifier, from_spec->source_path,
                                        &from_spec->authority, &mid, &err);
    if (rc != 0) {
        g->resolution_failed = true;
        /* A registered native module resolves with a NULL source_path rather
         * than failing, so reaching here means the specifier names nothing. */
        if (!g->unresolved_error)
            g->unresolved_error = err ? err : make_unresolved_message(specifier);
        else
            xr_free(err);
        return;
    }
    xr_free(err);

    /* Statically linked core/official modules can carry an embedded script
     * layer even when no development source tree is present. */
    if (!mid.source_path && (mid.kind == XR_MOD_STDLIB || mid.kind == XR_MOD_PACKAGE)) {
        char embedded_path[XR_PATH_MAX];
        if (graph_stdlib_embedded_path(mid.authority.namespace_id, embedded_path,
                                       sizeof(embedded_path))) {
            mid.source_path = xr_strdup(embedded_path);
            if (!mid.source_path) {
                g->resolution_failed = true;
                xr_module_id_cleanup(&mid);
                return;
            }
        } else {
            xr_module_id_cleanup(&mid);
            return;
        }
    }

    /* Find or create target spec in the graph. */
    if (!xr_module_identity_authority_valid(&mid.authority)) {
        g->resolution_failed = true;
        if (!g->unresolved_error)
            g->unresolved_error = xr_strdup("resolved module identity authority is invalid");
        xr_module_id_cleanup(&mid);
        return;
    }
    int target_idx = xr_module_graph_find(g, mid.canonical);
    bool newly_discovered = target_idx < 0;
    if (target_idx < 0) {
        target_idx = graph_add_spec(g, mid.canonical, mid.logical_path, mid.source_path, mid.kind,
                                    &mid.authority);
        if (target_idx >= 0 && mid.source_path &&
            strncmp(mid.source_path, GRAPH_EMBEDDED_STDLIB_PREFIX,
                    strlen(GRAPH_EMBEDDED_STDLIB_PREFIX)) == 0) {
            g->specs[target_idx].embedded_source = true;
        }
    }
    if (target_idx >= 0 && !newly_discovered &&
        (g->specs[target_idx].representation != mid.representation ||
         (mid.representation == XR_MODULE_CHECKED_LIBRARY && g->specs[target_idx].resource != mid.resource))) {
        g->resolution_failed = true;
        if (!g->unresolved_error) g->unresolved_error = xr_strdup("module identity representation conflicts with an existing graph node");
        xr_module_id_cleanup(&mid); return;
    }
    if (target_idx >= 0 && mid.representation == XR_MODULE_CHECKED_LIBRARY) {
        if (!g->admit_checked_resources || !mid.resource ||
            (g->specs[target_idx].resource && g->specs[target_idx].resource != mid.resource)) {
            g->resolution_failed = true;
            if (!g->unresolved_error) g->unresolved_error = xr_strdup("Checked resource is not admitted by this graph consumer");
            xr_module_id_cleanup(&mid); return;
        }
        g->specs[target_idx].representation = mid.representation;
        g->specs[target_idx].resource = mid.resource;
    }
    xr_module_id_cleanup(&mid);

    if (target_idx < 0) {
        g->resolution_failed = true;
        return;
    }
    if (target_idx >= 0) {
        /* Re-fetch from_spec pointer since realloc may have moved it. */
        from_spec = &g->specs[spec_idx];
        if (!spec_add_dep(from_spec, target_idx))
            g->resolution_failed = true;
    }
}

/* Collect import and re-export specifiers from an AST program node and resolve each.
 * For every resolved module, ensure it exists in the graph and add an edge. */
static void collect_and_resolve_imports(XrModuleGraph *g, int spec_idx, struct AstNode *ast) {
    XR_DCHECK(ast != NULL, "collect_and_resolve_imports: NULL ast");
    XrModuleSpec *from_spec = &g->specs[spec_idx];

    /* Walk top-level statements looking for AST_IMPORT_STMT */
    if (ast->type != AST_PROGRAM)
        return;

    for (int i = 0; i < ast->as.program.count; i++) {
        struct AstNode *stmt = ast->as.program.statements[i];
        if (!stmt)
            continue;

        if (stmt->type == AST_IMPORT_STMT) {
            graph_resolve_and_add_dep(g, spec_idx, stmt->as.import_stmt.module_name);
            continue;
        }
        if (stmt->type == AST_EXPORT_STMT && stmt->as.export_stmt.from_path) {
            graph_resolve_and_add_dep(g, spec_idx, stmt->as.export_stmt.from_path);
            continue;
        }
    }

    from_spec = &g->specs[spec_idx];
    from_spec->status = XR_MODSPEC_RESOLVED;
}

typedef struct GraphSourceRoot {
    const char *canonical, *logical_path, *source_path;
    XrModuleKind kind;
    const XrModuleIdentityAuthority *authority;
    const char *source;
} GraphSourceRoot;

static bool graph_optional_equal(const char *a, const char *b) {
    return a && b ? strcmp(a, b) == 0 : a == b;
}

static void graph_invalidate_order(XrModuleGraph *g) {
    xr_free(g->topo_order); g->topo_order = NULL; g->topo_count = 0;
    xr_free(g->cycle_desc); g->cycle_desc = NULL; g->has_cycle = false;
    for (int i = 0; i < g->spec_count; ++i) {
        g->specs[i].topo_index = -1;
        g->specs[i].scc_id = -1;
    }
}

static int graph_expand(XrModuleGraph *g, const GraphSourceRoot *root, char **out_err) {
    XR_DCHECK(g != NULL, "xr_module_graph_build: NULL graph");
    if (!g || !root || !root->canonical) {
        if (out_err)
            *out_err = xr_strdup("NULL graph or entry");
        return -1;
    }

    int entry_idx = xr_module_graph_find(g, root->canonical);
    if (entry_idx >= 0) {
        const XrModuleSpec *found = &g->specs[entry_idx];
        if (found->kind != root->kind || found->authority.kind != root->authority->kind ||
            !graph_optional_equal(found->source_path, root->source_path) ||
            !graph_optional_equal(found->authority.namespace_id, root->authority->namespace_id) ||
            !graph_optional_equal(found->authority.physical_root, root->authority->physical_root)) {
            if (out_err) *out_err = xr_strdup("module identity has conflicting source authority");
            return -1;
        }
        if (found->status >= XR_MODSPEC_RESOLVED) return 0;
    }
    graph_invalidate_order(g);
    if (entry_idx < 0)
        entry_idx = graph_add_spec(g, root->canonical, root->logical_path, root->source_path,
                                   root->kind, root->authority);
    if (entry_idx < 0) {
        if (out_err)
            *out_err = xr_strdup("failed to add entry module");
        return -1;
    }
    if (g->entry_index < 0) g->entry_index = entry_idx;

    /* BFS: process each spec in queue order.
     * spec_count grows as new modules are discovered. */
    for (int qi = 0; qi < g->spec_count; qi++) {
        XrModuleSpec *spec = &g->specs[qi];

        if (spec->representation == XR_MODULE_CHECKED_LIBRARY) {
            if (!g->admit_checked_resources || !spec->resource || !spec->resource->checked) {
                if (out_err) *out_err = xr_strdup("Checked graph resource is invalid");
                return -1;
            }
            spec->status = XR_MODSPEC_RESOLVED;
            continue;
        }
        /* Skip stdlib native (no source to parse).  The in-memory entry is
         * the only source-less spec that still has source text. */
        bool is_supplied_entry = (qi == entry_idx && root->source != NULL);
        if (!spec->source_path && !is_supplied_entry)
            continue;

        /* Skip already processed */
        if (spec->status >= XR_MODSPEC_RESOLVED)
            continue;

        /* Parse source */
        char *owned_source = NULL;
        const char *source = root->source;
        if (!is_supplied_entry && spec->embedded_source) {
            source = xr_get_embedded_stdlib(spec->authority.namespace_id);
            if (!source) {
                xr_log_warning("module_graph", "embedded stdlib source missing: %s",
                               spec->authority.namespace_id ? spec->authority.namespace_id : "?");
                if (out_err) {
                    char buf[1024];
                    snprintf(buf, sizeof(buf), "embedded stdlib source missing: %s",
                             spec->authority.namespace_id ? spec->authority.namespace_id : "?");
                    *out_err = xr_strdup(buf);
                }
                return -1;
            }
        } else if (!is_supplied_entry) {
            XrFileBytes input = {0};
            XrFileReadStatus read_status = xr_file_read_under_root(spec->authority.physical_root,
                spec->logical_path, SIZE_MAX - 1, &input);
            if (read_status == XR_FILE_READ_OK && !memchr(input.data, 0, input.size))
                owned_source = input.data;
            else
                xr_free(input.data);
            source = owned_source;
            if (!source) {
                xr_log_warning("module_graph", "cannot read: %s", spec->source_path);
                if (out_err) {
                    char buf[1024];
                    snprintf(buf, sizeof(buf), "cannot read module: %s", spec->source_path);
                    *out_err = xr_strdup(buf);
                }
                return -1;
            }
        }

        struct AstNode *ast = xr_parse_with_source(g->compiler_session, source, spec->source_path);
        xr_module_source_fingerprint(source, &spec->source_content_fingerprint);
        xr_free(owned_source);

        if (!ast) {
            const char *failed_path = spec->source_path ? spec->source_path : root->canonical;
            xr_log_warning("module_graph", "parse failed: %s", failed_path);
            if (out_err) {
                char buf[1024];
                snprintf(buf, sizeof(buf), "failed to parse module: %s", failed_path);
                *out_err = xr_strdup(buf);
            }
            return -1;
        }

        spec->ast = ast;
        spec->status = XR_MODSPEC_PARSED;

        /* Resolve imports and discover new modules */
        collect_and_resolve_imports(g, qi, ast);
        if (g->resolution_failed) {
            if (out_err) {
                *out_err = g->unresolved_error;
                g->unresolved_error = NULL;
            }
            return -1;
        }
    }

    return 0;
}

static int graph_include_path(XrModuleGraph *g, const char *entry_path,
                                  const XrModuleIdentityAuthority *entry_authority,
                                  char **out_err) {
    if (!entry_path || !entry_authority) {
        if (out_err)
            *out_err = xr_strdup(!entry_path ? "NULL entry_path"
                                             : "entry module identity authority is required");
        return -1;
    }

    /* Canonicalize entry path */
    char *abs_path = xr_realpath(entry_path);
    if (!abs_path) {
        if (!xr_fs_exists(entry_path)) {
            if (out_err) {
                char buf[512];
                snprintf(buf, sizeof(buf), "entry file not found: %s", entry_path);
                *out_err = xr_strdup(buf);
            }
            return -1;
        }
        abs_path = xr_strdup(entry_path);
    }

    char *entry_identity = NULL;
    char *entry_logical_path = NULL;
    if (!xr_module_identity_from_source(entry_authority, abs_path, &entry_identity,
                                        &entry_logical_path)) {
        if (out_err)
            *out_err = xr_strdup("entry module escapes or lacks its identity authority");
        xr_free(abs_path);
        return -1;
    }
    XrModuleKind entry_kind =
        entry_authority->kind == XR_MODULE_IDENTITY_STDLIB
            ? XR_MOD_STDLIB
            : (entry_authority->kind == XR_MODULE_IDENTITY_PACKAGE ? XR_MOD_PACKAGE : XR_MOD_FILE);
    GraphSourceRoot root = {entry_identity, entry_logical_path, abs_path, entry_kind, entry_authority, NULL};
    int rc = graph_expand(g, &root, out_err);
    xr_free(entry_identity);
    xr_free(entry_logical_path);
    xr_free(abs_path);
    return rc;
}

XR_FUNC int xr_module_graph_build(XrModuleGraph *g, const char *entry_path,
                                  const XrModuleIdentityAuthority *entry_authority, char **out_err) {
    if (!g || g->spec_count) {
        if (out_err) *out_err = xr_strdup("entry build requires an empty graph");
        return -1;
    }
    return graph_include_path(g, entry_path, entry_authority, out_err);
}

XR_FUNC int xr_module_graph_include(XrModuleGraph *g, const char *source_path,
                                    const XrModuleIdentityAuthority *authority, char **out_err) {
    if (!g || g->entry_index < 0) {
        if (out_err) *out_err = xr_strdup("additional source requires an existing graph entry");
        return -1;
    }
    return graph_include_path(g, source_path, authority, out_err);
}

XR_FUNC int xr_module_graph_build_source(XrModuleGraph *g,
                                         const XrModuleIdentityAuthority *entry_authority,
                                         const char *entry_source, char **out_err) {
    if (!g || g->spec_count || !entry_source) {
        if (out_err)
            *out_err = xr_strdup("memory entry requires an empty graph and source");
        return -1;
    }
    char *entry_identity = NULL;
    if (!entry_authority || entry_authority->kind != XR_MODULE_IDENTITY_MEMORY ||
        !xr_module_identity_from_logical(entry_authority, NULL, &entry_identity)) {
        if (out_err)
            *out_err = xr_strdup("memory module requires an explicit valid identity");
        return -1;
    }
    GraphSourceRoot root = {entry_identity, NULL, NULL, XR_MOD_MEMORY, entry_authority, entry_source};
    int rc = graph_expand(g, &root, out_err);
    xr_free(entry_identity);
    return rc;
}

int xr_module_graph_build_logical_source(XrModuleGraph *g,
    const XrModuleIdentityAuthority *authority, const char *logical_path,
    const char *source_path, const char *source, char **out_err) {
    if (out_err)
        *out_err = NULL;
    char *identity = NULL, *physical_identity = NULL, *physical_logical = NULL;
    if (!g || g->spec_count || !source ||
        !xr_module_identity_from_logical(authority, logical_path, &identity)) {
        if (out_err) *out_err = xr_strdup("source entry requires an empty graph and valid authority");
        return -1;
    }
    bool valid = true;
    if (authority->kind == XR_MODULE_IDENTITY_MEMORY)
        valid = source_path == NULL;
    else if (authority->physical_root)
        valid = source_path && xr_module_identity_from_source(authority, source_path,
                    &physical_identity, &physical_logical) &&
                strcmp(identity, physical_identity) == 0;
    else
        valid = authority->kind == XR_MODULE_IDENTITY_STDLIB;
    if (!valid) {
        if (out_err) *out_err = xr_strdup("source locator conflicts with its logical authority");
        xr_free(identity); xr_free(physical_identity); xr_free(physical_logical);
        return -1;
    }
    XrModuleKind kind = authority->kind == XR_MODULE_IDENTITY_MEMORY ? XR_MOD_MEMORY :
        authority->kind == XR_MODULE_IDENTITY_STDLIB ? XR_MOD_STDLIB :
        authority->kind == XR_MODULE_IDENTITY_PACKAGE ? XR_MOD_PACKAGE : XR_MOD_FILE;
    GraphSourceRoot entry = {identity, logical_path, source_path, kind, authority, source};
    int result = graph_expand(g, &entry, out_err);
    xr_free(identity); xr_free(physical_identity); xr_free(physical_logical);
    return result;
}

/* ========== Topological Sort (Tarjan SCC) ========== */

typedef struct {
    int index;
    int lowlink;
    bool on_stack;
} GraphTarjanNode;

typedef struct {
    XrModuleGraph *graph;
    GraphTarjanNode *nodes;
    int *stack;
    int stack_top;
    int next_index;
    int next_scc;
    int *scc_sizes; /* Track SCC sizes for cycle detection */
    int scc_cap;
} GraphTarjanCtx;

static void graph_tarjan_strongconnect(GraphTarjanCtx *tc, int v) {
    XR_DCHECK(v >= 0 && v < tc->graph->spec_count, "tarjan: bad index");
    GraphTarjanNode *vn = &tc->nodes[v];
    vn->index = tc->next_index;
    vn->lowlink = tc->next_index;
    tc->next_index++;
    tc->stack[tc->stack_top++] = v;
    vn->on_stack = true;

    /* Visit dependencies */
    XrModuleSpec *spec = &tc->graph->specs[v];
    for (int i = 0; i < spec->dep_count; i++) {
        int w = spec->dep_indices[i];
        XR_DCHECK(w >= 0 && w < tc->graph->spec_count, "tarjan: bad dep index");
        if (tc->nodes[w].index < 0) {
            graph_tarjan_strongconnect(tc, w);
            if (tc->nodes[w].lowlink < vn->lowlink)
                vn->lowlink = tc->nodes[w].lowlink;
        } else if (tc->nodes[w].on_stack) {
            if (tc->nodes[w].index < vn->lowlink)
                vn->lowlink = tc->nodes[w].index;
        }
    }

    /* Root of an SCC */
    if (vn->lowlink == vn->index) {
        int scc_id = tc->next_scc++;

        XR_DCHECK(scc_id < tc->scc_cap, "tarjan: SCC storage is bounded by node count");

        int scc_size = 0;
        int w;
        do {
            XR_DCHECK(tc->stack_top > 0, "tarjan: stack underflow");
            w = tc->stack[--tc->stack_top];
            tc->nodes[w].on_stack = false;
            tc->graph->specs[w].scc_id = scc_id;
            scc_size++;
        } while (w != v);

        if (scc_id < tc->scc_cap)
            tc->scc_sizes[scc_id] = scc_size;
    }
}

/* The authority-root-relative logical path is the short, stable name a reader
 * recognizes in a cycle. Canonical used to be an absolute path, so taking its
 * basename gave the same thing; a typed identity carries length framing and no
 * path separator, so that heuristic now prints the whole machine-facing string
 * back at the user. An in-memory module has no logical path and keeps it. */
static const char *cycle_display_name(const XrModuleSpec *spec) {
    if (spec && spec->logical_path && spec->logical_path[0])
        return spec->logical_path;
    const char *name = (spec && spec->canonical) ? spec->canonical : "?";
    const char *slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
}

static char *format_cycle_desc(XrModuleGraph *g, const int *path, int path_len) {
    size_t len = strlen("E0504: circular dependency: ") + 1;
    for (int i = 0; i < path_len; i++) {
        len += strlen(cycle_display_name(&g->specs[path[i]]));
        if (i + 1 < path_len)
            len += strlen(" -> ");
    }

    char *buf = xr_malloc(len);
    if (!buf)
        return NULL;

    size_t pos = 0;
    pos += (size_t) snprintf(buf + pos, len - pos, "E0504: circular dependency: ");
    for (int i = 0; i < path_len; i++) {
        if (i > 0)
            pos += (size_t) snprintf(buf + pos, len - pos, " -> ");
        pos +=
            (size_t) snprintf(buf + pos, len - pos, "%s", cycle_display_name(&g->specs[path[i]]));
    }
    return buf;
}

static bool find_cycle_path_dfs(XrModuleGraph *g, int scc_id, int start, int cur, bool *seen,
                                int *path, int depth, int *out_len) {
    seen[cur] = true;
    path[depth] = cur;

    XrModuleSpec *spec = &g->specs[cur];
    for (int i = 0; i < spec->dep_count; i++) {
        int next = spec->dep_indices[i];
        if (next < 0 || next >= g->spec_count || g->specs[next].scc_id != scc_id)
            continue;
        if (next == start && depth >= 1) {
            path[depth + 1] = start;
            *out_len = depth + 2;
            return true;
        }
        if (!seen[next] &&
            find_cycle_path_dfs(g, scc_id, start, next, seen, path, depth + 1, out_len)) {
            return true;
        }
    }

    seen[cur] = false;
    return false;
}

/* Build a human-readable cycle description from the first cyclic SCC or self-loop. */
static char *build_cycle_desc(XrModuleGraph *g, int *scc_sizes, int nscc) {
    for (int s = 0; s < nscc; s++) {
        if (scc_sizes[s] <= 1)
            continue;

        int n = g->spec_count;
        bool *seen = xr_calloc((size_t) n, sizeof(bool));
        int *path = xr_calloc((size_t) (n + 1), sizeof(int));
        if (!seen || !path) {
            xr_free(seen);
            xr_free(path);
            return xr_strdup("E0504: circular dependency detected");
        }

        for (int i = 0; i < n; i++) {
            if (g->specs[i].scc_id != s)
                continue;
            memset(seen, 0, (size_t) n * sizeof(bool));
            int path_len = 0;
            if (find_cycle_path_dfs(g, s, i, i, seen, path, 0, &path_len)) {
                char *desc = format_cycle_desc(g, path, path_len);
                xr_free(seen);
                xr_free(path);
                return desc;
            }
        }

        xr_free(seen);
        xr_free(path);
    }

    for (int i = 0; i < g->spec_count; i++) {
        XrModuleSpec *spec = &g->specs[i];
        for (int d = 0; d < spec->dep_count; d++) {
            if (spec->dep_indices[d] == i) {
                int path[2] = {i, i};
                return format_cycle_desc(g, path, 2);
            }
        }
    }

    return NULL;
}

XR_FUNC int xr_module_graph_topological_sort(XrModuleGraph *g) {
    XR_DCHECK(g != NULL, "xr_module_graph_topological_sort: NULL graph");
    if (!g || g->spec_count == 0)
        return 0;

    int n = g->spec_count;

    GraphTarjanNode *nodes = xr_calloc(n, sizeof(GraphTarjanNode));
    int *stack = xr_calloc(n, sizeof(int));
    int *scc_sizes = xr_calloc(n, sizeof(int));
    int *order = xr_calloc(n, sizeof(int));
    if (!nodes || !stack || !scc_sizes || !order) {
        xr_free(nodes);
        xr_free(stack);
        xr_free(scc_sizes);
        xr_free(order);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        nodes[i].index = -1;
        nodes[i].lowlink = -1;
        nodes[i].on_stack = false;
    }

    GraphTarjanCtx tc = {
        .graph = g,
        .nodes = nodes,
        .stack = stack,
        .stack_top = 0,
        .next_index = 0,
        .next_scc = 0,
        .scc_sizes = scc_sizes,
        .scc_cap = n,
    };

    for (int i = 0; i < n; i++) {
        if (nodes[i].index < 0)
            graph_tarjan_strongconnect(&tc, i);
    }

    /* Check for cycles: SCC with size > 1, or size == 1 with self-edge */
    g->has_cycle = false;
    for (int s = 0; s < tc.next_scc; s++) {
        if (s < tc.scc_cap && tc.scc_sizes[s] > 1) {
            g->has_cycle = true;
            break;
        }
    }
    /* Also check for self-loops (SCC size==1 but node depends on itself) */
    if (!g->has_cycle) {
        for (int i = 0; i < n; i++) {
            XrModuleSpec *spec = &g->specs[i];
            for (int d = 0; d < spec->dep_count; d++) {
                if (spec->dep_indices[d] == i) {
                    g->has_cycle = true;
                    break;
                }
            }
            if (g->has_cycle)
                break;
        }
    }

    if (g->has_cycle) {
        xr_free(g->cycle_desc);
        g->cycle_desc = build_cycle_desc(g, tc.scc_sizes, tc.next_scc);
    }

    /* Tarjan emits leaf SCCs first. Publish a complete order only after all
     * required storage has been admitted; an OOM cannot erase cycle evidence. */
    int pos = 0;
    for (int scc = 0; scc < tc.next_scc && pos < n; scc++) {
        for (int i = 0; i < n; i++) {
            if (g->specs[i].scc_id == scc) {
                order[pos] = i;
                g->specs[i].topo_index = pos;
                pos++;
            }
        }
    }
    XR_DCHECK(pos == n, "tarjan: every graph node must be ordered");
    xr_free(g->topo_order);
    g->topo_order = order;
    g->topo_count = pos;

    xr_free(nodes);
    xr_free(stack);
    xr_free(tc.scc_sizes);

    return g->has_cycle ? -1 : 0;
}

const char *xr_module_spec_import_name(const XrModuleSpec *spec) {
    if (!spec)
        return NULL;
    if (spec->kind == XR_MOD_STDLIB)
        return spec->authority.namespace_id;
    return spec->source_path;
}

bool xr_module_graph_preload(XrVMRuntime *X, const XrModuleGraph *g, XrModule ***out_table) {
    if (out_table)
        *out_table = NULL;
    if (!X || !g || !out_table || g->topo_count <= 0 || !g->topo_order)
        return false;

    XrModule **table = xr_calloc((size_t) g->topo_count, sizeof(XrModule *));
    if (!table)
        return false;

    for (int ti = 0; ti < g->topo_count; ti++) {
        int idx = g->topo_order[ti];
        if (idx == g->entry_index)
            continue;
        const XrModuleSpec *spec = &g->specs[idx];
        if (!spec->source_path)
            continue;
        const char *import_name = xr_module_spec_import_name(spec);
        if (!import_name) {
            xr_free(table);
            return false;
        }
        XrValue value = xr_module_import(X, import_name);
        if (XR_IS_NULL(value)) {
            xr_free(table);
            return false;
        }
        table[ti] = xr_value_to_module(value);
        if (!table[ti]) {
            xr_free(table);
            return false;
        }
    }

    *out_table = table;
    return true;
}
