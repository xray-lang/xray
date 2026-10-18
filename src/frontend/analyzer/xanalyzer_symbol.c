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
#include "xanalyzer_link_metadata.h"
#include "xanalyzer_enum_storage.h"
#include "xanalyzer.h"
#include "../parser/xast_nodes_common.h"
#include "../../base/xchecks.h"
#include "../../base/xhashmap.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xstruct_layout.h"
#include <string.h>

int xa_enum_info_find_variant(const XaEnumInfo *info, const char *name) {
    if (!info || !name)
        return -1;
    for (uint32_t i = 0; i < info->variant_count; i++) {
        const char *variant_name = info->variants[i].name;
        if (variant_name && strcmp(variant_name, name) == 0)
            return (int) i;
    }
    return -1;
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
bool xa_symbol_is_function(XaSymbol *symbol) {
    if (!symbol)
        return false;
    return symbol->kind == XA_SYM_FUNCTION || symbol->kind == XA_SYM_METHOD;
}

void xa_symbol_links_copy_export_metadata(XaAnalyzer *dst_analyzer, XaSymbolLinks *dst,
                                          const XaSymbolLinks *src) {
    XrCompileState *state = dst ? dst->resource_state : NULL;
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK || src == dst)
        return;
    XaAnalyzer *src_analyzer = src ? src->summary_owner : NULL;
    xa_symbol_links_dispose_contents(dst);
    memset(dst, 0, sizeof(*dst));
    dst->resource_state = state;
    dst->summary_owner = dst_analyzer;
    if (!src)
        return;

    dst->type = src->type;
    dst->declared_type = src->declared_type;
    dst->type_computed = src->type_computed;
    dst->is_definitely_assigned = src->is_definitely_assigned;
    dst->binding_use = XA_BINDING_LIVE;
    dst->root_id = 0;
    dst->root_alias = XA_ROOT_ALIAS_UNKNOWN;
    dst->binding_mutability = XA_BINDING_STABLE;
    dst->value_capability = src->value_capability;
    dst->storage_domain = src->storage_domain;
    dst->allocation_plan = src->allocation_plan;
    dst->moved_line = 0;
    dst->moved_column = 0;

    if (xa_symbol_links_set_function_sig(dst, src->param_types, src->param_names, src->param_count,
                                     src->return_type) != XR_COMPILE_RESOURCE_OK) {
        return;
    }
    if (src->param_defaults)
        if (xa_symbol_links_set_param_defaults(dst, src->param_defaults, src->param_count) != XR_COMPILE_RESOURCE_OK) {
            return;
        }
    if (xa_symbol_links_copy_param_effect_summaries(dst, src) != XR_COMPILE_RESOURCE_OK) return;
    dst->return_type_inferred = src->return_type_inferred;
    dst->return_storage_domain = src->return_storage_domain;
    dst->return_storage_known = src->return_storage_known;
    dst->return_storage_mixed = src->return_storage_mixed;
    dst->return_storage_scanned = src->return_storage_scanned;
    dst->return_storage_scan_in_progress = false;
    dst->return_ownership = src->return_ownership;
    dst->return_ownership_scanned = src->return_ownership_scanned;
    dst->return_ownership_scan_in_progress = false;
    if (xa_symbol_links_set_view_contract(dst, &src->return_view) != XR_COMPILE_RESOURCE_OK) return;
    if (xa_symbol_links_copy_return_function_effect_summary(dst, src) != XR_COMPILE_RESOURCE_OK) return;
    dst->function_decl_node = src->function_decl_node;
    dst->nominal_decl_node = src->nominal_decl_node;
    dst->interface_decl_node = src->interface_decl_node;
    if (xa_symbol_links_set_deprecated(dst, src->is_deprecated, src->deprecated_message) != XR_COMPILE_RESOURCE_OK) return;
    dst->is_extern = src->is_extern;
    if (src_analyzer && dst_analyzer && src_analyzer != dst_analyzer) {
        dst->effect_id =
            xa_effect_db_import(dst_analyzer->effect_db, src_analyzer->effect_db, src->effect_id);
        const XaMemoryEffectSummary *memory =
            xa_memory_effect_db_get(src_analyzer->memory_effect_db, src->memory_effect_id);
        dst->memory_effect_id =
            memory ? xa_memory_effect_db_intern(dst_analyzer->memory_effect_db, memory)
                   : XA_MEMORY_EFFECT_NONE;
        const XaAllocationSummary *allocation =
            xa_allocation_db_get(src_analyzer->allocation_db, src->alloc_effect_id);
        if (allocation) {
            XaAllocationSummary imported = *allocation;
            imported.first_site_node_id = 0;
            imported.first_callee_symbol_id = 0;
            imported.callee_effect_id = XA_ALLOC_EFFECT_NONE;
            dst->alloc_effect_id = xa_allocation_db_intern(dst_analyzer->allocation_db, &imported);
        }
    } else if (src_analyzer && src_analyzer == dst_analyzer) {
        dst->effect_id = src->effect_id;
        dst->memory_effect_id = src->memory_effect_id;
        dst->alloc_effect_id = src->alloc_effect_id;
    }
    dst->throw_effect = src->throw_effect;
    /* The typed throw-effect bit is an invariant of both the symbol links and
     * its function type. Import metadata used to copy only the links field,
     * leaving the freshly reconstructed function type at its fail-closed
     * MAY_THROW default. Xi lowering consults the type, so every cross-module
     * call retained a dead error-channel check even when the exported callee
     * had already been proven NO_THROW. */
    if (dst->type && dst->type->kind == XR_KIND_FUNCTION)
        xr_type_function_set_throw_effect(dst->type, dst->throw_effect);
    dst->intrinsic_id = src->intrinsic_id;
    dst->alloc_state = src->alloc_state;
    dst->alloc_reason_bits = src->alloc_reason_bits;
    dst->alloc_fingerprint = src->alloc_fingerprint;
    dst->alloc_effect_complete = src->alloc_effect_complete;
    for (int i = 0; i < dst->param_effect_count; i++) {
        dst->param_effects[i].callable_effects = dst->effect_id;
        dst->param_effects[i].memory_effects = dst->memory_effect_id;
    }

    if (src->type_param_count > 0) {
        if (xa_symbol_links_set_type_params(dst, src->type_param_names, src->type_param_constraints,
                                        src->type_param_constraint_counts, src->type_param_count) != XR_COMPILE_RESOURCE_OK) {
            return;
        }
    }

    /* Export metadata observes the declaration owner's canonical class graph.
     * The source symbol remains its sole owner; imported symbol views must not
     * reclaim that graph when their own scope is destroyed. */
    dst->class_info = src->class_info;
    dst->owns_class_info = false;
    XrOsIoPolicy enum_policy = xr_compile_io_policy(xr_compile_state_resources(state));
    if (xa_enum_capture_status(state, xa_enum_info_clone(&enum_policy, src->enum_info, &dst->enum_info)) != XR_COMPILE_RESOURCE_OK) return;

    dst->module_name = src->module_name;
    dst->import_member_name = src->import_member_name;
    dst->file_path = src->file_path;
    dst->assign_count = src->assign_count;
    dst->value_mutated = src->value_mutated;
    dst->is_const_foldable = src->is_const_foldable;
    dst->const_initializer = src->const_initializer;
    dst->has_ct_value = src->has_ct_value;
    dst->ct_value = src->ct_value;
    dst->is_loop_variable = src->is_loop_variable;
}

const XaMemoryEffectSummary *xa_symbol_links_memory_effect_summary(const XaSymbolLinks *links) {
    XaAnalyzer *owner = links ? links->summary_owner : NULL;
    if (!owner || !owner->memory_effect_db || links->memory_effect_id == XA_MEMORY_EFFECT_NONE)
        return NULL;
    return xa_memory_effect_db_get(owner->memory_effect_db, links->memory_effect_id);
}

// Callback context for collecting symbols
// Type alias helpers
XaSymbol *xa_scope_define_type_alias(XaScope *scope, const char *name, void *type) {
    if (!scope || !name)
        return NULL;

    // Check if already exists
    XaSymbol *existing = xa_scope_lookup_local(scope, name);
    if (existing)
        return NULL;  // Already defined

    // Create type alias symbol
    XaSymbol *sym = xa_symbol_new(scope->registry, name, XA_SYM_TYPE_ALIAS);
    if (!sym)
        return NULL;

    sym->alias_type = type;  // Store type directly in symbol
    if (xa_scope_add_symbol(scope, sym) != XR_COMPILE_RESOURCE_OK) {
        xa_symbol_free(sym);
        return NULL;
    }

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
