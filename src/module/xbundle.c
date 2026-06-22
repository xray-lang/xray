/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbundle.c - Multi-file bundling implementation
 *
 * KEY CONCEPT:
 *   Recursively analyzes imports, compiles dependencies,
 *   and bundles them into a single bytecode package.
 */

#include "xbundle.h"
#include "xmodule.h"
#include "xmodule_resolver.h"
#include "../base/xlog.h"
#include "../base/xchecks.h"
#include "../base/xfileio.h"
#include "xbytecode_io.h"
#include "../runtime/xisolate_api.h"
#include "../base/xmalloc.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../toolchain/xcompiler_session.h"
#include "../base/xhashmap.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../os/os_fs.h"

// xr_parse_with_source declared in xparse.h (included above)
// xr_compile_ast_with_source declared in xisolate_internal.h (included above).
// xr_program_destroy declared in xast.h (included via xast.h)

/* ========== Internal Structures ========== */

typedef struct {
    XrVMRuntime *X;
    XrCompilerSession *session;
    XrBundle *bundle;
    XrHashMap *visited;
    char *base_dir;
    XrBundleFlags flags;
    XrModuleResolver *resolver;
} BundleContext;

/* ========== Helper Functions ========== */

static void bundle_add_entry(XrBundle *bundle, const char *path, const uint8_t *bc,
                             size_t bc_size) {
    if (bundle->count >= bundle->capacity) {
        int new_cap = bundle->capacity * 2;
        if (new_cap < 16)
            new_cap = 16;
        XR_REALLOC_OR_ABORT(bundle->entries, (size_t) new_cap * sizeof(XrBundleEntry),
                            "bundle entries grow");
        bundle->capacity = new_cap;
    }

    XrBundleEntry *entry = &bundle->entries[bundle->count++];
    entry->path = xr_strdup(path);
    entry->bc = xr_malloc(bc_size);
    if (!entry->bc) {
        bundle->count--;
        xr_free(entry->path);
        return;
    }
    memcpy((void *) entry->bc, bc, bc_size);
    entry->bc_size = bc_size;
}

/* ========== AST Traversal for Import Collection ========== */

static void collect_imports_from_ast(BundleContext *ctx, AstNode *node, const char *current_dir);

static void add_external_dep(XrExternalDeps *deps, const char *name) {
    // Check if already exists
    for (int i = 0; i < deps->count; i++) {
        if (strcmp(deps->deps[i], name) == 0)
            return;
    }

    if (deps->count >= deps->capacity) {
        int new_cap = deps->capacity * 2;
        if (new_cap < 8)
            new_cap = 8;
        XR_REALLOC_OR_ABORT(deps->deps, (size_t) new_cap * sizeof(char *),
                            "bundle external deps grow");
        deps->capacity = new_cap;
    }
    deps->deps[deps->count++] = xr_strdup(name);
}

static void visit_node(BundleContext *ctx, AstNode *node, const char *current_dir) {
    if (!node)
        return;

    // Check if this is an import statement
    if (node->type == AST_IMPORT_STMT) {
        const char *module_name = node->as.import_stmt.module_name;
        bool is_bare = !node->as.import_stmt.is_quoted;

        /* Build an importer path for the resolver by joining current_dir
         * with a dummy filename. The resolver only uses the dirname. */
        char importer_buf[XR_PATH_MAX];
        snprintf(importer_buf, sizeof(importer_buf), "%s/_importer_.xr", current_dir);

        XrModuleId mid;
        char *err = NULL;
        int rc = xr_module_resolver_resolve(ctx->resolver, module_name, is_bare, importer_buf, &mid,
                                            &err);
        if (rc != 0) {
            if (err) {
                xr_log_warning("bundle", "%s", err);
                xr_free(err);
            }
            /* Bare names that failed resolver lookup are still stdlib deps */
            if (is_bare)
                add_external_dep(&ctx->bundle->stdlib, module_name);
            return;
        }

        /* Route by module kind */
        switch (mid.kind) {
            case XR_MOD_STDLIB:
                add_external_dep(&ctx->bundle->stdlib, mid.canonical);
                xr_module_id_cleanup(&mid);
                return;

            case XR_MOD_PACKAGE:
                if (mid.source_path && (ctx->flags & XR_BUNDLE_STATIC_PACKAGES)) {
                    /* Static bundle: compile the package */
                    if (!xr_hashmap_has(ctx->visited, mid.source_path)) {
                        /* The visited mark is the cycle breaker; never descend
                         * into a module that could not be marked, or circular
                         * imports would recurse forever. */
                        if (!xr_hashmap_set(ctx->visited, mid.source_path, (void *) 1)) {
                            xr_log_warning("bundle", "out of memory tracking visited: %s",
                                           mid.source_path);
                            xr_module_id_cleanup(&mid);
                            return;
                        }
                        char *source = xr_file_read_all(mid.source_path, "r", NULL);
                        if (source) {
                            AstNode *ast =
                                xr_parse_with_source(ctx->session, source, mid.source_path);
                            if (ast) {
                                char *pkg_dir = xr_path_dirname(mid.source_path);
                                collect_imports_from_ast(ctx, ast, pkg_dir);
                                XrProto *proto =
                                    xr_compile_ast_with_source(ctx->session, ast, mid.source_path);
                                if (proto) {
                                    size_t bc_size;
                                    uint8_t *bc = xr_bytecode_write(ctx->X, proto, 0, &bc_size);
                                    if (bc) {
                                        bundle_add_entry(ctx->bundle, mid.source_path, bc, bc_size);
                                        xr_free(bc);
                                    }
                                }
                                xr_program_destroy(ast);
                                xr_free(pkg_dir);
                            }
                            xr_free(source);
                        }
                    }
                    xr_module_id_cleanup(&mid);
                    return;
                }
                add_external_dep(&ctx->bundle->packages, mid.canonical);
                xr_module_id_cleanup(&mid);
                return;

            case XR_MOD_FILE: {
                XR_DCHECK(mid.source_path != NULL, "visit_node: FILE module without source_path");
                if (!xr_hashmap_has(ctx->visited, mid.source_path)) {
                    /* See XR_MOD_PACKAGE above: do not descend unmarked. */
                    if (!xr_hashmap_set(ctx->visited, mid.source_path, (void *) 1)) {
                        xr_log_warning("bundle", "out of memory tracking visited: %s",
                                       mid.source_path);
                        xr_module_id_cleanup(&mid);
                        return;
                    }
                    char *source = xr_file_read_all(mid.source_path, "r", NULL);
                    if (source) {
                        AstNode *ast = xr_parse_with_source(ctx->session, source, mid.source_path);
                        if (ast) {
                            char *module_dir = xr_path_dirname(mid.source_path);
                            collect_imports_from_ast(ctx, ast, module_dir);
                            XrProto *proto =
                                xr_compile_ast_with_source(ctx->session, ast, mid.source_path);
                            if (proto) {
                                size_t bc_size;
                                uint8_t *bc = xr_bytecode_write(ctx->X, proto, 0, &bc_size);
                                if (bc) {
                                    bundle_add_entry(ctx->bundle, mid.source_path, bc, bc_size);
                                    xr_free(bc);
                                } else {
                                    xr_log_warning("bundle", "bytecode serialization failed: %s",
                                                   mid.source_path);
                                }
                            } else {
                                xr_log_warning("bundle", "compilation failed: %s", mid.source_path);
                            }
                            xr_program_destroy(ast);
                            xr_free(module_dir);
                        } else {
                            xr_log_warning("bundle", "parse failed: %s", mid.source_path);
                        }
                        xr_free(source);
                    }
                }
                xr_module_id_cleanup(&mid);
                return;
            }
        }

        xr_module_id_cleanup(&mid);
        return;
    }

    // Recursively traverse child nodes that may contain import statements
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                visit_node(ctx, node->as.program.statements[i], current_dir);
            }
            break;

        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                visit_node(ctx, node->as.block.statements[i], current_dir);
            }
            break;

        case AST_IF_STMT:
            visit_node(ctx, node->as.if_stmt.condition, current_dir);
            visit_node(ctx, node->as.if_stmt.then_branch, current_dir);
            visit_node(ctx, node->as.if_stmt.else_branch, current_dir);
            break;

        case AST_WHILE_STMT:
            visit_node(ctx, node->as.while_stmt.condition, current_dir);
            visit_node(ctx, node->as.while_stmt.body, current_dir);
            break;

        case AST_FOR_STMT:
            visit_node(ctx, node->as.for_stmt.initializer, current_dir);
            visit_node(ctx, node->as.for_stmt.condition, current_dir);
            visit_node(ctx, node->as.for_stmt.increment, current_dir);
            visit_node(ctx, node->as.for_stmt.body, current_dir);
            break;

        case AST_FOR_IN_STMT:
            visit_node(ctx, node->as.for_in_stmt.collection, current_dir);
            visit_node(ctx, node->as.for_in_stmt.body, current_dir);
            break;

        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            visit_node(ctx, node->as.function_decl.body, current_dir);
            break;

        case AST_METHOD_DECL:
            visit_node(ctx, node->as.method_decl.body, current_dir);
            break;

        case AST_CLASS_DECL:
            for (int i = 0; i < node->as.class_decl.method_count; i++) {
                visit_node(ctx, node->as.class_decl.methods[i], current_dir);
            }
            break;

        case AST_STRUCT_DECL:
            for (int i = 0; i < node->as.struct_decl.method_count; i++) {
                visit_node(ctx, node->as.struct_decl.methods[i], current_dir);
            }
            break;

        case AST_INTERFACE_DECL:
            for (int i = 0; i < node->as.interface_decl.method_count; i++) {
                visit_node(ctx, node->as.interface_decl.methods[i], current_dir);
            }
            for (int i = 0; i < node->as.interface_decl.property_count; i++) {
                visit_node(ctx, node->as.interface_decl.properties[i], current_dir);
            }
            break;

        case AST_TRY_CATCH:
            visit_node(ctx, node->as.try_catch.try_body, current_dir);
            for (int ci = 0; ci < node->as.try_catch.catch_count; ci++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[ci];
                if (cc)
                    visit_node(ctx, cc->body, current_dir);
            }
            break;

        case AST_EXPORT_STMT:
            visit_node(ctx, node->as.export_stmt.declaration, current_dir);
            break;

        case AST_EXPR_STMT:
            visit_node(ctx, node->as.expr_stmt, current_dir);
            break;

        case AST_VAR_DECL:
        case AST_CONST_DECL:
            visit_node(ctx, node->as.var_decl.initializer, current_dir);
            break;

        case AST_MATCH_EXPR:
            visit_node(ctx, node->as.match_expr.expr, current_dir);
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                visit_node(ctx, node->as.match_expr.arms[i], current_dir);
            }
            break;

        case AST_MATCH_ARM:
            visit_node(ctx, node->as.match_arm.body, current_dir);
            break;

        case AST_SCOPE_BLOCK:
            visit_node(ctx, node->as.scope_block.body, current_dir);
            break;

        case AST_SELECT_STMT:
            for (int i = 0; i < node->as.select_stmt.case_count; i++) {
                visit_node(ctx, node->as.select_stmt.cases[i], current_dir);
            }
            break;

        case AST_SELECT_CASE:
            visit_node(ctx, node->as.select_case.body, current_dir);
            break;

        case AST_DEFER_STMT:
            visit_node(ctx, node->as.defer_stmt.expr, current_dir);
            break;

        default:
            break;
    }
}

static void collect_imports_from_ast(BundleContext *ctx, AstNode *node, const char *current_dir) {
    visit_node(ctx, node, current_dir);
}

/* ========== Public API ========== */

XrBundle *xr_bundle_create(XrVMRuntime *X, const char *entry_file) {
    return xr_bundle_create_ex(X, entry_file, XR_BUNDLE_DEFAULT);
}

XrBundle *xr_bundle_create_ex(XrVMRuntime *X, const char *entry_file, XrBundleFlags flags) {
    XR_DCHECK(X != NULL, "bundle_create_ex: NULL isolate");
    XR_DCHECK(entry_file != NULL, "bundle_create_ex: NULL entry_file");
    if (!X || !entry_file)
        return NULL;

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(X);
    if (!session) {
        xr_log_warning("bundle", "compiler session is required");
        return NULL;
    }

    // Read entry file
    char *source = xr_file_read_all(entry_file, "r", NULL);
    if (!source) {
        xr_log_warning("bundle", "cannot read entry file: %s", entry_file);
        return NULL;
    }

    // Get absolute path
    char *abs_path = xr_realpath(entry_file);
    if (!abs_path) {
        abs_path = xr_strdup(entry_file);
    }

    // Create bundle result
    XrBundle *bundle = xr_calloc(1, sizeof(XrBundle));
    bundle->entry_path = xr_strdup(abs_path);

    // Create resolver for import resolution
    XrModuleRegistry *registry = xr_isolate_get_module_registry(X);
    XrModuleResolverConfig rcfg = {
        .native_loaders = registry ? registry->native_loaders : NULL,
        .stdlib_path = registry ? registry->stdlib_path : NULL,
        .lockfile = NULL,
    };
    XrModuleResolver *resolver = xr_module_resolver_new(&rcfg);

    // Create context
    BundleContext ctx = {.X = X,
                         .session = session,
                         .bundle = bundle,
                         .visited = xr_hashmap_new(),
                         .base_dir = xr_path_dirname(abs_path),
                         .flags = flags,
                         .resolver = resolver};

    // Mark entry file as visited. Without the mark a circular import back
    // into the entry would recurse forever, so treat failure as fatal.
    if (!ctx.visited || !xr_hashmap_set(ctx.visited, abs_path, (void *) 1)) {
        xr_log_warning("bundle", "out of memory creating visited set");
        xr_free(source);
        xr_module_resolver_free(ctx.resolver);
        xr_hashmap_free(ctx.visited);
        xr_free(ctx.base_dir);
        xr_free(abs_path);
        xr_bundle_free(bundle);
        return NULL;
    }

    // Parse entry file
    AstNode *ast = xr_parse_with_source(session, source, abs_path);
    xr_free(source);

    if (!ast) {
        xr_log_warning("bundle", "failed to parse entry file: %s", entry_file);
        xr_module_resolver_free(ctx.resolver);
        xr_hashmap_free(ctx.visited);
        xr_free(ctx.base_dir);
        xr_free(abs_path);
        xr_bundle_free(bundle);
        return NULL;
    }

    // Collect all dependencies
    collect_imports_from_ast(&ctx, ast, ctx.base_dir);

    // Compile entry file and add to bundle (placed last to ensure dependencies come first)
    XrProto *proto = xr_compile_ast_with_source(session, ast, abs_path);
    if (proto) {
        size_t bc_size;
        uint8_t *bc = xr_bytecode_write(X, proto, 0, &bc_size);
        if (bc) {
            bundle_add_entry(bundle, abs_path, bc, bc_size);
            xr_free(bc);
        }
    }

    xr_program_destroy(ast);
    xr_module_resolver_free(ctx.resolver);
    xr_hashmap_free(ctx.visited);
    xr_free(ctx.base_dir);
    xr_free(abs_path);

    return bundle;
}

static void free_external_deps(XrExternalDeps *deps) {
    for (int i = 0; i < deps->count; i++) {
        xr_free(deps->deps[i]);
    }
    xr_free(deps->deps);
    deps->deps = NULL;
    deps->count = 0;
    deps->capacity = 0;
}

void xr_bundle_free(XrBundle *bundle) {
    if (!bundle)
        return;

    for (int i = 0; i < bundle->count; i++) {
        xr_free((void *) bundle->entries[i].path);
        xr_free((void *) bundle->entries[i].bc);
    }
    xr_free(bundle->entries);
    xr_free((void *) bundle->entry_path);
    free_external_deps(&bundle->stdlib);
    free_external_deps(&bundle->packages);
    xr_free(bundle);
}

static bool bundle_emitf(char **buf, size_t *cap, size_t *len, const char *fmt, ...) {
    if (!buf || !*buf || !cap || !len || !fmt)
        return false;
    for (;;) {
        size_t avail = (*cap > *len) ? (*cap - *len) : 0;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(*buf + *len, avail, fmt, ap);
        va_end(ap);
        if (n < 0)
            return false;
        if ((size_t) n < avail) {
            *len += (size_t) n;
            return true;
        }
        size_t need = *len + (size_t) n + 1;
        size_t new_cap = *cap ? *cap : 4096;
        while (new_cap < need)
            new_cap *= 2;
        char *tmp = (char *) xr_realloc(*buf, new_cap);
        if (!tmp)
            return false;
        *buf = tmp;
        *cap = new_cap;
    }
}

static bool bundle_emit_c_string(char **buf, size_t *cap, size_t *len, const char *s) {
    if (!bundle_emitf(buf, cap, len, "\""))
        return false;
    for (const unsigned char *p = (const unsigned char *) (s ? s : ""); *p; p++) {
        switch (*p) {
            case '\\':
                if (!bundle_emitf(buf, cap, len, "\\\\"))
                    return false;
                break;
            case '"':
                if (!bundle_emitf(buf, cap, len, "\\\""))
                    return false;
                break;
            case '\n':
                if (!bundle_emitf(buf, cap, len, "\\n"))
                    return false;
                break;
            case '\r':
                if (!bundle_emitf(buf, cap, len, "\\r"))
                    return false;
                break;
            case '\t':
                if (!bundle_emitf(buf, cap, len, "\\t"))
                    return false;
                break;
            default:
                if (*p < 0x20 || *p == 0x7f) {
                    if (!bundle_emitf(buf, cap, len, "\\%03o", (unsigned) *p))
                        return false;
                } else if (!bundle_emitf(buf, cap, len, "%c", *p)) {
                    return false;
                }
                break;
        }
    }
    return bundle_emitf(buf, cap, len, "\"");
}

char *xr_bundle_to_c_source(XrBundle *bundle, const char *var_prefix) {
    if (!bundle || bundle->count == 0)
        return NULL;

    const char *prefix = var_prefix ? var_prefix : "xr_app";

    // Estimate output size
    size_t total_bc = 0;
    for (int i = 0; i < bundle->count; i++) {
        total_bc += bundle->entries[i].bc_size;
    }
    size_t buf_size = total_bc * 6 + bundle->count * 512 + 4096;

    char *output = xr_malloc(buf_size);
    if (!output)
        return NULL;
    size_t len = 0;

#define EMIT(...)                                                                                  \
    do {                                                                                           \
        if (!bundle_emitf(&output, &buf_size, &len, __VA_ARGS__)) {                                \
            xr_free(output);                                                                       \
            return NULL;                                                                           \
        }                                                                                          \
    } while (0)

#define EMIT_C_STRING(s)                                                                           \
    do {                                                                                           \
        if (!bundle_emit_c_string(&output, &buf_size, &len, (s))) {                                \
            xr_free(output);                                                                       \
            return NULL;                                                                           \
        }                                                                                          \
    } while (0)

    // Header
    EMIT("/* Auto-generated by xray build */\n\n");
    EMIT("#include <stdint.h>\n");
    EMIT("#include <stddef.h>\n\n");

    // Bytecode for each module
    for (int i = 0; i < bundle->count; i++) {
        const XrBundleEntry *e = &bundle->entries[i];
        EMIT("/* Module %d */\n", i);
        EMIT("static const uint8_t %s_mod%d_bc[%zu] = {\n", prefix, i, e->bc_size);

        for (size_t j = 0; j < e->bc_size; j++) {
            if (j % 12 == 0)
                EMIT("    ");
            EMIT("0x%02x", e->bc[j]);
            if (j < e->bc_size - 1)
                EMIT(",");
            if ((j + 1) % 12 == 0 || j == e->bc_size - 1)
                EMIT("\n");
            else
                EMIT(" ");
        }
        EMIT("};\n\n");
    }

    // Module table
    EMIT("/* Module table */\n");
    EMIT("typedef struct {\n");
    EMIT("    const char *path;\n");
    EMIT("    const uint8_t *bc;\n");
    EMIT("    size_t size;\n");
    EMIT("} XrEmbeddedModule;\n\n");

    EMIT("const int %s_module_count = %d;\n\n", prefix, bundle->count);

    EMIT("const XrEmbeddedModule %s_modules[%d] = {\n", prefix, bundle->count);
    for (int i = 0; i < bundle->count; i++) {
        const XrBundleEntry *e = &bundle->entries[i];
        EMIT("    {");
        EMIT_C_STRING(e->path);
        EMIT(", %s_mod%d_bc, %zu},\n", prefix, i, e->bc_size);
    }
    EMIT("};\n\n");

    // Entry module index (last one)
    EMIT("const int %s_entry_index = %d;\n\n", prefix, bundle->count - 1);

    // Lookup function
    EMIT("/* Find embedded module */\n");
    EMIT("const uint8_t* %s_find_module(const char *path, size_t *out_size) {\n", prefix);
    EMIT("    for (int i = 0; i < %s_module_count; i++) {\n", prefix);
    EMIT("        if (strcmp(%s_modules[i].path, path) == 0) {\n", prefix);
    EMIT("            if (out_size) *out_size = %s_modules[i].size;\n", prefix);
    EMIT("            return %s_modules[i].bc;\n", prefix);
    EMIT("        }\n");
    EMIT("    }\n");
    EMIT("    return NULL;\n");
    EMIT("}\n");

#undef EMIT
#undef EMIT_C_STRING

    return output;
}
