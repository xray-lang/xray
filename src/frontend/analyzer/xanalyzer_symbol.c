/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_symbol.c - Symbol and scope implementation
 */

#include "xanalyzer_symbol.h"
#include "../../base/xchecks.h"
#include "../../base/xhashmap.h"
#include "../../base/xintmap.h"
#include "../../base/xmalloc.h"
#include <string.h>

// Thread-local symbol ID counter pointer (set by analyzer)
// No fallback - must be explicitly set via xa_symbol_set_id_counter()
static XR_THREAD_LOCAL uint32_t *g_symbol_id_ptr = NULL;

// Thread-local symbol registry for O(1) ID lookup (set by analyzer)
static XR_THREAD_LOCAL XrIntMap *g_symbol_registry = NULL;

// Set current symbol ID counter (called by XaAnalyzer before analysis)
void xa_symbol_set_id_counter(uint32_t *counter) {
    g_symbol_id_ptr = counter;
}

// Set symbol registry for O(1) ID lookup (called by XaAnalyzer)
void xa_symbol_set_registry(void *intmap) {
    g_symbol_registry = (XrIntMap *) intmap;
}

// Get next symbol ID (returns 0 if counter not set)
static uint32_t next_symbol_id(void) {
    if (g_symbol_id_ptr)
        return (*g_symbol_id_ptr)++;
    return 0;
}

// Create a new symbol
XaSymbol *xa_symbol_new(const char *name, XaSymbolKind kind) {
    XR_DCHECK(name != NULL, "xa_symbol_new: name is NULL");
    XaSymbol *sym = xr_calloc(1, sizeof(XaSymbol));
    if (!sym)
        return NULL;

    sym->name = name ? xr_strdup(name) : NULL;
    sym->kind = kind;
    sym->id = next_symbol_id();

    return sym;
}

// Release dynamic fields owned by XaSymbolLinks (inline helper, no free of struct itself)
static void links_release_dynamic(XaSymbolLinks *links) {
    if (!links)
        return;
    if (links->param_types)
        xr_free(links->param_types);
    if (links->param_names) {
        for (int i = 0; i < links->param_count; i++) {
            if (links->param_names[i])
                xr_free((char *) links->param_names[i]);
        }
        xr_free(links->param_names);
    }
    if (links->param_escapes)
        xr_free(links->param_escapes);
    if (links->c_export_symbol)
        xr_free((void *) links->c_export_symbol);
    if (links->type_param_names) {
        for (int i = 0; i < links->type_param_count; i++) {
            if (links->type_param_names[i])
                xr_free((char *) links->type_param_names[i]);
        }
        xr_free(links->type_param_names);
    }
    if (links->type_param_constraints) {
        for (int i = 0; i < links->type_param_count; i++) {
            if (links->type_param_constraints[i])
                xr_free(links->type_param_constraints[i]);
        }
        xr_free(links->type_param_constraints);
    }
    if (links->type_param_constraint_counts)
        xr_free(links->type_param_constraint_counts);
    // ADT enum payload metadata
    if (links->enum_payload_types) {
        for (int i = 0; i < links->enum_member_count; i++) {
            if (links->enum_payload_types[i])
                xr_free(links->enum_payload_types[i]);
        }
        xr_free(links->enum_payload_types);
    }
    if (links->enum_payload_counts)
        xr_free(links->enum_payload_counts);
    XaRefLocation *ref = links->references;
    while (ref) {
        XaRefLocation *next = ref->next;
        xr_free(ref);
        ref = next;
    }
}

// Free a symbol (also releases its inline links dynamic fields)
void xa_symbol_free(XaSymbol *symbol) {
    if (!symbol)
        return;
    links_release_dynamic(&symbol->links);
    if (symbol->name)
        xr_free((void *) symbol->name);
    xr_free(symbol);
}

// Add a reference to symbol links
void xa_symbol_add_ref(XaSymbolLinks *links, uint32_t line, uint32_t col, uint32_t end_col,
                       bool is_write) {
    if (!links)
        return;

    XaRefLocation *ref = xr_malloc(sizeof(XaRefLocation));
    if (!ref)
        return;

    ref->line = line;
    ref->column = col;
    ref->end_column = end_col;
    ref->is_write = is_write;
    ref->next = links->references;
    links->references = ref;
    links->ref_count++;
}

// Get references from symbol links
XaRefLocation *xa_symbol_get_refs(XaSymbolLinks *links, int *count) {
    if (!links) {
        if (count)
            *count = 0;
        return NULL;
    }
    if (count)
        *count = links->ref_count;
    return links->references;
}

// Create a new scope
XaScope *xa_scope_new(XaScopeKind kind, XaScope *parent) {
    XR_DCHECK(kind >= 0, "scope_new: invalid scope kind");
    XaScope *scope = xr_calloc(1, sizeof(XaScope));
    if (!scope)
        return NULL;

    scope->kind = kind;
    scope->parent = parent;
    scope->symbols = xr_hashmap_new();

    // Add to parent's children
    if (parent) {
        if (parent->child_count >= parent->child_capacity) {
            int new_cap = parent->child_capacity == 0 ? 4 : parent->child_capacity * 2;
            XR_REALLOC_OR_ABORT(parent->children, sizeof(XaScope *) * (size_t) new_cap,
                                "scope children grow");
            parent->child_capacity = new_cap;
        }
        parent->children[parent->child_count++] = scope;
        XR_DCHECK(parent->child_count <= parent->child_capacity,
                  "scope_new: child_count > child_capacity");
    }

    return scope;
}

// Free a scope (and its children)
void xa_scope_free(XaScope *scope) {
    if (!scope)
        return;

    // Free children first
    for (int i = 0; i < scope->child_count; i++) {
        xa_scope_free(scope->children[i]);
    }
    if (scope->children)
        xr_free(scope->children);

    // Free symbol map
    xr_hashmap_free(scope->symbols);

    xr_free(scope);
}

// Add symbol to scope
void xa_scope_add_symbol(XaScope *scope, XaSymbol *symbol) {
    if (!scope || !symbol || !symbol->name)
        return;
    symbol->scope = scope;
    // The map is the only storage for scope symbols; a dropped insert
    // would silently corrupt name resolution. Fail-stop on OOM.
    XR_CHECK(xr_hashmap_set(scope->symbols, symbol->name, symbol),
             "scope symbol insert failed (out of memory)");
    // Auto-register for O(1) ID lookup
    if (g_symbol_registry && symbol->id != 0) {
        xr_intmap_set(g_symbol_registry, symbol->id, symbol);
    }
}

// Remove symbol from scope (does NOT free the symbol)
bool xa_scope_remove_symbol(XaScope *scope, const char *name) {
    if (!scope || !name)
        return false;
    return xr_hashmap_delete(scope->symbols, name);
}

// Look up symbol (search up through parent scopes)
XaSymbol *xa_scope_lookup(XaScope *scope, const char *name) {
    XR_DCHECK(name != NULL, "scope_lookup: NULL name");
    while (scope) {
        XaSymbol *sym = xr_hashmap_get(scope->symbols, name);
        if (sym)
            return sym;
        scope = scope->parent;
    }
    return NULL;
}

// Look up symbol only in current scope
XaSymbol *xa_scope_lookup_local(XaScope *scope, const char *name) {
    if (!scope)
        return NULL;
    return xr_hashmap_get(scope->symbols, name);
}

// Check if child_scope is a descendant of (or equal to) ancestor_scope
bool xa_scope_is_descendant(XaScope *child, XaScope *ancestor) {
    if (!child || !ancestor)
        return false;
    XaScope *scope = child;
    while (scope) {
        if (scope == ancestor)
            return true;
        scope = scope->parent;
    }
    return false;
}

// Find the scope where a symbol is defined (searching up through parents)
XaScope *xa_scope_find_definition(XaScope *scope, const char *name) {
    while (scope) {
        if (xa_scope_lookup_local(scope, name)) {
            return scope;
        }
        scope = scope->parent;
    }
    return NULL;
}

// Find scope by AST node (recursive search through children)
static XaScope *find_scope_by_node_recursive(XaScope *scope, void *ast_node) {
    if (!scope)
        return NULL;
    if (scope->ast_node == ast_node)
        return scope;

    for (int i = 0; i < scope->child_count; i++) {
        XaScope *found = find_scope_by_node_recursive(scope->children[i], ast_node);
        if (found)
            return found;
    }
    return NULL;
}

XaScope *xa_scope_find_by_node(XaScope *root, void *ast_node) {
    if (!root || !ast_node)
        return NULL;
    return find_scope_by_node_recursive(root, ast_node);
}

// Create class info
XrClassInfo *xa_class_info_new(const char *name) {
    XR_DCHECK(name != NULL, "class_info_new: NULL name");
    XrClassInfo *info = xr_calloc(1, sizeof(XrClassInfo));
    if (!info)
        return NULL;
    info->name = name ? xr_strdup(name) : NULL;
    info->members_map = xr_hashmap_new();
    return info;
}

// Free class info
void xa_class_info_free(XrClassInfo *info) {
    if (!info)
        return;
    if (info->name)
        xr_free((void *) info->name);
    if (info->fields)
        xr_free(info->fields);
    if (info->methods)
        xr_free(info->methods);
    if (info->static_fields)
        xr_free(info->static_fields);
    if (info->static_methods)
        xr_free(info->static_methods);
    if (info->constructor_params)
        xr_free(info->constructor_params);
    if (info->vtable)
        xr_free(info->vtable);
    if (info->struct_layout) {
        xr_free(info->struct_layout->field_names);
        xr_free(info->struct_layout);
    }
    if (info->members_map)
        xr_hashmap_free(info->members_map);
    xr_free(info);
}

// Grow member list with 2x strategy (initial 4, double at powers of 2)
static void member_list_append(XaSymbol ***list, int *count, XaSymbol *item) {
    int n = *count;
    if (n == 0) {
        *list = xr_malloc(sizeof(XaSymbol *) * 4);
    } else if (n >= 4 && (n & (n - 1)) == 0) {
        *list = xr_realloc(*list, sizeof(XaSymbol *) * n * 2);
    }
    (*list)[n] = item;
    (*count)++;
}

// Add field to class
void xa_class_info_add_field(XrClassInfo *info, XaSymbol *field) {
    if (!info || !field)
        return;
    if (field->is_static) {
        member_list_append(&info->static_fields, &info->static_field_count, field);
    } else {
        member_list_append(&info->fields, &info->field_count, field);
    }
    if (info->members_map && field->name) {
        // Lookup only falls back to linear scan when the map is absent
        // entirely; a member missing from a live map would be invisible.
        XR_CHECK(xr_hashmap_set(info->members_map, field->name, field),
                 "class member insert failed (out of memory)");
    }
}

// Add method to class
void xa_class_info_add_method(XrClassInfo *info, XaSymbol *method) {
    if (!info || !method)
        return;
    if (method->is_static) {
        member_list_append(&info->static_methods, &info->static_method_count, method);
    } else {
        member_list_append(&info->methods, &info->method_count, method);
    }
    if (info->members_map && method->name) {
        // See xa_class_info_add_field: missing entries are invisible.
        XR_CHECK(xr_hashmap_set(info->members_map, method->name, method),
                 "class member insert failed (out of memory)");
    }
}

static XaSymbol *xa_class_info_lookup_member_owner_filtered(XrClassInfo *info, const char *name,
                                                            bool want_static,
                                                            XrClassInfo **owner_out) {
    if (owner_out)
        *owner_out = NULL;
    if (!info || !name)
        return NULL;

    if (want_static) {
        for (int i = 0; i < info->static_field_count; i++) {
            if (strcmp(info->static_fields[i]->name, name) == 0) {
                if (owner_out)
                    *owner_out = info;
                return info->static_fields[i];
            }
        }
        for (int i = 0; i < info->static_method_count; i++) {
            if (strcmp(info->static_methods[i]->name, name) == 0) {
                if (owner_out)
                    *owner_out = info;
                return info->static_methods[i];
            }
        }
    } else {
        for (int i = 0; i < info->field_count; i++) {
            if (strcmp(info->fields[i]->name, name) == 0) {
                if (owner_out)
                    *owner_out = info;
                return info->fields[i];
            }
        }
        for (int i = 0; i < info->method_count; i++) {
            if (strcmp(info->methods[i]->name, name) == 0) {
                if (owner_out)
                    *owner_out = info;
                return info->methods[i];
            }
        }
    }

    return info->base ? xa_class_info_lookup_member_owner_filtered(info->base, name, want_static,
                                                                   owner_out)
                      : NULL;
}

// Look up member in class (searches base classes too)
XaSymbol *xa_class_info_lookup_instance_member(XrClassInfo *info, const char *name) {
    return xa_class_info_lookup_member_owner_filtered(info, name, false, NULL);
}

XaSymbol *xa_class_info_lookup_static_member(XrClassInfo *info, const char *name) {
    return xa_class_info_lookup_member_owner_filtered(info, name, true, NULL);
}

XaSymbol *xa_class_info_lookup_member(XrClassInfo *info, const char *name) {
    XaSymbol *member = xa_class_info_lookup_instance_member(info, name);
    return member ? member : xa_class_info_lookup_static_member(info, name);
}

XaSymbol *xa_class_info_lookup_instance_member_owner(XrClassInfo *info, const char *name,
                                                     XrClassInfo **owner_out) {
    return xa_class_info_lookup_member_owner_filtered(info, name, false, owner_out);
}

XaSymbol *xa_class_info_lookup_static_member_owner(XrClassInfo *info, const char *name,
                                                   XrClassInfo **owner_out) {
    return xa_class_info_lookup_member_owner_filtered(info, name, true, owner_out);
}

XaSymbol *xa_class_info_lookup_member_owner(XrClassInfo *info, const char *name,
                                            XrClassInfo **owner_out) {
    XaSymbol *member = xa_class_info_lookup_instance_member_owner(info, name, owner_out);
    return member ? member : xa_class_info_lookup_static_member_owner(info, name, owner_out);
}

// Function signature helpers
void xa_symbol_links_set_function_sig(XaSymbolLinks *links, XrType **param_types,
                                      const char **param_names, int param_count,
                                      XrType *return_type) {
    if (!links)
        return;

    // Free old param types
    if (links->param_types) {
        xr_free(links->param_types);
        links->param_types = NULL;
    }

    // Free old param names
    if (links->param_names) {
        for (int i = 0; i < links->param_count; i++) {
            xr_free((char *) links->param_names[i]);
        }
        xr_free(links->param_names);
        links->param_names = NULL;
    }
    if (links->param_escapes) {
        xr_free(links->param_escapes);
        links->param_escapes = NULL;
        links->param_escape_count = 0;
    }
    // Default-value expressions are reset together with the signature; call
    // xa_symbol_links_set_param_defaults afterwards to repopulate them.
    if (links->param_defaults) {
        xr_free(links->param_defaults);
        links->param_defaults = NULL;
    }

    // Copy param types
    if (param_count > 0 && param_types) {
        links->param_types = xr_malloc(sizeof(XrType *) * param_count);
        if (links->param_types) {
            memcpy(links->param_types, param_types, sizeof(XrType *) * param_count);
        }
    }

    // Copy param names
    if (param_count > 0 && param_names) {
        links->param_names = xr_malloc(sizeof(char *) * param_count);
        if (links->param_names) {
            for (int i = 0; i < param_count; i++) {
                links->param_names[i] = param_names[i] ? xr_strdup(param_names[i]) : NULL;
            }
        }
    }

    links->param_count = param_count;
    links->return_type = return_type;
}

void xa_symbol_links_set_param_defaults(XaSymbolLinks *links, struct AstNode **defaults,
                                        int count) {
    if (!links)
        return;
    if (links->param_defaults) {
        xr_free(links->param_defaults);
        links->param_defaults = NULL;
    }
    if (!defaults || count <= 0)
        return;
    bool any = false;
    for (int i = 0; i < count; i++) {
        if (defaults[i]) {
            any = true;
            break;
        }
    }
    if (!any)
        return;
    struct AstNode **copy = (struct AstNode **) xr_calloc((size_t) count, sizeof(struct AstNode *));
    if (!copy)
        return;
    for (int i = 0; i < count; i++)
        copy[i] = defaults[i];
    links->param_defaults = copy;
}

XrType *xa_symbol_links_get_return_type(XaSymbolLinks *links) {
    return links ? links->return_type : NULL;
}

XrType **xa_symbol_links_get_param_types(XaSymbolLinks *links, int *count) {
    if (!links) {
        if (count)
            *count = 0;
        return NULL;
    }
    if (count)
        *count = links->param_count;
    return links->param_types;
}

const char **xa_symbol_links_get_param_names(XaSymbolLinks *links, int *count) {
    if (!links) {
        if (count)
            *count = 0;
        return NULL;
    }
    if (count)
        *count = links->param_count;
    return links->param_names;
}

bool xa_symbol_is_function(XaSymbol *symbol) {
    if (!symbol)
        return false;
    return symbol->kind == XA_SYM_FUNCTION || symbol->kind == XA_SYM_METHOD;
}

// Set generic type parameters for a function/class.  Each parameter may
// carry an intersection-style constraint list (T: A & B & C); the lists
// are deep-copied into the symbol storage so callers retain ownership of
// their original buffers.
void xa_symbol_links_set_type_params(XaSymbolLinks *links, const char **names,
                                     XrType ***constraint_lists, const int *constraint_counts,
                                     int count) {
    if (!links || count <= 0)
        return;

    links->type_param_count = count;
    links->type_param_names = xr_malloc(sizeof(const char *) * count);
    links->type_param_constraints = xr_malloc(sizeof(XrType **) * count);
    links->type_param_constraint_counts = xr_malloc(sizeof(int) * count);
    if (!links->type_param_names || !links->type_param_constraints ||
        !links->type_param_constraint_counts) {
        xr_free(links->type_param_names);
        xr_free(links->type_param_constraints);
        xr_free(links->type_param_constraint_counts);
        links->type_param_names = NULL;
        links->type_param_constraints = NULL;
        links->type_param_constraint_counts = NULL;
        links->type_param_count = 0;
        return;
    }

    for (int i = 0; i < count; i++) {
        links->type_param_names[i] = names[i] ? xr_strdup(names[i]) : NULL;

        int n = constraint_counts ? constraint_counts[i] : 0;
        XrType **src = constraint_lists ? constraint_lists[i] : NULL;
        if (n > 0 && src) {
            XrType **copy = xr_malloc(sizeof(XrType *) * n);
            for (int j = 0; j < n; j++) {
                copy[j] = src[j];
            }
            links->type_param_constraints[i] = copy;
            links->type_param_constraint_counts[i] = n;
        } else {
            links->type_param_constraints[i] = NULL;
            links->type_param_constraint_counts[i] = 0;
        }
    }
}

int xa_symbol_links_get_type_param_count(XaSymbolLinks *links) {
    return links ? links->type_param_count : 0;
}

const char *xa_symbol_links_get_type_param_name(XaSymbolLinks *links, int index) {
    if (!links || index < 0 || index >= links->type_param_count)
        return NULL;
    return links->type_param_names[index];
}

XrType **xa_symbol_links_get_type_param_constraints(XaSymbolLinks *links, int index,
                                                    int *out_count) {
    if (out_count)
        *out_count = 0;
    if (!links || index < 0 || index >= links->type_param_count)
        return NULL;
    if (out_count)
        *out_count = links->type_param_constraint_counts[index];
    return links->type_param_constraints[index];
}

void xa_symbol_links_copy_export_metadata(XaSymbolLinks *dst, const XaSymbolLinks *src) {
    if (!dst)
        return;

    links_release_dynamic(dst);
    memset(dst, 0, sizeof(*dst));
    if (!src)
        return;

    dst->type = src->type;
    dst->declared_type = src->declared_type;
    dst->type_computed = src->type_computed;
    dst->is_definitely_assigned = src->is_definitely_assigned;
    dst->move_state = src->move_state;
    dst->moved_line = src->moved_line;
    dst->moved_column = src->moved_column;

    xa_symbol_links_set_function_sig(dst, src->param_types, src->param_names, src->param_count,
                                     src->return_type);
    if (src->param_defaults)
        xa_symbol_links_set_param_defaults(dst, src->param_defaults, src->param_count);
    if (src->param_escapes && src->param_escape_count > 0) {
        dst->param_escapes = xr_malloc((size_t) src->param_escape_count * sizeof(uint8_t));
        if (dst->param_escapes) {
            memcpy(dst->param_escapes, src->param_escapes,
                   (size_t) src->param_escape_count * sizeof(uint8_t));
            dst->param_escape_count = src->param_escape_count;
        }
    }
    dst->return_type_inferred = src->return_type_inferred;
    dst->is_extern = src->is_extern;
    dst->is_c_export = src->is_c_export;
    dst->c_export_symbol = src->c_export_symbol ? xr_strdup(src->c_export_symbol) : NULL;
    dst->error_set = src->error_set;

    if (src->type_param_count > 0) {
        xa_symbol_links_set_type_params(dst, src->type_param_names, src->type_param_constraints,
                                        src->type_param_constraint_counts, src->type_param_count);
    }

    dst->class_info = src->class_info;
    dst->enum_member_names = src->enum_member_names;
    dst->enum_member_count = src->enum_member_count;
    dst->enum_value_type = src->enum_value_type;
    dst->is_adt_enum = src->is_adt_enum;
    if (src->enum_member_count > 0 && src->enum_payload_counts) {
        dst->enum_payload_counts = xr_malloc((size_t) src->enum_member_count * sizeof(int));
        if (dst->enum_payload_counts) {
            memcpy(dst->enum_payload_counts, src->enum_payload_counts,
                   (size_t) src->enum_member_count * sizeof(int));
        }
    }
    if (src->enum_member_count > 0 && src->enum_payload_types) {
        dst->enum_payload_types = xr_calloc((size_t) src->enum_member_count, sizeof(XrType **));
        if (dst->enum_payload_types) {
            for (int i = 0; i < src->enum_member_count; i++) {
                int pc = src->enum_payload_counts ? src->enum_payload_counts[i] : 0;
                if (pc <= 0 || !src->enum_payload_types[i])
                    continue;
                XrType **copy = xr_malloc((size_t) pc * sizeof(XrType *));
                if (!copy)
                    continue;
                memcpy(copy, src->enum_payload_types[i], (size_t) pc * sizeof(XrType *));
                dst->enum_payload_types[i] = copy;
            }
        }
    }

    dst->module_name = src->module_name;
    dst->file_path = src->file_path;
    dst->assign_count = src->assign_count;
    dst->is_const_foldable = src->is_const_foldable;
    dst->is_loop_variable = src->is_loop_variable;
}

// Callback context for collecting symbols
struct SymbolCollectCtx {
    XaSymbol **result;
    int idx;
};

static void collect_symbol_cb(const char *key, void *value, void *userdata) {
    (void) key;
    struct SymbolCollectCtx *ctx = (struct SymbolCollectCtx *) userdata;
    ctx->result[ctx->idx++] = (XaSymbol *) value;
}

// Get all symbols in a scope (for LSP completion)
XaSymbol **xa_scope_get_all_symbols(XaScope *scope, int *count) {
    *count = 0;
    if (!scope || !scope->symbols)
        return NULL;

    uint32_t n = xr_hashmap_count((XrHashMap *) scope->symbols);
    if (n == 0)
        return NULL;

    XaSymbol **result = xr_malloc(sizeof(XaSymbol *) * n);
    if (!result)
        return NULL;

    struct SymbolCollectCtx ctx = {result, 0};
    xr_hashmap_foreach((XrHashMap *) scope->symbols, collect_symbol_cb, &ctx);

    *count = ctx.idx;
    return result;
}

// Lookup symbol by ID — O(1) via registry, fallback to O(n) scan
XaSymbol *xa_scope_lookup_by_id(XaScope *scope, uint32_t id) {
    if (id == 0)
        return NULL;

    // Fast path: O(1) registry lookup
    if (g_symbol_registry) {
        return (XaSymbol *) xr_intmap_get(g_symbol_registry, id);
    }

    // Slow fallback (should not happen when analyzer is active)
    if (!scope || !scope->symbols)
        return NULL;
    XrHashMap *map = (XrHashMap *) scope->symbols;
    for (uint32_t i = 0; i < map->capacity; i++) {
        XrHashMapEntry *entry = &map->entries[i];
        if (entry->key && entry->value != XR_HASHMAP_TOMBSTONE) {
            XaSymbol *sym = (XaSymbol *) entry->value;
            if (sym && sym->id == id)
                return sym;
        }
    }
    if (scope->parent)
        return xa_scope_lookup_by_id(scope->parent, id);
    return NULL;
}

// Type alias helpers
XaSymbol *xa_scope_define_type_alias(XaScope *scope, const char *name, void *type) {
    if (!scope || !name)
        return NULL;

    // Check if already exists
    XaSymbol *existing = xa_scope_lookup_local(scope, name);
    if (existing)
        return NULL;  // Already defined

    // Create type alias symbol
    XaSymbol *sym = xa_symbol_new(name, XA_SYM_TYPE_ALIAS);
    if (!sym)
        return NULL;

    sym->alias_type = type;  // Store type directly in symbol
    xa_scope_add_symbol(scope, sym);

    return sym;
}

void *xa_scope_resolve_type_alias(XaScope *scope, const char *name) {
    if (!scope || !name)
        return NULL;

    XaSymbol *sym = xa_scope_lookup(scope, name);
    if (!sym || sym->kind != XA_SYM_TYPE_ALIAS)
        return NULL;

    return sym->alias_type;
}

bool xa_symbol_is_type_alias(XaSymbol *symbol) {
    return symbol && symbol->kind == XA_SYM_TYPE_ALIAS;
}
