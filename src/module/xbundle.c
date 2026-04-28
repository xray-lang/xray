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
#include "../base/xlog.h"
#include "../base/xchecks.h"
#include "../base/xfileio.h"
#include "xbytecode_io.h"
#include "../runtime/xisolate_api.h"
#include "../base/xmalloc.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../base/xhashmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../os/os_fs.h"

// xr_parse_with_source declared in xparse.h (included above)
// xr_compile_ast_with_source declared in xisolate_internal.h (included above)
// xr_program_destroy declared in xast.h (included via xast.h)

/* ========== Internal Structures ========== */

typedef struct {
    XrayIsolate *X;
    XrBundle *bundle;
    XrHashMap *visited;
    char *base_dir;
    XrBundleFlags flags;
} BundleContext;

/* ========== Helper Functions ========== */

static char *resolve_module_path(const char *base_dir, const char *module_name) {
    XR_DCHECK(base_dir != NULL, "resolve_module_path: NULL base_dir");
    XR_DCHECK(module_name != NULL, "resolve_module_path: NULL module_name");
    char path[XR_PATH_MAX];

    // Absolute path
    if (module_name[0] == '/') {
        return xr_strdup(module_name);
    }

    // Relative path
    if (strncmp(module_name, "./", 2) == 0 || strncmp(module_name, "../", 3) == 0) {
        // Check if .xr extension exists
        const char *ext = strrchr(module_name, '.');
        if (ext && strcmp(ext, ".xr") == 0) {
            snprintf(path, sizeof(path), "%s/%s", base_dir, module_name);
        } else {
            snprintf(path, sizeof(path), "%s/%s.xr", base_dir, module_name);
        }

        // Check if file exists
        FILE *f = fopen(path, "r");
        if (f) {
            fclose(f);
            char *real = xr_realpath(path);
            return real ? real : xr_strdup(path);
        }

        // Try directory entry
        snprintf(path, sizeof(path), "%s/%s/index.xr", base_dir, module_name);
        f = fopen(path, "r");
        if (f) {
            fclose(f);
            char *real = xr_realpath(path);
            return real ? real : xr_strdup(path);
        }
    }

    // Stdlib or package: not bundled (loaded at runtime)
    return NULL;
}

// Resolve third-party package path: owner/name -> ~/.xray/packages/owner/name/latest/main.xr
static char *resolve_package_path(const char *package_name) {
    const char *home = getenv("HOME");
    if (!home)
        return NULL;

    char owner[64], name[64];
    if (sscanf(package_name, "%63[^/]/%63s", owner, name) != 2) {
        return NULL;
    }

    char path[512];

    // Try latest/src/main.xr
    snprintf(path, sizeof(path), "%s/.xray/packages/%s/%s/latest/src/main.xr", home, owner, name);
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        char *real = xr_realpath(path);
        return real ? real : xr_strdup(path);
    }

    // Try latest/main.xr
    snprintf(path, sizeof(path), "%s/.xray/packages/%s/%s/latest/main.xr", home, owner, name);
    f = fopen(path, "r");
    if (f) {
        fclose(f);
        char *real = xr_realpath(path);
        return real ? real : xr_strdup(path);
    }

    return NULL;
}

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
        ImportType import_type = node->as.import_stmt.import_type;

        // Collect stdlib dependency
        if (import_type == IMPORT_STDLIB) {
            add_external_dep(&ctx->bundle->stdlib, module_name);
            return;
        }

        // Collect third-party package dependency
        if (import_type == IMPORT_PACKAGE) {
            // Static bundle mode: try to bundle third-party package
            if (ctx->flags & XR_BUNDLE_STATIC_PACKAGES) {
                char *pkg_path = resolve_package_path(module_name);
                if (pkg_path) {
                    if (!xr_hashmap_has(ctx->visited, pkg_path)) {
                        xr_hashmap_set(ctx->visited, pkg_path, (void *) 1);

                        char *source = xr_file_read_all(pkg_path, "r", NULL);
                        if (source) {
                            AstNode *ast = xr_parse_with_source(ctx->X, source, pkg_path);
                            if (ast) {
                                char *pkg_dir = xr_path_dirname(pkg_path);
                                collect_imports_from_ast(ctx, ast, pkg_dir);

                                XrProto *proto = xr_compile_ast_with_source(ctx->X, ast, pkg_path);
                                if (proto) {
                                    size_t bc_size;
                                    uint8_t *bc = xr_bytecode_write(ctx->X, proto, 0, &bc_size);
                                    if (bc) {
                                        bundle_add_entry(ctx->bundle, pkg_path, bc, bc_size);
                                        xr_free(bc);
                                    }
                                }
                                xr_program_destroy(ast);
                                xr_free(pkg_dir);
                            }
                            xr_free(source);
                        }
                    }
                    xr_free(pkg_path);
                    return;
                } else {
                    xr_log_warning("bundle", "package '%s' not installed, cannot bundle statically",
                                   module_name);
                }
            }
            add_external_dep(&ctx->bundle->packages, module_name);
            return;
        }

        // Only process file and directory imports
        if (import_type == IMPORT_FILE || import_type == IMPORT_DIR) {
            char *resolved = resolve_module_path(current_dir, module_name);
            if (resolved) {
                // Check if already processed
                if (!xr_hashmap_has(ctx->visited, resolved)) {
                    xr_hashmap_set(ctx->visited, resolved, (void *) 1);

                    // Recursively process dependencies
                    char *source = xr_file_read_all(resolved, "r", NULL);
                    if (source) {
                        AstNode *ast = xr_parse_with_source(ctx->X, source, resolved);
                        if (ast) {
                            // Get module directory
                            char *module_dir = xr_path_dirname(resolved);

                            // Recursively collect dependencies
                            collect_imports_from_ast(ctx, ast, module_dir);

                            // Compile and add to bundle
                            XrProto *proto = xr_compile_ast_with_source(ctx->X, ast, resolved);
                            if (proto) {
                                size_t bc_size;
                                uint8_t *bc = xr_bytecode_write(ctx->X, proto, 0, &bc_size);
                                if (bc) {
                                    bundle_add_entry(ctx->bundle, resolved, bc, bc_size);
                                    xr_free(bc);
                                } else {
                                    xr_log_warning("bundle", "bytecode serialization failed: %s",
                                                   resolved);
                                }
                            } else {
                                xr_log_warning("bundle", "compilation failed: %s", resolved);
                            }

                            xr_program_destroy(ast);
                            xr_free(module_dir);
                        } else {
                            xr_log_warning("bundle", "parse failed: %s", resolved);
                        }
                        xr_free(source);
                    }
                }
                xr_free(resolved);
            }
        }
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
            break;

        case AST_TRY_CATCH:
            visit_node(ctx, node->as.try_catch.try_body, current_dir);
            visit_node(ctx, node->as.try_catch.catch_body, current_dir);
            visit_node(ctx, node->as.try_catch.finally_body, current_dir);
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

XrBundle *xr_bundle_create(XrayIsolate *X, const char *entry_file) {
    return xr_bundle_create_ex(X, entry_file, XR_BUNDLE_DEFAULT);
}

XrBundle *xr_bundle_create_ex(XrayIsolate *X, const char *entry_file, XrBundleFlags flags) {
    XR_DCHECK(X != NULL, "bundle_create_ex: NULL isolate");
    XR_DCHECK(entry_file != NULL, "bundle_create_ex: NULL entry_file");
    if (!X || !entry_file)
        return NULL;

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

    // Create context
    BundleContext ctx = {.X = X,
                         .bundle = bundle,
                         .visited = xr_hashmap_new(),
                         .base_dir = xr_path_dirname(abs_path),
                         .flags = flags};

    // Mark entry file as visited
    xr_hashmap_set(ctx.visited, abs_path, (void *) 1);

    // Parse entry file
    AstNode *ast = xr_parse_with_source(X, source, abs_path);
    xr_free(source);

    if (!ast) {
        xr_log_warning("bundle", "failed to parse entry file: %s", entry_file);
        xr_hashmap_free(ctx.visited);
        xr_free(ctx.base_dir);
        xr_free(abs_path);
        xr_bundle_free(bundle);
        return NULL;
    }

    // Collect all dependencies
    collect_imports_from_ast(&ctx, ast, ctx.base_dir);

    // Compile entry file and add to bundle (placed last to ensure dependencies come first)
    XrProto *proto = xr_compile_ast_with_source(X, ast, abs_path);
    if (proto) {
        size_t bc_size;
        uint8_t *bc = xr_bytecode_write(X, proto, 0, &bc_size);
        if (bc) {
            bundle_add_entry(bundle, abs_path, bc, bc_size);
            xr_free(bc);
        }
    }

    xr_program_destroy(ast);
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
    char *p = output;
    char *end = output + buf_size;

#define EMIT(...)                                                                                  \
    do {                                                                                           \
        int _n = snprintf(p, (size_t) (end - p), __VA_ARGS__);                                     \
        if (_n > 0)                                                                                \
            p += _n;                                                                               \
    } while (0)

    // Header
    EMIT("/* Auto-generated by xray build */\n\n");
    EMIT("#include <stdint.h>\n");
    EMIT("#include <stddef.h>\n\n");

    // Bytecode for each module
    for (int i = 0; i < bundle->count; i++) {
        const XrBundleEntry *e = &bundle->entries[i];
        EMIT("/* Module: %s */\n", e->path);
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
        EMIT("    {\"%s\", %s_mod%d_bc, %zu},\n", e->path, prefix, i, e->bc_size);
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

    return output;
}
