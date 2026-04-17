/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_incremental.c - Incremental analysis implementation
 */

#include "xanalyzer_incremental.h"
#include "../../base/xchecks.h"
#include "xanalyzer.h"
#include "../parser/xast.h"
#include "../../base/xmalloc.h"
#include "../../base/xhash.h"
#include <string.h>
#include <stdio.h>

#define INITIAL_BUCKET_COUNT 64

// ============================================================================
// Hash Utilities (reuse xhash.h)
// ============================================================================

uint64_t xa_hash_content(const char *content, int length) {
    if (!content || length <= 0) return 0;
    return xr_hash_bytes64(content, (size_t)length);
}

// Forward declaration for recursive hashing
static uint64_t hash_ast_node(AstNode *node, uint64_t hash);

// Hash a string into the running hash
static inline uint64_t hash_string_into(const char *str, uint64_t hash) {
    if (!str) return hash;
    while (*str) {
        hash ^= (uint64_t)(unsigned char)*str++;
        hash *= XR_FNV64_PRIME;
    }
    return hash;
}

// Hash an integer into the running hash
static inline uint64_t hash_int_into(int64_t val, uint64_t hash) {
    for (int i = 0; i < 8; i++) {
        hash ^= (val >> (i * 8)) & 0xFF;
        hash *= XR_FNV64_PRIME;
    }
    return hash;
}

// Hash a list of AST nodes
static uint64_t hash_node_list(AstNode **nodes, int count, uint64_t hash) {
    hash = hash_int_into(count, hash);
    for (int i = 0; i < count; i++) {
        hash = hash_ast_node(nodes[i], hash);
    }
    return hash;
}

// Recursive AST node hasher - captures structural changes
static uint64_t hash_ast_node(AstNode *node, uint64_t hash) {
    if (!node) {
        hash ^= 0xDEADBEEF;
        hash *= XR_FNV64_PRIME;
        return hash;
    }

    // Hash node type
    hash ^= (uint64_t)node->type;
    hash *= XR_FNV64_PRIME;

    // Hash based on node type
    switch (node->type) {
        case AST_LITERAL_INT:
            hash = hash_int_into(node->as.literal.raw_value.int_val, hash);
            break;
        case AST_LITERAL_FLOAT:
            { int64_t fbits; memcpy(&fbits, &node->as.literal.raw_value.float_val, sizeof(fbits));
              hash = hash_int_into(fbits, hash); }
            break;
        case AST_LITERAL_STRING:
            hash = hash_string_into(node->as.literal.raw_value.string_val, hash);
            break;
        case AST_VARIABLE:
            hash = hash_string_into(node->as.variable.name, hash);
            break;
        case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
        case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_EQ:
        case AST_BINARY_NE: case AST_BINARY_LT: case AST_BINARY_LE:
        case AST_BINARY_GT: case AST_BINARY_GE: case AST_BINARY_AND:
        case AST_BINARY_OR:
            hash = hash_ast_node(node->as.binary.left, hash);
            hash = hash_ast_node(node->as.binary.right, hash);
            break;
        case AST_UNARY_NEG: case AST_UNARY_NOT: case AST_UNARY_BNOT:
            hash = hash_ast_node(node->as.unary.operand, hash);
            break;
        case AST_CALL_EXPR:
            hash = hash_ast_node(node->as.call_expr.callee, hash);
            hash = hash_node_list(node->as.call_expr.arguments,
                                  node->as.call_expr.arg_count, hash);
            break;
        case AST_MEMBER_ACCESS:
            hash = hash_ast_node(node->as.member_access.object, hash);
            hash = hash_string_into(node->as.member_access.name, hash);
            break;
        case AST_VAR_DECL: case AST_CONST_DECL:
            hash = hash_string_into(node->as.var_decl.name, hash);
            hash = hash_ast_node(node->as.var_decl.initializer, hash);
            break;
        case AST_ASSIGNMENT:
            hash = hash_string_into(node->as.assignment.name, hash);
            hash = hash_ast_node(node->as.assignment.value, hash);
            break;
        case AST_BLOCK:
            hash = hash_node_list(node->as.block.statements,
                                  node->as.block.count, hash);
            break;
        case AST_IF_STMT:
            hash = hash_ast_node(node->as.if_stmt.condition, hash);
            hash = hash_ast_node(node->as.if_stmt.then_branch, hash);
            hash = hash_ast_node(node->as.if_stmt.else_branch, hash);
            break;
        case AST_WHILE_STMT:
            hash = hash_ast_node(node->as.while_stmt.condition, hash);
            hash = hash_ast_node(node->as.while_stmt.body, hash);
            break;
        case AST_FOR_STMT:
            hash = hash_ast_node(node->as.for_stmt.initializer, hash);
            hash = hash_ast_node(node->as.for_stmt.condition, hash);
            hash = hash_ast_node(node->as.for_stmt.increment, hash);
            hash = hash_ast_node(node->as.for_stmt.body, hash);
            break;
        case AST_RETURN_STMT:
            hash = hash_node_list(node->as.return_stmt.values,
                                  node->as.return_stmt.value_count, hash);
            break;
        case AST_FUNCTION_DECL:
            hash = hash_string_into(node->as.function_decl.name, hash);
            hash = hash_int_into(node->as.function_decl.param_count, hash);
            for (int i = 0; i < node->as.function_decl.param_count; i++) {
                XrParamNode *param = node->as.function_decl.params[i];
                if (param) hash = hash_string_into(param->name, hash);
            }
            hash = hash_ast_node(node->as.function_decl.body, hash);
            break;
        case AST_PROGRAM:
            hash = hash_node_list(node->as.program.statements,
                                  node->as.program.count, hash);
            break;
        default:
            // For other node types, just use type (already hashed above)
            break;
    }

    return hash;
}

// Hash an AST block by traversing its full structure
uint64_t xa_hash_ast_block(AstNode *block) {
    if (!block) return 0;
    return hash_ast_node(block, XR_FNV64_OFFSET_BASIS);
}

// ============================================================================
// Dependency Graph Implementation
// ============================================================================

static XaDependencyGraph *dep_graph_new(void) {
    XaDependencyGraph *g = xr_calloc(1, sizeof(XaDependencyGraph));
    if (!g) return NULL;

    g->bucket_count = INITIAL_BUCKET_COUNT;
    g->forward = xr_calloc(g->bucket_count, sizeof(XaDependency*));
    g->reverse = xr_calloc(g->bucket_count, sizeof(XaDependency*));

    if (!g->forward || !g->reverse) {
        xr_free(g->forward);
        xr_free(g->reverse);
        xr_free(g);
        return NULL;
    }

    return g;
}

static void dep_graph_free(XaDependencyGraph *g) {
    if (!g) return;

    // Free forward edges
    for (int i = 0; i < g->bucket_count; i++) {
        XaDependency *dep = g->forward[i];
        while (dep) {
            XaDependency *next = dep->next;
            xr_free(dep);
            dep = next;
        }
    }

    // Reverse edges point to same nodes, already freed
    xr_free(g->forward);
    xr_free(g->reverse);
    xr_free(g);
}

void xa_dep_add(XaIncrementalCtx *ctx, uint32_t from, uint32_t to, XaDepKind kind) {
    if (!ctx || !ctx->deps) return;

    XaDependencyGraph *g = ctx->deps;

    // Check if already exists
    int bucket = from % g->bucket_count;
    XaDependency *dep = g->forward[bucket];
    while (dep) {
        if (dep->from_id == from && dep->to_id == to && dep->kind == kind) {
            return;  // Already exists
        }
        dep = dep->next;
    }

    // Add new dependency
    dep = xr_malloc(sizeof(XaDependency));
    if (!dep) return;

    dep->from_id = from;
    dep->to_id = to;
    dep->kind = kind;

    // Add to forward list
    dep->next = g->forward[bucket];
    g->forward[bucket] = dep;

    // Add to reverse list (create a separate node for reverse lookup)
    XaDependency *rev = xr_malloc(sizeof(XaDependency));
    if (rev) {
        rev->from_id = from;
        rev->to_id = to;
        rev->kind = kind;
        int rev_bucket = to % g->bucket_count;
        rev->next = g->reverse[rev_bucket];
        g->reverse[rev_bucket] = rev;
    }

    g->edge_count++;
}

void xa_dep_remove_symbol(XaIncrementalCtx *ctx, uint32_t symbol_id) {
    if (!ctx || !ctx->deps) return;

    XaDependencyGraph *g = ctx->deps;

    // Remove from forward (where symbol is the source)
    int bucket = symbol_id % g->bucket_count;
    XaDependency **pp = &g->forward[bucket];
    while (*pp) {
        if ((*pp)->from_id == symbol_id) {
            XaDependency *to_free = *pp;
            *pp = to_free->next;
            xr_free(to_free);
            g->edge_count--;
        } else {
            pp = &(*pp)->next;
        }
    }

    // Remove from reverse (where symbol is the target)
    bucket = symbol_id % g->bucket_count;
    pp = &g->reverse[bucket];
    while (*pp) {
        if ((*pp)->to_id == symbol_id) {
            XaDependency *to_free = *pp;
            *pp = to_free->next;
            xr_free(to_free);
        } else {
            pp = &(*pp)->next;
        }
    }
}

void xa_dep_get_dependents(XaIncrementalCtx *ctx, uint32_t symbol_id,
                           uint32_t **out_ids, int *out_count) {
    if (!ctx || !ctx->deps || !out_ids || !out_count) return;

    *out_ids = NULL;
    *out_count = 0;

    XaDependencyGraph *g = ctx->deps;
    int bucket = symbol_id % g->bucket_count;

    // Count dependents
    int count = 0;
    XaDependency *dep = g->reverse[bucket];
    while (dep) {
        if (dep->to_id == symbol_id) count++;
        dep = dep->next;
    }

    if (count == 0) return;

    // Allocate and fill
    *out_ids = xr_malloc(sizeof(uint32_t) * count);
    if (!*out_ids) return;

    int idx = 0;
    dep = g->reverse[bucket];
    while (dep && idx < count) {
        if (dep->to_id == symbol_id) {
            (*out_ids)[idx++] = dep->from_id;
        }
        dep = dep->next;
    }
    *out_count = idx;
}

// ============================================================================
// Cache Implementation
// ============================================================================

XaFileCache *xa_cache_get_file(XaIncrementalCtx *ctx, const char *path) {
    if (!ctx || !path) return NULL;

    XaFileCache *fc = ctx->file_caches;
    while (fc) {
        if (fc->path && strcmp(fc->path, path) == 0) {
            return fc;
        }
        fc = fc->next;
    }
    return NULL;
}

static XaFileCache *cache_get_or_create_file(XaIncrementalCtx *ctx, const char *path) {
    XaFileCache *fc = xa_cache_get_file(ctx, path);
    if (fc) return fc;

    fc = xr_calloc(1, sizeof(XaFileCache));
    if (!fc) return NULL;

    fc->path = xr_strdup(path);
    fc->next = ctx->file_caches;
    ctx->file_caches = fc;
    ctx->file_count++;

    return fc;
}

XaBlockCache *xa_cache_get_block(XaFileCache *file, uint32_t symbol_id) {
    if (!file) return NULL;

    XaBlockCache *bc = file->blocks;
    while (bc) {
        if (bc->symbol_id == symbol_id) {
            return bc;
        }
        bc = bc->next;
    }
    return NULL;
}

void xa_cache_update_block(XaIncrementalCtx *ctx, const char *path,
                           uint32_t symbol_id, uint64_t hash,
                           uint32_t start_line, uint32_t end_line,
                           XrType *inferred_type) {
    if (!ctx || !path) return;

    XaFileCache *fc = cache_get_or_create_file(ctx, path);
    if (!fc) return;

    // Find or create block cache
    XaBlockCache *bc = xa_cache_get_block(fc, symbol_id);
    if (!bc) {
        bc = xr_calloc(1, sizeof(XaBlockCache));
        if (!bc) return;
        bc->symbol_id = symbol_id;
        bc->next = fc->blocks;
        fc->blocks = bc;
        fc->block_count++;
    }

    bc->content_hash = hash;
    bc->start_line = start_line;
    bc->end_line = end_line;
    bc->inferred_type = inferred_type;
}

void xa_cache_invalidate_file(XaIncrementalCtx *ctx, const char *path) {
    if (!ctx || !path) return;

    XaFileCache *fc = xa_cache_get_file(ctx, path);
    if (!fc) return;

    // Free all block caches
    XaBlockCache *bc = fc->blocks;
    while (bc) {
        XaBlockCache *next = bc->next;
        xr_free(bc);
        bc = next;
    }
    fc->blocks = NULL;
    fc->block_count = 0;
    fc->file_hash = 0;
}

// ============================================================================
// Incremental Context Lifecycle
// ============================================================================

XaIncrementalCtx *xa_incremental_new(void) {
    XaIncrementalCtx *ctx = xr_calloc(1, sizeof(XaIncrementalCtx));
    if (!ctx) return NULL;

    ctx->deps = dep_graph_new();
    if (!ctx->deps) {
        xr_free(ctx);
        return NULL;
    }

    ctx->dirty_capacity = 32;
    ctx->dirty_symbols = xr_malloc(sizeof(uint32_t) * ctx->dirty_capacity);

    return ctx;
}

void xa_incremental_free(XaIncrementalCtx *ctx) {
    if (!ctx) return;

    dep_graph_free(ctx->deps);

    // Free file caches
    XaFileCache *fc = ctx->file_caches;
    while (fc) {
        XaFileCache *next = fc->next;
        XaBlockCache *bc = fc->blocks;
        while (bc) {
            XaBlockCache *bc_next = bc->next;
            xr_free(bc);
            bc = bc_next;
        }
        xr_free((void*)fc->path);
        xr_free(fc);
        fc = next;
    }

    xr_free(ctx->dirty_symbols);
    xr_free(ctx);
}

// ============================================================================
// Change Detection
// ============================================================================

// Helper: collect function symbols from AST
static void collect_functions(AstNode *node, uint32_t **ids, int *count, int *capacity,
                              XaAnalyzer *analyzer) {
    if (!node) return;

    if (node->type == AST_FUNCTION_DECL) {
        const char *name = node->as.function_decl.name;
        if (name) {
            XaSymbol *sym = xa_analyzer_lookup(analyzer, name);
            if (sym) {
                if (*count >= *capacity) {
                    *capacity = *capacity == 0 ? 16 : *capacity * 2;
                    *ids = xr_realloc(*ids, sizeof(uint32_t) * (*capacity));
                }
                (*ids)[(*count)++] = sym->id;
            }
        }
        return;  // Don't recurse into nested functions for now
    }

    if (node->type == AST_PROGRAM) {
        for (int i = 0; i < node->as.program.count; i++) {
            collect_functions(node->as.program.statements[i], ids, count, capacity, analyzer);
        }
    } else if (node->type == AST_CLASS_DECL) {
        // Collect methods
        for (int i = 0; i < node->as.class_decl.method_count; i++) {
            collect_functions(node->as.class_decl.methods[i], ids, count, capacity, analyzer);
        }
    } else if (node->type == AST_STRUCT_DECL) {
        for (int i = 0; i < node->as.struct_decl.method_count; i++) {
            collect_functions(node->as.struct_decl.methods[i], ids, count, capacity, analyzer);
        }
    }
}

XaChangeSet *xa_detect_changes(XaIncrementalCtx *ctx, XaAnalyzer *analyzer,
                               const char *file, AstNode *old_ast, AstNode *new_ast) {
    if (!ctx || !analyzer || !file) return NULL;

    XaChangeSet *cs = xr_calloc(1, sizeof(XaChangeSet));
    if (!cs) return NULL;

    // Collect function IDs from both ASTs
    uint32_t *old_ids = NULL;
    int old_count = 0, old_cap = 0;
    collect_functions(old_ast, &old_ids, &old_count, &old_cap, analyzer);

    uint32_t *new_ids = NULL;
    int new_count = 0, new_cap = 0;
    collect_functions(new_ast, &new_ids, &new_count, &new_cap, analyzer);

    // Simple comparison: mark all as modified for now
    // A more sophisticated implementation would compare hashes
    cs->modified_symbols = new_ids;
    cs->modified_count = new_count;

    xr_free(old_ids);

    return cs;
}

void xa_changeset_free(XaChangeSet *cs) {
    if (!cs) return;
    xr_free(cs->added_symbols);
    xr_free(cs->removed_symbols);
    xr_free(cs->modified_symbols);
    xr_free(cs);
}

// ============================================================================
// Dirty Propagation
// ============================================================================

static void add_dirty(XaIncrementalCtx *ctx, uint32_t id) {
    // Check if already dirty
    for (int i = 0; i < ctx->dirty_count; i++) {
        if (ctx->dirty_symbols[i] == id) return;
    }

    // Add to dirty set
    if (ctx->dirty_count >= ctx->dirty_capacity) {
        ctx->dirty_capacity *= 2;
        XR_REALLOC_OR_ABORT(ctx->dirty_symbols,
                            sizeof(uint32_t) * (size_t)ctx->dirty_capacity,
                            "incremental dirty_symbols grow");
    }
    ctx->dirty_symbols[ctx->dirty_count++] = id;
}

void xa_propagate_dirty(XaIncrementalCtx *ctx, XaChangeSet *changes) {
    if (!ctx || !changes) return;

    ctx->dirty_count = 0;

    // Add directly changed symbols
    for (int i = 0; i < changes->modified_count; i++) {
        add_dirty(ctx, changes->modified_symbols[i]);
    }
    for (int i = 0; i < changes->removed_count; i++) {
        add_dirty(ctx, changes->removed_symbols[i]);
    }

    // Propagate to dependents (BFS)
    int processed = 0;
    while (processed < ctx->dirty_count) {
        uint32_t sym_id = ctx->dirty_symbols[processed++];

        uint32_t *dependents = NULL;
        int dep_count = 0;
        xa_dep_get_dependents(ctx, sym_id, &dependents, &dep_count);

        for (int i = 0; i < dep_count; i++) {
            add_dirty(ctx, dependents[i]);
        }
        xr_free(dependents);
    }
}

// ============================================================================
// Incremental Update
// ============================================================================

void xa_incremental_update(XaAnalyzer *analyzer, XaIncrementalCtx *incr,
                           const char *file, AstNode *ast) {
    if (!analyzer || !file || !ast) return;

    // Get file cache
    XaFileCache *fc = xa_cache_get_file(incr, file);

    // Calculate file hash
    // For now, we use a simple approach - full re-analysis with caching
    // TODO: Implement true incremental parsing

    if (!fc) {
        // New file - full analysis
        incr->full_analyses++;
        xa_analyzer_analyze(analyzer, file, (XrAstNode*)ast);
        return;
    }

    // File exists - check if content changed
    uint64_t new_hash = xa_hash_ast_block(ast);
    if (fc->file_hash == new_hash) {
        // No change - skip analysis
        incr->skipped_functions++;
        return;
    }

    // Content changed - update
    incr->incremental_updates++;
    fc->file_hash = new_hash;

    // For now, do full re-analysis but track statistics
    // Future: only re-analyze modified functions
    xa_analyzer_update(analyzer, file, (XrAstNode*)ast);
}
