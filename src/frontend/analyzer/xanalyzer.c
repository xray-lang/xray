/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer.c - Static type analyzer implementation
 */

#include "xanalyzer_enum_storage.h"
#include "xanalyzer.h"
#include "xanalyzer_link_metadata.h"
#include "xanalyzer_signature.h"
#include "xanalyzer_storage.h"
#include "xanalyzer_export_query.h"
#include "xanalyzer_exports.h"
#include "xanalyzer_file_storage.h"
#include "xanalyzer_visitor.h"
#include "../parser/xast_walk.h"
#include "../../base/xchecks.h"
#include "xanalyzer_infer.h"
#include "xanalyzer_builtins.h"
#include "xanalyzer_incremental.h"
#include "xanalyzer_builtin_interfaces.h"
#include "xa_node_table.h"
#include "xa_effect_db.h"
#include "xa_memory_effect_db.h"
#include "xa_alloc_effect.h"
#include "xa_parallel_call_plan.h"
#include "xa_enum_record_plan.h"
#include "xa_resolved_call.h"
#include "xa_selection.h"
#include "../../runtime/value/xtype_internal.h"
#include "../../runtime/value/xenum_layout.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../shared/xr_core_intrinsic.h"
#include "../../shared/xr_target_query_registry_gen.h"
#include "../../toolchain/xcompiler_session.h"
#include "../parser/xast_nodes.h"
#include "../parser/xast_types.h"
#include "../../base/xhashmap.h"
#include "../../base/xhash.h"
#include "../../base/xmalloc.h"
#include "../../base/xarena.h"
#include "../../module/xmodule_graph.h"
#include "../../stdlib/xstdlib_metadata.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>

typedef struct XaForeignSymbolView {
    const XaSymbol *source;
    const XaAnalyzer *source_owner;
    uint64_t source_revision;
    XaSymbol *view;
    struct XaForeignSymbolView *next;
} XaForeignSymbolView;

const XrTargetDataLayout *xa_analyzer_target_data_layout(const XaAnalyzer *analyzer) {
    return analyzer ? &analyzer->target_data_layout : NULL;
}

// Register a builtin function symbol in analyzer scope
static void register_builtin_func(XaAnalyzer *analyzer, const char *name, XrType *type) {
    XR_DCHECK(analyzer != NULL, "register_builtin_func: NULL analyzer");
    XR_DCHECK(name != NULL, "register_builtin_func: NULL name");
    XR_DCHECK(type != NULL, "register_builtin_func: NULL type");
    XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, name, XA_SYM_FUNCTION);
    if (!sym) return;
    sym->location.line = 0;
    sym->is_builtin = true;
    if (xa_scope_add_symbol(analyzer->global_scope, sym) != XR_COMPILE_RESOURCE_OK) {
        xa_symbol_free(sym);
        return;
    }
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links) {
        links->type = type;
        links->declared_type = type;
        links->is_definitely_assigned = true;
    }
}

/* Register a compiler-owned core callable by its stable registry identity.
 * Source spelling is a display/binding projection only and is never recovered
 * by downstream semantic consumers. */
static void register_core_builtin_func(XaAnalyzer *analyzer, XrCoreBuiltinId id, XrType *type) {
    const XrCoreIntrinsicDesc *desc = xr_core_intrinsic_by_id(id);
    XR_DCHECK(desc != NULL, "register_core_builtin_func: invalid builtin id");
    register_builtin_func(analyzer, desc->source_name, type);
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) return;
    XaSymbol *sym = xa_scope_lookup(analyzer->global_scope, desc->source_name);
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) return;
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(analyzer, sym) : NULL;
    XR_DCHECK(sym && sym->is_builtin && links,
              "register_core_builtin_func: builtin binding missing");
    if (links)
        links->core_builtin_id = id;
}

// Register a builtin module namespace (XA_SYM_MODULE triggers member signature lookup)
static void register_builtin_module(XaAnalyzer *analyzer, const char *name) {
    XR_DCHECK(analyzer != NULL, "register_builtin_module: NULL analyzer");
    XR_DCHECK(name != NULL, "register_builtin_module: NULL name");
    XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, name, XA_SYM_MODULE);
    if (!sym) return;
    sym->location.line = 0;
    sym->is_builtin = true;
    if (xa_scope_add_symbol(analyzer->global_scope, sym) != XR_COMPILE_RESOURCE_OK) {
        xa_symbol_free(sym);
        return;
    }
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links) {
        links->type = xr_type_new_unknown(NULL);
        links->declared_type = links->type;
        links->is_definitely_assigned = true;
        links->module_name = name;
    }
}

static void register_target_query_namespace(XaAnalyzer *analyzer) {
    register_builtin_module(analyzer, "target");
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) return;
    XaSymbol *symbol = xa_scope_lookup(analyzer->global_scope, "target");
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) return;
    XaSymbolLinks *links = symbol ? xa_analyzer_get_links(analyzer, symbol) : NULL;
    XR_DCHECK(symbol && symbol->is_builtin && links,
              "register_target_query_namespace: builtin binding missing");
    if (links)
        links->target_namespace_id = XA_TARGET_NAMESPACE_TARGET;
}

// Register a builtin variable symbol in analyzer scope
static void register_builtin_var(XaAnalyzer *analyzer, const char *name, XrType *type,
                                 bool is_const) {
    XR_DCHECK(analyzer != NULL, "register_builtin_var: NULL analyzer");
    XR_DCHECK(name != NULL, "register_builtin_var: NULL name");
    XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, name, XA_SYM_VARIABLE);
    if (!sym) return;
    sym->location.line = 0;
    sym->is_builtin = true;
    sym->is_const = is_const;
    if (xa_scope_add_symbol(analyzer->global_scope, sym) != XR_COMPILE_RESOURCE_OK) {
        xa_symbol_free(sym);
        return;
    }
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links) {
        links->type = type;
        links->declared_type = type;
        links->is_definitely_assigned = true;
    }
}

/* Exact scalar keywords are compile-time type namespaces. Build their symbols
 * from the canonical registry so static members such as i64.parse resolve
 * without a hand-written name table or a generic identifier fallback. */
static void register_exact_scalar_namespaces(XaAnalyzer *analyzer) {
    size_t count = 0;
    const XrExactScalarDesc *rows = xr_exact_scalar_rows(&count);
    for (size_t i = 0; i < count; i++) {
        const XrExactScalarDesc *row = &rows[i];
        XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, row->source_name, XA_SYM_CLASS);
        if (!sym)
            return;
        sym->location.line = 0;
        sym->is_builtin = true;
        sym->is_const = true;
        if (xa_scope_add_symbol(analyzer->global_scope, sym) != XR_COMPILE_RESOURCE_OK) {
            xa_symbol_free(sym);
            return;
        }
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links)
            continue;
        links->type = xr_scalar_rep_is_float(row->native_type)
                          ? xr_type_new_float_width(analyzer->isolate, row->native_type)
                          : xr_type_new_int_width(analyzer->isolate, row->native_type);
        links->declared_type = links->type;
        links->is_definitely_assigned = true;
    }
}

static void register_builtin_module_types_in_prelude(XaAnalyzer *analyzer,
                                                     const char *module_name) {
    const XaBuiltinModule *module = xa_builtin_get_module_info(module_name);
    if (!analyzer || !module)
        return;

    for (int i = 0; i < module->object_shape_count; i++) {
        const XaBuiltinObjectShape *object_shape = &module->object_shapes[i];
        if (!object_shape->name ||
            xa_scope_lookup_local(analyzer->global_scope, object_shape->name))
            continue;
        XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, object_shape->name, XA_SYM_TYPE_ALIAS);
        if (!sym)
            return;
        sym->location.line = 0;
        sym->is_builtin = true;
        sym->is_const = true;
        sym->is_exported = true;
        sym->alias_type = xa_builtin_object_shape_decl_type(analyzer->isolate, analyzer, object_shape);
        if (xa_scope_add_symbol(analyzer->global_scope, sym) != XR_COMPILE_RESOURCE_OK) {
            xa_symbol_free(sym);
            return;
        }
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (links) {
            links->type = sym->alias_type;
            links->declared_type = links->type;
            links->is_definitely_assigned = true;
            links->module_name = module_name;
            links->import_member_name = object_shape->name;
        }
    }

    for (int i = 0; i < module->enum_count; i++) {
        const XaBuiltinEnum *enum_decl = &module->enums[i];
        if (!enum_decl->name || xa_scope_lookup_local(analyzer->global_scope, enum_decl->name))
            continue;
        XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, enum_decl->name, XA_SYM_ENUM);
        if (!sym)
            return;
        sym->location.line = 0;
        sym->is_builtin = true;
        sym->is_const = true;
        sym->is_exported = true;
        if (xa_scope_add_symbol(analyzer->global_scope, sym) != XR_COMPILE_RESOURCE_OK) {
            xa_symbol_free(sym);
            return;
        }
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (links) {
            links->type =
                xa_builtin_enum_decl_type(analyzer->isolate, analyzer, enum_decl, &links->enum_info);
            links->declared_type = links->type;
            links->is_definitely_assigned = true;
            links->module_name = module_name;
            links->import_member_name = enum_decl->name;
        }
    }
}

/* Native classes that are legal inheritance roots need ordinary analyzer
 * class metadata, not only the XaBuiltinType signature table used for direct
 * member calls.  Without this bridge `PanicInfo.message` works while
 * `class E extends PanicInfo; e.message` fails: the subclass has no base
 * XrClassInfo to traverse.  Materialise the metadata once in the analyzer's
 * builtin scope so inheritance, visibility, selections and Xi lowering all
 * observe one canonical parent graph. */
static void register_inheritable_builtin_class(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name || xa_scope_lookup_local(analyzer->global_scope, name))
        return;

    const XaBuiltinType *builtin = xa_builtin_get_by_name(name);
    if (!builtin || !builtin->members || builtin->member_count <= 0)
        return;

    XaSymbol *class_sym = xa_symbol_new(analyzer->symbol_registry, name, XA_SYM_CLASS);
    if (!class_sym)
        return;
    class_sym->location.line = 0;
    class_sym->is_builtin = true;
    class_sym->is_const = true;
    if (xa_scope_add_symbol(analyzer->global_scope, class_sym) != XR_COMPILE_RESOURCE_OK) {
        xa_symbol_free(class_sym);
        return;
    }

    XaSymbolLinks *class_links = xa_analyzer_get_links(analyzer, class_sym);
    if (!class_links) return;
    XrClassInfo *info = xa_class_info_new(xr_compile_session_compile_state(analyzer->compiler_session), name);
    if (!info) return;
    XaScope *class_scope = xa_scope_new(analyzer->symbol_registry, XA_SCOPE_CLASS, analyzer->global_scope);
    if (!class_scope) {
        xa_class_info_free(info);
        (void) xr_compile_state_fail(xr_compile_session_compile_state(analyzer->compiler_session),
            XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        return;
    }

    class_scope->class_symbol = class_sym;
    info->scope = class_scope;
    class_links->class_info = info;
    class_links->owns_class_info = true;
    /* Keep the builtin type token declaration-identity-free.  User classes
     * carry class_ref; native builtin identity is deliberately class_ref==NULL. */
    class_links->type = xr_type_new_class(analyzer->isolate, name);
    class_links->declared_type = class_links->type;
    class_links->is_definitely_assigned = true;

    for (int i = 0; i < builtin->member_count; i++) {
        const XaBuiltinMember *member = &builtin->members[i];
        if (!member->name || member->is_static || member->is_internal)
            continue;

        XaSymbol *member_sym =
            xa_symbol_new(analyzer->symbol_registry, member->name, member->is_method ? XA_SYM_METHOD : XA_SYM_PROPERTY);
        if (!member_sym)
            return;
        member_sym->location.line = 0;
        member_sym->is_builtin = true;
        if (xa_scope_add_symbol(class_scope, member_sym) != XR_COMPILE_RESOURCE_OK) {
            xa_symbol_free(member_sym);
            return;
        }

        XaSymbolLinks *member_links = xa_analyzer_get_links(analyzer, member_sym);
        if (!member_links)
            continue;
        if (member->is_method) {
            member_links->type = xa_builtin_parse_full_signature(analyzer, member->signature);
            if (xa_class_info_add_method(xr_compile_session_compile_state(analyzer->compiler_session), info, member_sym) != XR_COMPILE_RESOURCE_OK) return;
            if (strcmp(member->name, "init") == 0 && member_links->type &&
                XR_TYPE_IS_FUNCTION(member_links->type)) {
                info->has_constructor = true;
                info->constructor_param_count = member_links->type->function.param_count;
                info->constructor_required_params = member_links->type->function.min_params;
                if (info->constructor_param_count > 0) {
                    info->constructor_params =
                        xr_calloc((size_t) info->constructor_param_count, sizeof(XrType *));
                    for (int p = 0; p < info->constructor_param_count; p++)
                        info->constructor_params[p] = member_links->type->function.params[p].type;
                }
            }
        } else {
            const char *type_text = member->signature;
            if (type_text && type_text[0] == ':')
                type_text++;
            while (type_text && *type_text == ' ')
                type_text++;
            member_links->type = xa_builtin_parse_type_string(analyzer, type_text);
            if (xa_class_info_add_field(xr_compile_session_compile_state(analyzer->compiler_session), info, member_sym) != XR_COMPILE_RESOURCE_OK) return;
        }
        if (!member_links->type)
            member_links->type = xr_type_new_unknown(analyzer->isolate);
        member_links->declared_type = member_links->type;
        member_links->is_definitely_assigned = true;
    }
}

// Register all Codegen builtin functions/constructors that are available at runtime
static void xa_register_codegen_builtins(XaAnalyzer *analyzer) {
    // Reusable param types
    XrType *p_any = xr_type_new_unknown(NULL);
    XrType *t_void = xr_type_new_unit(NULL);
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_float = xr_type_new_float(NULL);
    XrType *t_string = xr_type_new_string(NULL);
    XrType *t_bool = xr_type_new_bool(NULL);
    XrType *t_char = xr_type_new_rune(NULL);

    register_exact_scalar_namespaces(analyzer);

    /* Assertions are direct compiler intrinsics with exact surface shapes.
     * `assertEqual`'s same-T relation is checked at the resolved call site;
     * unknown here means a value of any one inferred type, not truthiness. */
    XrType *assert_params[2] = {t_bool, t_string};
    XrType *fn_assert = xr_type_new_function(analyzer->isolate, assert_params, 2, t_void, false);
    fn_assert->function.min_params = 1;
    register_core_builtin_func(analyzer, XR_CORE_BUILTIN_ASSERT, fn_assert);

    XrType *equal_params[3] = {p_any, p_any, t_string};
    XrType *fn_assert_equal =
        xr_type_new_function(analyzer->isolate, equal_params, 3, t_void, false);
    fn_assert_equal->function.min_params = 2;
    register_core_builtin_func(analyzer, XR_CORE_BUILTIN_ASSERT_EQUAL, fn_assert_equal);

    XrType *action_type = xr_type_new_function(analyzer->isolate, NULL, 0, p_any, false);
    XrType *action_params[2] = {action_type, t_string};
    XrType *fn_assert_action =
        xr_type_new_function(analyzer->isolate, action_params, 2, t_void, false);
    fn_assert_action->function.min_params = 1;
    register_core_builtin_func(analyzer, XR_CORE_BUILTIN_ASSERT_THROWS, fn_assert_action);
    register_core_builtin_func(analyzer, XR_CORE_BUILTIN_ASSERT_PANICS, fn_assert_action);

    // The exact numeric surface uses `as` for numeric conversion and
    // i64/f64.parse for text. No numeric type name is a global callable.
    XrType *fn_to_string = xr_type_new_function(analyzer->isolate, &p_any, 1, t_string, false);
    register_builtin_func(analyzer, "string", fn_to_string);
    XrType *fn_to_bool = xr_type_new_function(analyzer->isolate, &p_any, 1, t_bool, false);
    register_builtin_func(analyzer, "bool", fn_to_bool);
    // rune(n): checked Unicode scalar construction.
    XrType *fn_to_rune = xr_type_new_function(analyzer->isolate, &p_any, 1, t_char, false);
    register_builtin_func(analyzer, "rune", fn_to_rune);

    // Type constructors: fn(...any) -> Container
    XrType *fn_array = xr_type_new_function(analyzer->isolate, NULL, 0,
                                            xr_type_new_array(analyzer->isolate, p_any), true);
    fn_array->function.min_params = 0;
    register_builtin_func(analyzer, "Array", fn_array);
    XrType *fn_map = xr_type_new_function(analyzer->isolate, NULL, 0,
                                          xr_type_new_map(analyzer->isolate, p_any, p_any), true);
    fn_map->function.min_params = 0;
    register_builtin_func(analyzer, "Map", fn_map);
    XrType *fn_set = xr_type_new_function(analyzer->isolate, NULL, 0,
                                          xr_type_new_set(analyzer->isolate, p_any), true);
    fn_set->function.min_params = 0;
    register_builtin_func(analyzer, "Set", fn_set);

    // typeOf: fn(any) -> int (returns stable XrTypeId for fast Type.xxx comparison)
    XrType *fn_typeof = xr_type_new_function(analyzer->isolate, &p_any, 1, t_int, false);
    register_builtin_func(analyzer, "typeOf", fn_typeof);
    // typeName: fn(any) -> string (cold/debug type display name)
    XrType *fn_typename = xr_type_new_function(analyzer->isolate, &p_any, 1, t_string, false);
    register_builtin_func(analyzer, "typeName", fn_typename);
    // len: compiler-known query; operand support is checked by xa_visit_call.
    XrType *fn_len = xr_type_new_function(analyzer->isolate, &p_any, 1, t_int, false);
    register_core_builtin_func(analyzer, XR_CORE_BUILTIN_LEN, fn_len);
    // chr: fn(int) -> string
    XrType *fn_chr = xr_type_new_function(analyzer->isolate, &t_int, 1, t_string, false);
    register_builtin_func(analyzer, "chr", fn_chr);
    // copy: fn(any) -> any (preserves unknown type)
    XrType *fn_copy = xr_type_new_function(analyzer->isolate, &p_any, 1, p_any, false);
    register_builtin_func(analyzer, "copy", fn_copy);
    // dump: fn(any, ...) -> void
    XrType *fn_dump = xr_type_new_function(analyzer->isolate, &p_any, 1, t_void, true);
    register_builtin_func(analyzer, "dump", fn_dump);
    // print: fn(...any) -> void
    XrType *fn_print = xr_type_new_function(analyzer->isolate, NULL, 0, t_void, true);
    fn_print->function.min_params = 0;
    register_core_builtin_func(analyzer, XR_CORE_BUILTIN_PRINT, fn_print);

    // Modules/namespaces (XA_SYM_MODULE enables member signature lookup)
    register_builtin_module(analyzer, "JSON");
    register_builtin_module(analyzer, "Coro");
    register_builtin_module_types_in_prelude(analyzer, "Coro");
    register_builtin_module(analyzer, "CoroPool");
    register_builtin_module(analyzer, "Channel");
    register_target_query_namespace(analyzer);

    register_inheritable_builtin_class(analyzer, "PanicInfo");

    // Runtime global variables materialized from the full-runtime configuration.
    register_builtin_var(analyzer, "process", p_any, true);
    register_builtin_var(analyzer, "__file__", t_string, true);
    register_builtin_var(analyzer, "__dir__", t_string, true);
}

// Register a prelude enum symbol into the analyzer's global scope so it is
// visible in every compilation unit (entry file and imported modules alike)
// with a single canonical identity — mirroring how the runtime binds the
// matching XrEnumType into a builtin global slot.  member_names are stable
// string literals; payload_counts is copied (NULL for simple enums).
static void register_prelude_enum_full(XaAnalyzer *analyzer, const char *name,
                                       const char **type_param_names, int type_param_count,
                                       const char **member_names, int member_count,
                                       const int *payload_counts, const char **payload_names,
                                       XrType ***payload_types, bool is_adt) {
    XR_DCHECK(analyzer != NULL, "register_prelude_enum: NULL analyzer");
    XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, name, XA_SYM_ENUM);
    if (!sym)
        return;
    sym->location.line = 0;
    sym->is_builtin = true;
    sym->is_const = true;
    if (xa_scope_add_symbol(analyzer->global_scope, sym) != XR_COMPILE_RESOURCE_OK) {
        xa_symbol_free(sym);
        return;
    }
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (!links)
        return;
    links->type = xr_type_new_enum(analyzer->isolate, name);
    if (!links->type)
        return;
    XrClassInfo *nominal = xa_class_info_new(xr_compile_session_compile_state(analyzer->compiler_session), name);
    if (!nominal)
        return;
    nominal->declaration_symbol = sym;
    nominal->nominal_kind = XA_NOMINAL_ENUM;
    links->class_info = nominal;
    links->owns_class_info = true;
    links->type->enum_type.nominal_ref = nominal;
    links->declared_type = links->type;
    links->is_definitely_assigned = true;
    if (type_param_count > 0 && type_param_names) {
        if (xa_symbol_links_set_type_params(links, type_param_names, NULL, NULL, type_param_count) != XR_COMPILE_RESOURCE_OK) {
            return;
        }
    }

    if (member_count > 0) {
        XrCompileState *state = xa_symbol_registry_state(analyzer->symbol_registry);
        if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return;
        XrOsIoPolicy policy = xr_compile_io_policy(xr_compile_state_resources(state));
        XaEnumInfo *info = NULL;
        if (xa_enum_capture_status(state, xa_enum_info_new(&policy, "prelude", name,
                (uint32_t)member_count, &info)) != XR_COMPILE_RESOURCE_OK) return;
        for (int i = 0; i < member_count; ++i) {
            XaEnumVariantInfo *variant = &info->variants[i];
            if (xa_enum_capture_status(state, xa_enum_variant_set_name(variant, member_names[i])) != XR_COMPILE_RESOURCE_OK) goto enum_failed;
            int count = is_adt && payload_counts ? payload_counts[i] : 0;
            if (count < 0 || count > UINT16_MAX) {
                (void)xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT); goto enum_failed;
            }
            variant->payload_count = (uint16_t)count;
            if (count > 0 && payload_types && payload_types[i]) {
                const char *names[] = {payload_names ? payload_names[i] : NULL};
                if (xa_enum_capture_status(state, xa_enum_variant_set_payload_names(variant, names, 1)) != XR_COMPILE_RESOURCE_OK) goto enum_failed;
                if (xa_enum_capture_status(state, xa_enum_variant_set_payload_types(variant, payload_types[i], (uint16_t)count)) != XR_COMPILE_RESOURCE_OK) goto enum_failed;
            }
        }
        if (xa_enum_capture_status(state, xa_enum_info_finalize_layout(info)) != XR_COMPILE_RESOURCE_OK) goto enum_failed;
        links->enum_info = info; links->type->enum_type.layout = info->layout;
        links->type->enum_type.layout_id = info->layout ? info->layout->layout_id : 0;
        links->declared_type = links->type;
        return;
enum_failed:
        xa_enum_info_free(info);
    }
}

/* Prelude enum registry, generated from builtin_symbols.def. A variant payload
 * is either absent, typed as the enum's first type parameter, or an error
 * value; those three shapes cover every prelude enum. */
typedef enum {
    XA_PRELUDE_PAYLOAD_NONE,
    XA_PRELUDE_PAYLOAD_TYPE_PARAM_0,
    XA_PRELUDE_PAYLOAD_ERROR,
} XaPreludePayloadKind;

typedef struct {
    const char *name;
    uint8_t payload;
    const char *payload_name;
} XaPreludeVariantDesc;

typedef struct {
    const char *name;
    int arity;
    int variant_count;
} XaPreludeEnumDesc;

/* All variants of all prelude enums, flattened in declaration order. Each
 * enum's slice starts where the previous one ended. */
static const XaPreludeVariantDesc g_prelude_enum_variants[] = {
#define XR_BUILTIN_ENUM(ename, earity, evm_slot, evariants) evariants
#define XR_BUILTIN_ENUM_VARIANT(vname, payload)                                                    \
    {(vname), XA_PRELUDE_PAYLOAD_##payload, XA_PRELUDE_PAYLOAD_NAME_##payload},
#define XA_PRELUDE_PAYLOAD_NAME_NONE NULL
#define XA_PRELUDE_PAYLOAD_NAME_TYPE_PARAM_0 "value"
#define XA_PRELUDE_PAYLOAD_NAME_ERROR "error"
#include "../../../stdlib/prelude/builtin_symbols.def"
};

static const XaPreludeEnumDesc g_prelude_enums[] = {
#define XR_BUILTIN_ENUM(ename, earity, evm_slot, evariants)                                        \
    {(ename), (earity),                                                                            \
     (int) (sizeof((const XaPreludeVariantDesc[]) {evariants}) / sizeof(XaPreludeVariantDesc))},
#define XR_BUILTIN_ENUM_VARIANT(vname, payload)                                                    \
    {(vname), XA_PRELUDE_PAYLOAD_##payload, XA_PRELUDE_PAYLOAD_NAME_##payload},
#include "../../../stdlib/prelude/builtin_symbols.def"
};

#undef XA_PRELUDE_PAYLOAD_NAME_NONE
#undef XA_PRELUDE_PAYLOAD_NAME_TYPE_PARAM_0
#undef XA_PRELUDE_PAYLOAD_NAME_ERROR

#define XA_PRELUDE_ENUM_COUNT (sizeof(g_prelude_enums) / sizeof(g_prelude_enums[0]))
/* Widest prelude enum; sizes the per-enum scratch arrays below. */
#define XA_PRELUDE_ENUM_MAX_VARIANTS 8

// Register prelude enums so they are available without a
// per-module declaration.  The runtime binds the matching canonical
// XrEnumType into builtin VM slots.
static void xa_register_prelude_enums(XaAnalyzer *analyzer) {
    static const char *const type_param_names[] = {"T"};
    int variant_base = 0;

    for (size_t e = 0; e < XA_PRELUDE_ENUM_COUNT; e++) {
        const XaPreludeEnumDesc *desc = &g_prelude_enums[e];
        const XaPreludeVariantDesc *variants = &g_prelude_enum_variants[variant_base];
        variant_base += desc->variant_count;

        XR_DCHECK(desc->variant_count <= XA_PRELUDE_ENUM_MAX_VARIANTS,
                  "prelude enum exceeds XA_PRELUDE_ENUM_MAX_VARIANTS");
        if (desc->variant_count > XA_PRELUDE_ENUM_MAX_VARIANTS)
            continue;

        const char *member_names[XA_PRELUDE_ENUM_MAX_VARIANTS];
        int payload_counts[XA_PRELUDE_ENUM_MAX_VARIANTS];
        const char *payload_names[XA_PRELUDE_ENUM_MAX_VARIANTS];
        XrType *payload_slots[XA_PRELUDE_ENUM_MAX_VARIANTS];
        XrType **payload_types[XA_PRELUDE_ENUM_MAX_VARIANTS];
        bool is_adt = false;

        for (int v = 0; v < desc->variant_count; v++) {
            member_names[v] = variants[v].name;
            switch ((XaPreludePayloadKind) variants[v].payload) {
                case XA_PRELUDE_PAYLOAD_TYPE_PARAM_0:
                    payload_slots[v] = xr_type_new_type_param(analyzer->isolate, "T", 0);
                    break;
                case XA_PRELUDE_PAYLOAD_ERROR:
                    /* Reuse the canonical interface type that
                     * xa_register_builtin_interfaces already put in the global
                     * scope. Minting a second one with the same name here left
                     * two `Error` types in play and the source spelling stopped
                     * resolving at all. */
                    payload_slots[v] =
                        (XrType *) xa_scope_resolve_type_alias(analyzer->global_scope, "Error");
                    XR_DCHECK(payload_slots[v] != NULL,
                              "prelude ERROR payload before the Error interface is registered");
                    break;
                case XA_PRELUDE_PAYLOAD_NONE:
                default:
                    payload_slots[v] = NULL;
                    break;
            }
            payload_counts[v] = payload_slots[v] ? 1 : 0;
            payload_names[v] = variants[v].payload_name;
            payload_types[v] = payload_slots[v] ? &payload_slots[v] : NULL;
            is_adt = is_adt || payload_counts[v] > 0;
        }

        register_prelude_enum_full(
            analyzer, desc->name, desc->arity > 0 ? (const char **) type_param_names : NULL,
            desc->arity, member_names, desc->variant_count, is_adt ? payload_counts : NULL,
            is_adt ? payload_names : NULL, is_adt ? payload_types : NULL, is_adt);
    }
}

/* TargetMachine is the sole source of the public target enum domains.  The
 * generated registry supplies both these analyzer declarations and the
 * runtime numeric constants, so source members cannot drift from the values
 * returned by target queries. */
static void xa_register_target_query_enums(XaAnalyzer *analyzer) {
    for (size_t query = 0; query < XR_TARGET_QUERY_ENUM_COUNT; ++query) {
        const XrTargetQueryEnumDesc *desc = &xr_target_query_enum_rows[query];
        const char *member_names[9];
        XR_DCHECK(desc->member_count <= sizeof(member_names) / sizeof(member_names[0]),
                  "target query enum exceeds registry scratch capacity");
        if (desc->member_count > sizeof(member_names) / sizeof(member_names[0]))
            continue;
        for (size_t member = 0; member < desc->member_count; ++member)
            member_names[member] = desc->members[member].source_name;
        register_prelude_enum_full(analyzer, desc->type_name, NULL, 0, member_names,
                                   (int) desc->member_count, NULL, NULL, NULL, false);
    }
}

static XrCompileResourceStatus analyzer_copy_identity(XaAnalyzer *analyzer,
                                                        const char *identity) {
    XrCompileState *state = xr_compile_session_compile_state(analyzer->compiler_session);
    if (!identity)
        return xr_compile_state_status(state);
    size_t length = 0;
    XrCompileResourceStatus status = xr_compile_state_string_length(state, identity, &length);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    XrModuleIdentityKind kind;
    if (!xr_module_identity_valid(identity, &kind))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    status = xr_compile_state_strndup(state, identity, length, &analyzer->standalone_module_identity);
    if (status != XR_COMPILE_RESOURCE_OK || kind != XR_MODULE_IDENTITY_STDLIB)
        return status;
    const char *namespace_id = NULL;
    size_t namespace_length = 0;
    if (!xr_module_identity_stdlib_namespace(identity, &namespace_id, &namespace_length))
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    status = xr_compile_state_strndup(state, namespace_id, namespace_length,
                                    &analyzer->standalone_stdlib_module_name);
    if (status == XR_COMPILE_RESOURCE_OK)
        analyzer->standalone_module_is_stdlib = true;
    return status;
}

XR_FUNC XaAnalyzer *xa_analyzer_new(const XaAnalyzerInput *input) {
    if (!input || !input->session)
        return NULL;
    XrCompilerSession *session = input->session;
    XrCompileState *state = xr_compile_session_compile_state(session);
    if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK)
        return NULL;
    if (!input->runtime || !xr_target_data_layout_validate(input->target)) {
        (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return NULL;
    }
    void *memory = NULL;
    if (xr_compile_state_calloc(state, 1, sizeof(XaAnalyzer), &memory) != XR_COMPILE_RESOURCE_OK)
        return NULL;
    XaAnalyzer *analyzer = memory;
    analyzer->compiler_session = session;
    analyzer->isolate = input->runtime;
    analyzer->target_data_layout = *input->target;
    analyzer->semantic_revision = 1;
    if (analyzer_copy_identity(analyzer, input->module_identity) != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(analyzer->standalone_module_identity);
        xr_compile_state_free(analyzer->standalone_stdlib_module_name);
        xr_compile_state_free(analyzer);
        return NULL;
    }
    xr_type_global_init();
    XrVMRuntime *X = input->runtime;

    if (xa_analyzer_storage_open(xr_compile_session_compile_state(session), analyzer) !=
        XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(analyzer->standalone_module_identity);
        xr_compile_state_free(analyzer->standalone_stdlib_module_name);
        xr_compile_state_free(analyzer);
        return NULL;
    }

    // Install the analyzer-owned pool for type allocation in this compiler pass.
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->type_pool->next_type_id);
    if (xa_analyzer_files_open(analyzer) != XR_COMPILE_RESOURCE_OK) {
        xa_analyzer_free(analyzer);
        return NULL;
    }

    // Register built-in interfaces (Iterable, Comparable, etc.)
    xa_register_builtin_interfaces(X, analyzer->global_scope);

    // Register Codegen builtin functions/constructors
    xa_register_codegen_builtins(analyzer);

    // Register prelude enums (Ordering) with single canonical identity,
    // visible in every compilation unit.
    xa_register_prelude_enums(analyzer);
    xa_register_target_query_enums(analyzer);
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) {
        xa_analyzer_free(analyzer);
        return NULL;
    }

    // Default options. Strict null checks are ON by default: a possibly-null
    // value must be narrowed before member/index/call access (best-practice
    // null safety; no silent runtime null-panic escapes the type checker).
    analyzer->strict_null_checks = true;
    analyzer->strict_mode = false;
    analyzer->infer_return_types = true;
    analyzer->build_profile = XA_ANALYZER_BUILD_PROFILE_HOSTED;

    // Initialize incremental analysis support
    analyzer->incremental = xa_incremental_new();

    // AST -> inferred-type side table.
    analyzer->node_table = xa_node_table_new();

    // AST -> selection facts table (member/method/index resolution).
    analyzer->selection_table = xa_selection_table_new();

    // AST call -> resolved stdlib parallel intrinsic identity.
    analyzer->parallel_call_plan_table = xa_parallel_call_plan_table_new();

    // Named enum record syntax -> immutable declaration-slot plans.
    analyzer->enum_record_plan_table = xa_enum_record_plan_table_new();

    // AST call -> canonical resolved call identity.
    analyzer->resolved_call_table = xa_resolved_call_table_new();

    // Canonical typed-error effect summaries.
    analyzer->effect_db = xa_effect_db_new();

    // Canonical root-relative memory effects.
    analyzer->memory_effect_db = xa_memory_effect_db_new();

    // Canonical allocation-effect summaries.
    analyzer->allocation_db = xa_allocation_db_new();

    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK ||
        !analyzer->enum_record_plan_table || !analyzer->effect_db || !analyzer->memory_effect_db ||
        !analyzer->allocation_db) {
        xa_analyzer_free(analyzer);
        return NULL;
    }

    return analyzer;
}

// Free analyzer
void xa_analyzer_free(XaAnalyzer *analyzer) {
    if (!analyzer)
        return;

    xa_analyzer_files_close(analyzer);

    /* Release enum layouts retired by re-analysis (kept alive so cached
     * XrType copies stayed valid across the second analysis pass). */
    for (size_t i = 0; i < analyzer->retired_enum_layout_count; i++)
        xr_enum_layout_free((XrEnumLayout *) analyzer->retired_enum_layouts[i]);
    xr_free(analyzer->retired_enum_layouts);
    analyzer->retired_enum_layouts = NULL;
    analyzer->retired_enum_layout_count = 0;
    analyzer->retired_enum_layout_cap = 0;

    XaForeignSymbolView *foreign_view = (XaForeignSymbolView *) analyzer->foreign_symbol_views;
    while (foreign_view) {
        XaForeignSymbolView *next = foreign_view->next;
        xr_compile_state_free(foreign_view);
        foreign_view = next;
    }
    analyzer->foreign_symbol_views = NULL;
    xa_scope_free(analyzer->foreign_symbol_scope);
    analyzer->foreign_symbol_scope = NULL;

    xa_scope_free(analyzer->global_scope);
    analyzer->global_scope = NULL;
    analyzer->current_scope = NULL;

    // Free incremental analysis context
    if (analyzer->incremental) {
        xa_incremental_free(analyzer->incremental);
    }

    // Free the AST -> inferred-type side table.
    if (analyzer->node_table) {
        xa_node_table_free((XaNodeTable *) analyzer->node_table);
        analyzer->node_table = NULL;
    }

    // Free the selection facts table.
    if (analyzer->selection_table) {
        xa_selection_table_free((XaSelectionTable *) analyzer->selection_table);
        analyzer->selection_table = NULL;
    }

    // Free the parallel call plan table.
    if (analyzer->parallel_call_plan_table) {
        xa_parallel_call_plan_table_free(
            (XaParallelCallPlanTable *) analyzer->parallel_call_plan_table);
        analyzer->parallel_call_plan_table = NULL;
    }

    if (analyzer->enum_record_plan_table) {
        xa_enum_record_plan_table_free((XaEnumRecordPlanTable *) analyzer->enum_record_plan_table);
        analyzer->enum_record_plan_table = NULL;
    }

    if (analyzer->resolved_call_table) {
        xa_resolved_call_table_free((XaResolvedCallTable *) analyzer->resolved_call_table);
        analyzer->resolved_call_table = NULL;
    }

    if (analyzer->effect_db) {
        xa_effect_db_free(analyzer->effect_db);
        analyzer->effect_db = NULL;
    }

    if (analyzer->memory_effect_db) {
        xa_memory_effect_db_free(analyzer->memory_effect_db);
        analyzer->memory_effect_db = NULL;
    }

    if (analyzer->allocation_db) {
        xa_allocation_db_free(analyzer->allocation_db);
        analyzer->allocation_db = NULL;
    }

    /* A parser session owns no fallback analyzer. Subsequent semantic work
     * selects its actual analyzer pool instead of restoring an unowned one. */
    if (analyzer->type_pool && xr_type_get_current_pool() == analyzer->type_pool)
        xr_type_set_current_pool(NULL, NULL);

    xa_analyzer_clear_diagnostics(analyzer);

    xr_compile_state_free(analyzer->standalone_module_identity);
    xr_compile_state_free(analyzer->standalone_stdlib_module_name);
    xa_analyzer_storage_close(analyzer);

    xr_compile_state_free(analyzer);
}

size_t xa_analyzer_type_pool_bytes(const XaAnalyzer *analyzer) {
    if (!analyzer || !analyzer->type_pool)
        return 0;
    return xr_arena_get_allocated_size(&analyzer->type_pool->arena);
}

// Configuration
void xa_analyzer_set_strict_null(XaAnalyzer *analyzer, bool enable) {
    if (analyzer)
        analyzer->strict_null_checks = enable;
}

void xa_analyzer_set_strict_mode(XaAnalyzer *analyzer, bool enable) {
    if (analyzer)
        analyzer->strict_mode = enable;
}

void xa_analyzer_set_build_profile(XaAnalyzer *analyzer, XaAnalyzerBuildProfile profile) {
    if (analyzer)
        analyzer->build_profile = profile;
}

bool xa_analyzer_is_freestanding(const XaAnalyzer *analyzer) {
    return analyzer && analyzer->build_profile == XA_ANALYZER_BUILD_PROFILE_FREESTANDING;
}

void xa_analyzer_set_graph(XaAnalyzer *analyzer, struct XrModuleGraph *graph) {
    if (!analyzer)
        return;
    if (analyzer->graph != graph) {
        xa_node_table_clear_type_ref_types((XaNodeTable *) analyzer->node_table);
        xa_node_table_clear_generic_specializations((XaNodeTable *) analyzer->node_table);
        analyzer->type_ref_batch_root_id = 0u;
    }
    analyzer->graph = graph;
}

// Symbol lookup
XaSymbol *xa_analyzer_lookup(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return NULL;
    return xa_scope_lookup(analyzer->current_scope, name);
}

XaSymbol *xa_analyzer_lookup_in_scope(XaAnalyzer *analyzer, const char *name, XaScope *scope) {
    if (!analyzer || !name)
        return NULL;
    return xa_scope_lookup(scope ? scope : analyzer->current_scope, name);
}

// Recursive deep search with position awareness
// Collects all matches, caller picks the best one
static void lookup_deep_collect(XaScope *scope, const char *name, XaSymbol **results, int *count,
                                int max) {
    if (!scope || *count >= max)
        return;

    XaSymbol *local = xa_scope_lookup_local(scope, name);
    if (local) {
        results[*count] = local;
        (*count)++;
    }

    for (int i = 0; i < scope->child_count && *count < max; i++) {
        lookup_deep_collect(scope->children[i], name, results, count, max);
    }
}

XaSymbol *xa_analyzer_lookup_deep(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name || !analyzer->global_scope)
        return NULL;

    XaSymbol *results[32];
    int count = 0;
    lookup_deep_collect(analyzer->global_scope, name, results, &count, 32);

    if (count == 0)
        return NULL;
    if (count == 1)
        return results[0];

    // Multiple matches: return the one with the highest line number
    // (innermost / latest declaration is most likely the intended one)
    XaSymbol *best = results[0];
    for (int i = 1; i < count; i++) {
        if (results[i]->location.line > best->location.line) {
            best = results[i];
        }
    }
    return best;
}


XaSymbol *xa_analyzer_import_export_symbol(XaAnalyzer *analyzer, const XaSymbol *source) {
    if (!analyzer) return NULL;
    XrCompileState *state = xa_symbol_registry_state(analyzer->symbol_registry);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!source) {
        (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return NULL;
    }

    const XaAnalyzer *source_owner = source->links.summary_owner;
    if (source_owner == analyzer)
        return (XaSymbol *) source;

    const uint64_t source_revision = source_owner ? source_owner->semantic_revision : 0;
    for (XaForeignSymbolView *entry = (XaForeignSymbolView *) analyzer->foreign_symbol_views; entry;
         entry = entry->next) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return NULL;
        if (entry->source == source && entry->source_owner == source_owner &&
            entry->source_revision == source_revision)
            return entry->view;
    }

    if (!analyzer->foreign_symbol_scope) {
        /* Keep declaration-context views outside the lexical scope tree.
         * Their fresh ids are resolved through the explicit registry only; exposing
         * their source names to deep lookup would make an unimported foreign
         * declaration appear visible to LSP and recovery paths. */
        analyzer->foreign_symbol_scope = xa_scope_new(analyzer->symbol_registry, XA_SCOPE_GLOBAL, NULL);
        if (!analyzer->foreign_symbol_scope)
            return NULL;
    }

    XaSymbol *view = xa_symbol_new(analyzer->symbol_registry, source->name, source->kind);
    if (!view)
        return NULL;
    view->location = source->location;
    view->is_const = source->is_const;
    view->is_weak = source->is_weak;
    view->is_rebindable = source->is_rebindable;
    view->is_readonly_binding = source->is_readonly_binding;
    view->is_exported = source->is_exported;
    view->is_static = source->is_static;
    view->is_private = source->is_private;
    view->is_protected = source->is_protected;
    view->is_override = source->is_override;
    view->is_imported = true;
    view->is_builtin = source->is_builtin;
    view->receiver_mode = source->receiver_mode;
    view->has_declared_default = source->has_declared_default;
    view->passing_mode = source->passing_mode;
    view->type_alias_node = source->type_alias_node;
    view->alias_type = source->alias_type;
    xa_symbol_links_copy_export_metadata(analyzer, &view->links, &source->links);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) {
        xa_symbol_free(view);
        return NULL;
    }
    void *memory = NULL;
    XrCompileResourceStatus status = xr_compile_state_calloc(state, 1,
        sizeof(XaForeignSymbolView), &memory);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 5);
    if (status == XR_COMPILE_RESOURCE_OK)
        status = xa_scope_add_symbol(analyzer->foreign_symbol_scope, view);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_free(memory);
        xa_symbol_free(view);
        return NULL;
    }
    XaForeignSymbolView *entry = memory;
    entry->source = source;
    entry->source_owner = source_owner;
    entry->source_revision = source_revision;
    entry->view = view;
    entry->next = (XaForeignSymbolView *) analyzer->foreign_symbol_views;
    analyzer->foreign_symbol_views = entry;
    return view;
}

bool xa_symbol_is_module(XaAnalyzer *analyzer, XaSymbol *symbol, const char *module_name) {
    if (!analyzer || !symbol || !module_name || symbol->kind != XA_SYM_MODULE)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, symbol);
    return links && links->module_name && strcmp(links->module_name, module_name) == 0;
}

bool xa_symbol_is_builtin_module(XaAnalyzer *analyzer, XaSymbol *symbol, const char *module_name) {
    /* register_builtin_module() is the only producer of this shape: a global
     * XA_SYM_MODULE symbol flagged built-in whose module name is its own name. */
    return symbol && symbol->is_builtin && xa_symbol_is_module(analyzer, symbol, module_name);
}

// Get type of symbol (lazy computation)
XrType *xa_analyzer_get_type(XaAnalyzer *analyzer, XaSymbol *symbol) {
    if (!analyzer || !symbol)
        return NULL;

    // Ensure type pool is set for this thread
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->type_pool->next_type_id);

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, symbol);
    if (!links)
        return xr_type_new_unknown(NULL);

    // Return cached type
    if (links->type)
        return links->type;

    // Return declared type if available
    if (links->declared_type) {
        links->type = links->declared_type;
        return links->type;
    }

    // Fallback: some symbols may not have type after analysis (e.g. forward refs).
    // Return unknown as safe default.
    return xr_type_new_unknown(NULL);
}

// Get class info
XrClassInfo *xa_analyzer_get_class(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return NULL;

    XaSymbol *sym = xa_scope_lookup(analyzer->global_scope, name);
    if (!sym || sym->kind != XA_SYM_CLASS)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    return links ? links->class_info : NULL;
}

XR_FUNC XrCompileResourceStatus xa_analyzer_get_members(
    XaAnalyzer *analyzer, XrType *type, XaMemberList *output) {
    XaSymbolRegistry *registry = analyzer ? analyzer->symbol_registry : NULL;
    XrCompileState *state = xa_symbol_registry_state(registry);
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    if (!output || output->items || output->count || output->owns_symbols)
        return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK || !type)
        return status;
    if (XR_TYPE_IS_INSTANCE(type) && type->instance.class_ref)
        return xa_member_list_class(registry, type->instance.class_ref, output);
    return xa_builtin_get_members(registry, type, output);
}

// Get symbols in scope (includes parent scopes)
XaSymbol **xa_analyzer_get_scope_symbols(XaAnalyzer *analyzer, XaScope *scope, int *count) {
    *count = 0;
    if (!analyzer)
        return NULL;

    XaScope *s = scope ? scope : analyzer->current_scope;
    if (!s)
        return NULL;

    // Single-pass: collect symbols with dynamic array growth
    int capacity = 64;
    int idx = 0;
    XaSymbol **result = xr_malloc(sizeof(XaSymbol *) * capacity);
    if (!result)
        return NULL;

    for (XaScope *p = s; p; p = p->parent) {
        int sc = 0;
        XaSymbol **syms = xa_scope_get_all_symbols(p, &sc);
        if (xa_scope_resource_status(p) != XR_COMPILE_RESOURCE_OK) {
            xr_free(result);
            return NULL;
        }
        if (syms) {
            // Grow if needed
            if (idx + sc > capacity) {
                while (idx + sc > capacity)
                    capacity *= 2;
                XaSymbol **tmp = xr_realloc(result, sizeof(XaSymbol *) * capacity);
                if (!tmp) {
                    xr_free(result);
                    xr_compile_state_free(syms);
                    return NULL;
                }
                result = tmp;
            }
            for (int i = 0; i < sc; i++) {
                result[idx++] = syms[i];
            }
            xr_compile_state_free(syms);
        }
    }

    if (idx == 0) {
        xr_free(result);
        return NULL;
    }

    *count = idx;
    return result;
}

// Diagnostics
// Scope management
// Reuses existing child scope if ast_node matches (Pass 2 reuses Pass 1 scope)


// Type checking
static bool typecheck_source_precise_for_target(XrType *target, XrType *source) {
    XR_DCHECK(target != NULL, "typecheck_source_precise_for_target: NULL target");
    XR_DCHECK(source != NULL, "typecheck_source_precise_for_target: NULL source");
    if (!target || !source)
        return false;

    if (XR_TYPE_IS_UNKNOWN(target))
        return true;
    if (XR_TYPE_IS_UNKNOWN(source))
        return false;
    return true;
}

bool xa_recovery_compatible(XrType *target, XrType *source) {
    XR_DCHECK(target != NULL, "xa_recovery_compatible: NULL target");
    XR_DCHECK(source != NULL, "xa_recovery_compatible: NULL source");
    if (!target || !source)
        return false;
    if (xr_type_contains_error(target) || xr_type_contains_error(source))
        return true;
    return xr_type_assignable(target, source);
}

bool xa_typecheck_assignable(XrType *target, XrType *source) {
    XR_DCHECK(target != NULL, "xa_typecheck_assignable: NULL target");
    XR_DCHECK(source != NULL, "xa_typecheck_assignable: NULL source");
    if (!target || !source)
        return false;
    if (!xa_recovery_compatible(target, source))
        return false;
    return typecheck_source_precise_for_target(target, source);
}

bool xa_call_arg_type_assignable(XrType *target, XrType *source, XrParamMode mode) {
    if (!target || !source)
        return false;
    if (xa_typecheck_assignable(target, source))
        return true;
    if (mode != XR_PARAM_READ || !source->is_const)
        return false;

    /* READ creates no mutable authority in the callee.  Strip only the
     * expression's top-level readonly view for structural comparison; nested
     * const qualifiers remain part of the type and are checked normally. */
    XrType readable_source = *source;
    readable_source.is_const = false;
    return xa_typecheck_assignable(target, &readable_source);
}

bool xa_analyzer_check_assignment(XaAnalyzer *analyzer, XrType *target, XrType *source,
                                  XrLocation *loc) {
    if (!analyzer || !target || !source)
        return false;

    if (xa_typecheck_assignable(target, source)) {
        return true;
    }

    // Generate error
    char message[256];
    snprintf(message, sizeof(message), "Type '%s' is not assignable to type '%s'",
             xr_type_to_string(source), xr_type_to_string(target));
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, message,
                               loc);

    return false;
}

bool xa_analyzer_check_call(XaAnalyzer *analyzer, XrType *func_type, XrType **arg_types,
                            int arg_count, XrLocation *loc) {
    if (!analyzer || !func_type)
        return false;

    if (!XR_TYPE_IS_FUNCTION(func_type)) {
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   "Value is not callable", loc);
        return false;
    }

    // Check argument count
    int expected = func_type->function.param_count;
    int min_params = func_type->function.min_params;
    bool is_variadic = func_type->function.is_variadic;
    if (arg_count < min_params) {
        char message[128];
        if (is_variadic) {
            snprintf(message, sizeof(message), "Expected at least %d arguments, but got %d",
                     min_params, arg_count);
        } else {
            snprintf(message, sizeof(message), "Expected %d arguments, but got %d", min_params,
                     arg_count);
        }
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   message, loc);
        return false;
    }
    if (arg_count > expected && !is_variadic) {
        char message[128];
        snprintf(message, sizeof(message), "Expected %d arguments, but got %d", expected,
                 arg_count);
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   message, loc);
        return false;
    }

    // Check argument types
    bool ok = true;
    int rest_param_index = (is_variadic && expected > 0) ? expected - 1 : -1;
    int fixed_param_count = rest_param_index >= 0 ? rest_param_index : expected;
    for (int i = 0; i < arg_count; i++) {
        int param_slot = i;
        if (is_variadic && rest_param_index >= 0 && i >= fixed_param_count)
            param_slot = rest_param_index;
        if (param_slot < 0 || param_slot >= expected)
            continue;
        XrType *param_type = xr_type_function_param_type(func_type, param_slot);
        XrParamMode mode = xr_type_function_param_mode(func_type, param_slot);
        if (!xa_call_arg_type_assignable(param_type, arg_types[i], mode)) {
            ok = false;
            char message[256];
            snprintf(message, sizeof(message),
                     "Argument %d: Type '%s' is not assignable to parameter type '%s'", i + 1,
                     xr_type_to_string(arg_types[i]), xr_type_to_string(param_type));
            xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       message, loc);
        }
    }

    return ok;
}

// Full AST analysis
static void remove_file_symbols(XaScope *scope, const char *file, XaAnalyzer *analyzer) {
    if (!scope || !file) return;
    XrCompileState *state = xa_symbol_registry_state(scope->registry);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return;
    XaSymbol *symbol = scope->owned_symbols;
    while (symbol) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return;
        XaSymbol *next = symbol->scope_owned_next;
        XaSymbolLinks *links = &symbol->links;
        bool matches = false;
        if (links->file_path && !xa_export_identity_equal(analyzer, links->file_path, file, &matches))
            return;
        if (matches) {
            if (links->class_info && links->owns_class_info) {
                if (xa_scope_clear_class_base_references(analyzer->global_scope,
                        links->class_info) != XR_COMPILE_RESOURCE_OK) return;
            }
            if (xa_scope_remove_owned_symbol(scope, symbol) != XR_COMPILE_RESOURCE_OK) return;
            if (links->class_info) {
                if (links->owns_class_info) {
                    xa_effect_db_detach_error_types_for_nominal(analyzer->effect_db, links->class_info);
                    xa_class_info_free(links->class_info);
                }
                links->class_info = NULL;
                links->owns_class_info = false;
            }
            xa_symbol_free(symbol);
        }
        symbol = next;
    }
    for (int i = 0; i < scope->child_count; ++i) {
        remove_file_symbols(scope->children[i], file, analyzer);
        if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return;
    }
}

typedef struct XaFileOwnedIds {
    XaAnalyzer *analyzer;
    const char *file;
    uint32_t *ids;
    int count;
    int capacity;
} XaFileOwnedIds;

static bool collect_file_owned_ids(XaScope *scope, XaFileOwnedIds *collect) {
    if (!scope) return true;
    XrCompileState *state = xa_symbol_registry_state(scope->registry);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return false;
    for (XaSymbol *symbol = scope->owned_symbols; symbol; symbol = symbol->scope_owned_next) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return false;
        bool matches = false;
        if (symbol->links.file_path && !xa_export_identity_equal(collect->analyzer,
                symbol->links.file_path, collect->file, &matches)) return false;
        if (!matches) continue;
        if (collect->count == INT_MAX || (collect->ids && collect->count >= collect->capacity)) {
            (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
            return false;
        }
        if (collect->ids) collect->ids[collect->count] = symbol->id;
        ++collect->count;
    }
    for (int i = 0; i < scope->child_count; ++i)
        if (!collect_file_owned_ids(scope->children[i], collect)) return false;
    return true;
}

static bool xa_path_char_matches(char a, char b) {
    if (a == b)
        return true;
    return (a == '/' || a == '\\') && (b == '/' || b == '\\');
}

static bool xa_path_has_suffix(const char *file, const char *suffix) {
    if (!file || !suffix)
        return false;
    size_t flen = strlen(file);
    size_t slen = strlen(suffix);
    if (flen < slen)
        return false;
    const char *tail = file + flen - slen;
    for (size_t i = 0; i < slen; i++) {
        if (!xa_path_char_matches(tail[i], suffix[i]))
            return false;
    }
    return true;
}

static const char *xa_path_find_stdlib_marker(const char *file) {
    if (!file)
        return NULL;
    const char *embedded = strstr(file, "<embedded stdlib>/");
    if (embedded)
        return embedded + strlen("<embedded stdlib>/");
    embedded = strstr(file, "<embedded stdlib>\\");
    if (embedded)
        return embedded + strlen("<embedded stdlib>\\");
    const char *disk = strstr(file, "stdlib/");
    if (disk)
        return disk + strlen("stdlib/");
    disk = strstr(file, "stdlib\\");
    return disk ? disk + strlen("stdlib\\") : NULL;
}

/* True for a source that lives in the standard library, on disk or embedded.
 * A builtin's own declaration lives there -- stdlib/path/path.xr declares
 * `class Path` -- so that is a definition, not a redeclaration. */
bool xa_analyzer_path_is_stdlib(const char *file) {
    return xa_path_find_stdlib_marker(file) != NULL;
}

static bool xa_path_is_stdlib_module(const char *file, const char *module_name) {
    if (!file || !module_name)
        return false;
    char suffix[128];
    int n = snprintf(suffix, sizeof(suffix), "stdlib/%s/%s.xr", module_name, module_name);
    if (n < 0 || (size_t) n >= sizeof(suffix))
        return false;
    if (xa_path_has_suffix(file, suffix))
        return true;

    n = snprintf(suffix, sizeof(suffix), "<embedded stdlib>/%s/%s.xr", module_name, module_name);
    if (n < 0 || (size_t) n >= sizeof(suffix))
        return false;
    return xa_path_has_suffix(file, suffix);
}

char *xa_analyzer_nominal_owner_for_file(XaAnalyzer *analyzer, const char *file) {
    if (!analyzer)
        return NULL;
    if (analyzer->current_module_identity && analyzer->current_module_identity[0])
        return xr_strdup(analyzer->current_module_identity);
    if (!file)
        return NULL;

    XrModuleGraph *graph = analyzer->graph;
    for (int i = 0; graph && i < graph->spec_count; i++) {
        const XrModuleSpec *spec = &graph->specs[i];
        if (!spec->canonical || !spec->source_path)
            continue;
        size_t file_len = strlen(file);
        size_t source_len = strlen(spec->source_path);
        if (file_len != source_len)
            continue;
        bool same = true;
        for (size_t j = 0; j < file_len; j++) {
            if (!xa_path_char_matches(file[j], spec->source_path[j])) {
                same = false;
                break;
            }
        }
        if (same)
            return xr_strdup(spec->canonical);
    }

    return NULL;
}

XR_FUNC bool xa_analyzer_publish_nominal_identity(XaAnalyzer *analyzer, const AstNode *declaration,
                                                  XrClassInfo *info) {
    if (!analyzer || !declaration || !info || !info->name || !info->declaration_symbol)
        return false;
    char *owner = xa_analyzer_nominal_owner_for_file(analyzer, analyzer->current_file);
    /* Standalone editor analysis may have no module authority. It can check
     * local types, but a zero key cannot enter specialization or publication. */
    if (!owner)
        return !analyzer->graph && !analyzer->current_module_identity;
    /* Declaration coordinates and names survive analysis and body rewrites.
     * Neither graph order nor analyzer-local symbol allocation contributes. */
    uint64_t fields[] = {
        UINT64_C(0x58524445434c4944),   xr_hash_bytes64(owner, strlen(owner)),
        (uint64_t) declaration->type,   (uint64_t) declaration->line,
        (uint64_t) declaration->column, xr_hash_bytes64(info->name, strlen(info->name))};
    uint8_t bytes[sizeof(fields)];
    for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i)
        for (unsigned byte = 0u; byte < 8u; ++byte)
            bytes[i * 8u + byte] = (uint8_t) (fields[i] >> (byte * 8u));
    xr_free(owner);
    uint64_t key = xr_hash_bytes64(bytes, sizeof(bytes));
    if (key == 0u || (info->declaration_key != 0u && info->declaration_key != key))
        return false;
    info->declaration_key = key;
    return true;
}

static void xa_register_native_class_symbol(XaAnalyzer *analyzer, XaScope *scope, const char *file,
                                            const char *name, bool is_exported) {
    if (!analyzer || !scope || !name)
        return;
    if (xa_scope_lookup_local(scope, name))
        return;

    XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, name, XA_SYM_CLASS);
    if (!sym)
        return;
    sym->location.line = 0;
    sym->is_builtin = true;
    sym->is_const = true;
    sym->is_exported = is_exported;
    if (xa_scope_add_symbol(scope, sym) != XR_COMPILE_RESOURCE_OK) {
        xa_symbol_free(sym);
        return;
    }

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links) {
        links->type = xr_type_new_class(analyzer->isolate, name);
        if (links->type && analyzer->current_module_is_stdlib &&
            analyzer->current_stdlib_module_name) {
            const char *module = analyzer->current_stdlib_module_name;
            const XrStdlibNativeClassDefEntry *entry =
                xr_stdlib_metadata_unique_native_class_span(module, strlen(module), name, strlen(name));
            if (entry)
                (void) xr_stdlib_metadata_resource_identity(entry, &links->type->instance.resource_id);
        }
        links->declared_type = links->type;
        links->is_definitely_assigned = true;
        links->file_path = file;
        if (strcmp(name, "Thread") == 0) {
            const char *type_param_names[] = {"T"};
            if (xa_symbol_links_set_type_params(links, type_param_names, NULL, NULL, 1) != XR_COMPILE_RESOURCE_OK) {
                return;
            }
        }
    }
}

static bool xa_stdlib_native_type_is_internal(const char *name) {
    return name && name[0] == '_' && name[1] == '_';
}

/* A semantic stdlib source file is the implementation body of its native
 * module, so it needs the module's private raw value types in the same scope as
 * its private `__*` functions. Public imports still see only exported source
 * declarations plus explicitly public native types. */
static void xa_register_stdlib_native_module_types(XaAnalyzer *analyzer, const char *file,
                                                   XaScope *scope) {
    if (!analyzer || !scope || !analyzer->current_module_is_stdlib ||
        !analyzer->current_stdlib_module_name)
        return;

    const XaBuiltinModule *module =
        xa_builtin_get_module_info(analyzer->current_stdlib_module_name);
    if (!module)
        return;

    for (int i = 0; i < module->handle_count; i++) {
        const XaBuiltinHandle *handle = &module->handles[i];
        if (handle->name)
            xa_register_native_class_symbol(analyzer, scope, file, handle->name,
                                            !xa_stdlib_native_type_is_internal(handle->name));
    }

    /* Native classes declared by standard library tables. An internal class is still
     * registered -- the module's own body is exactly where it has to be
     * nameable -- it just stays off the exported surface. The .def visibility
     * key answers that, not the name: unlike a handle or an object shape, a
     * class cannot be renamed to `__Something` without that name reaching
     * diagnostics and completion. */
    for (int i = 0; i < module->class_count; i++) {
        const XaBuiltinClass *class_decl = &module->classes[i];
        if (class_decl->name)
            xa_register_native_class_symbol(analyzer, scope, file, class_decl->name,
                                            !class_decl->is_internal);
    }

    for (int i = 0; i < module->object_shape_count; i++) {
        const XaBuiltinObjectShape *object_shape = &module->object_shapes[i];
        if (!object_shape->name || xa_scope_lookup_local(scope, object_shape->name))
            continue;
        XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, object_shape->name, XA_SYM_TYPE_ALIAS);
        if (!sym)
            return;
        sym->location.line = 0;
        sym->is_builtin = true;
        sym->is_const = true;
        sym->is_exported = !xa_stdlib_native_type_is_internal(object_shape->name);
        sym->alias_type = xa_builtin_object_shape_decl_type(analyzer->isolate, analyzer, object_shape);
        if (xa_scope_add_symbol(scope, sym) != XR_COMPILE_RESOURCE_OK) {
            xa_symbol_free(sym);
            return;
        }
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (links) {
            links->type = sym->alias_type;
            links->declared_type = links->type;
            links->is_definitely_assigned = true;
            links->file_path = file;
            /* module->name, not the local buffer: the link outlives this frame
             * and readers such as sync_runtime_import_class_name compare it
             * long after the stack slot is gone (R-OWN-1). */
            links->module_name = module->name;
            links->import_member_name = object_shape->name;
        }
    }

    for (int i = 0; i < module->enum_count; i++) {
        const XaBuiltinEnum *enum_decl = &module->enums[i];
        if (!enum_decl->name || xa_scope_lookup_local(scope, enum_decl->name))
            continue;
        XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, enum_decl->name, XA_SYM_ENUM);
        if (!sym)
            return;
        sym->location.line = 0;
        sym->is_builtin = true;
        sym->is_const = true;
        sym->is_exported = !xa_stdlib_native_type_is_internal(enum_decl->name);
        if (xa_scope_add_symbol(scope, sym) != XR_COMPILE_RESOURCE_OK) {
            xa_symbol_free(sym);
            return;
        }
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (links) {
            links->type =
                xa_builtin_enum_decl_type(analyzer->isolate, analyzer, enum_decl, &links->enum_info);
            links->declared_type = links->type;
            links->is_definitely_assigned = true;
            links->file_path = file;
            links->module_name = module->name;
            links->import_member_name = enum_decl->name;
        }
    }
}

static XrCompileResourceStatus xa_register_native_return_ownership(XrVMRuntime *X, XaSymbolLinks *links,
                                                XaBuiltinReturnOwnership ownership) {
    XrCompileState *state = links ? links->resource_state : NULL;
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    links->return_ownership = (XaReturnOwnershipSummary) {
        .kind = XA_RETURN_OWNERSHIP_UNKNOWN,
        .param_index = -1,
        .complete = false,
    };
    switch (ownership) {
        case XA_BUILTIN_RETURN_FRESH:
            links->return_ownership.kind = XA_RETURN_OWNERSHIP_OWNED;
            links->return_ownership.complete = true;
            break;
        case XA_BUILTIN_RETURN_BORROWED_STATIC:
            links->return_ownership.kind = XA_RETURN_OWNERSHIP_BORROWED_STATIC;
            links->return_ownership.complete = true;
            break;
        default: {
            int param_index = xa_builtin_return_ownership_param_index(ownership);
            if (param_index >= 0) {
                links->return_ownership.kind = XA_RETURN_OWNERSHIP_BORROWED_PARAM;
                links->return_ownership.param_index = (int16_t) param_index;
                links->return_ownership.complete = true;
            }
            break;
        }
    }
    /* A bodyless provider can lend a Slice from an opaque owner even though
     * the owner's nominal type is not itself a contiguous collection.  The
     * generated return_ownership contract is the authority for that
     * relationship; publish the same fact through the ordinary view-origin
     * channel so source wrappers can compose it just like a source function
     * declared with `from parameter`.
     *
     * Mutable Slice returns intentionally stay out of this channel: Xray only
     * permits borrowed Slice results to be const, while scoped mutable access
     * is expressed by a callback API. */
    XrType *type = links->type;
    XrType *return_type = type && XR_TYPE_IS_FUNCTION(type) ? type->function.return_type : NULL;
    if (return_type && XR_TYPE_IS_SLICE(return_type) && return_type->is_const) {
        XrViewOrigin origin = {.kind = XR_VIEW_ORIGIN_STATIC, .param_ordinal = -1};
        bool has_origin = ownership == XA_BUILTIN_RETURN_BORROWED_STATIC;
        int param_index = xa_builtin_return_ownership_param_index(ownership);
        if (param_index >= 0) {
            origin.kind = XR_VIEW_ORIGIN_PARAM;
            origin.param_ordinal = (int16_t) param_index;
            has_origin = true;
        }
        if (has_origin) {
            status = xa_type_set_view_contract(state, X, type, &origin, 1, false);
            if (status != XR_COMPILE_RESOURCE_OK) return status;
            XaViewReturnContract contract = {.origins = type->function.view_origin_set,
                .origin_count = type->function.view_origin_count,
                .was_elided = type->function.view_origin_was_elided, .checked = true, .valid = true};
            status = xa_symbol_links_set_view_contract(links, &contract);
            if (status != XR_COMPILE_RESOURCE_OK) return status;
        }
    }

    /* Bodyless primitive contracts come exclusively from generated metadata;
     * there is no AST body for the generic prepass to inspect. */
    links->return_ownership_scanned = true;
    return XR_COMPILE_RESOURCE_OK;
}

static void xa_register_stdlib_native_module_functions(XaAnalyzer *analyzer, const char *file,
                                                       XaScope *scope) {
    if (!analyzer || !scope || !analyzer->current_module_is_stdlib ||
        !analyzer->current_stdlib_module_name)
        return;

    const XaBuiltinModule *mod = xa_builtin_get_module_info(analyzer->current_stdlib_module_name);
    if (!mod || !mod->functions)
        return;

    for (int i = 0; i < mod->function_count; i++) {
        const XaBuiltinMember *member = &mod->functions[i];
        if (!member->is_internal || !member->name || xa_scope_lookup_local(scope, member->name))
            continue;

        XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, member->name, XA_SYM_FUNCTION);
        if (!sym)
            return;
        sym->location.line = 0;
        sym->is_builtin = true;
        sym->is_const = true;
        if (xa_scope_add_symbol(scope, sym) != XR_COMPILE_RESOURCE_OK) {
            xa_symbol_free(sym);
            return;
        }

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links)
            continue;
        links->type =
            xa_builtin_parse_full_signature_for_module(analyzer, mod->name, member->signature);
        if (!links->type)
            links->type = xr_type_new_unknown(analyzer->isolate);
        links->declared_type = links->type;
        links->is_definitely_assigned = true;
        links->file_path = file;
        links->module_name = mod->name;
        links->import_member_name = member->name;
        if (xa_register_native_return_ownership(analyzer->isolate, links, member->return_ownership) != XR_COMPILE_RESOURCE_OK)
            return;
    }
}

#include "xanalyzer_conditional_admission.inc.c"
void xa_analyzer_analyze(XaAnalyzer *analyzer, const char *file, XrAstNode *ast) {
    if (!analyzer || !ast)
        return;

    /* Inferred syntax belongs to the borrowed program arena. Select it before
     * changing semantic tables, and keep cleanup valid after any first failure. */
    XrCompilerSessionScope ast_scope = {0};
    if (xa_analyzer_ast_scope_open(analyzer, ast, &ast_scope) != XR_COMPILE_RESOURCE_OK)
        return;

    /* Node IDs are monotonic within this exact parser state. Distinct
     * graphless roots still begin a fresh semantic batch; no REPL owner or
     * reset epoch is inferred from the parser session. */
    if (!analyzer->graph &&
        analyzer->type_ref_batch_root_id != ast->node_id) {
        xa_node_table_clear_type_ref_types((XaNodeTable *) analyzer->node_table);
        xa_node_table_clear_generic_specializations((XaNodeTable *) analyzer->node_table);
    }
    analyzer->type_ref_batch_root_id = analyzer->graph ? 0u : ast->node_id;
    if (!xa_conditional_admission(analyzer, file, ast)) {
        if (ast_scope.active)
            (void) xr_compile_session_pop_arena(&ast_scope);
        return;
    }

    xa_enum_record_plan_table_begin_analysis(
        (XaEnumRecordPlanTable *) analyzer->enum_record_plan_table);

    /* Type helpers still select the analyzer-owned pool. Symbol construction
     * receives its explicit registry and cannot route into another analyzer. */
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->type_pool->next_type_id);

    // Track file
    XaFileEntry *entry = NULL;
    if (xa_analyzer_file_get_or_create(analyzer, file, analyzer->global_scope, &entry) != XR_COMPILE_RESOURCE_OK) {
        if (ast_scope.active) (void) xr_compile_session_pop_arena(&ast_scope);
        return;
    }
    if (entry) {
        entry->dirty = false;
    }

    XaScope *file_scope = entry && entry->file_scope ? entry->file_scope : analyzer->global_scope;

    XrModuleSpec *module_spec = xa_graph_spec_for_ast(analyzer, ast);
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) {
        if (ast_scope.active) (void) xr_compile_session_pop_arena(&ast_scope);
        return;
    }
    if (!module_spec && analyzer->standalone_module_identity)
        module_spec = xa_graph_spec_for_identity(analyzer, analyzer->standalone_module_identity);
    if (!module_spec && !analyzer->standalone_module_identity) {
        (void) xr_compile_state_fail(xr_compile_session_compile_state(analyzer->compiler_session),
                                     XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        if (ast_scope.active) (void) xr_compile_session_pop_arena(&ast_scope);
        return;
    }
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) {
        if (ast_scope.active) (void) xr_compile_session_pop_arena(&ast_scope);
        return;
    }
    analyzer->current_module_is_stdlib = module_spec
                                             ? module_spec->kind == XR_MOD_STDLIB
                                             : analyzer->standalone_module_is_stdlib;
    analyzer->current_module_identity =
        module_spec ? module_spec->canonical : analyzer->standalone_module_identity;
    analyzer->current_stdlib_module_name = module_spec && module_spec->kind == XR_MOD_STDLIB
                                               ? module_spec->authority.namespace_id
                                               : analyzer->standalone_stdlib_module_name;

    // Set current file/scope for symbol ownership tracking. The root global
    // scope is reserved for builtins/prelude; source modules live in their
    // own top-level scopes so private names cannot collide across modules.
    analyzer->current_file = entry ? entry->path : NULL;
    analyzer->current_scope = file_scope;
    xa_register_stdlib_native_module_types(analyzer, analyzer->current_file, file_scope);
    xa_register_stdlib_native_module_functions(analyzer, analyzer->current_file, file_scope);

    // Use the visitor-based analysis
    xa_analyze_ast(analyzer, ast);

    if (ast_scope.active)
        (void) xr_compile_session_pop_arena(&ast_scope);

    analyzer->semantic_revision++;
    if (analyzer->semantic_revision == 0)
        analyzer->semantic_revision = 1;

    /* The file cursor names the scope that stays installed, so it outlives the
     * pass exactly as that scope does.  Clearing one half of a two-half fact
     * left the analyzer claiming a file scope with no file, and every consumer
     * that ran after analysis read the missing half. */
    analyzer->current_module_is_stdlib = false;
    analyzer->current_module_identity = NULL;
    analyzer->current_stdlib_module_name = NULL;
    analyzer->current_scope = file_scope;

    // Keep pool set - type_check may call xa_analyzer_infer_expr_type after analyze
    // Pool will be cleared when analyzer is freed
}

void xa_analyzer_update(XaAnalyzer *analyzer, const char *file, XrAstNode *ast) {
    if (!analyzer || !ast)
        return;

    // Select the current analyzer-owned type pool
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->type_pool->next_type_id);

    // Find file entry
    XaFileEntry *entry = NULL;
    if (xa_analyzer_file_get_or_create(analyzer, file, analyzer->global_scope, &entry) != XR_COMPILE_RESOURCE_OK) return;

    // Check if content changed using hash (incremental optimization)
    XaIncrementalCtx *incr = (XaIncrementalCtx *) analyzer->incremental;
    if (incr && entry) {
        uint64_t new_hash = xa_hash_ast_block((AstNode *) ast);
        if (entry->content_hash == new_hash && !entry->dirty) {
            // Content unchanged - skip re-analysis
            return;
        }
        entry->content_hash = new_hash;
    }

    // Clear diagnostics for this file
    xa_analyzer_clear_diagnostics(analyzer);

    // Exact type-reference facts may point through types to the class metadata
    // removed below. Invalidate the cache before replacing any declaration.
    xa_node_table_clear_type_ref_types((XaNodeTable *) analyzer->node_table);
    xa_node_table_clear_generic_specializations((XaNodeTable *) analyzer->node_table);

    // Remove old symbols from this file
    if (file) {
        remove_file_symbols(analyzer->global_scope, file, analyzer);
    }

    // Mark as dirty then re-analyze
    if (entry)
        entry->dirty = true;
    xa_analyzer_analyze(analyzer, file, ast);
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) return;

    // Mark as clean after analysis
    if (entry)
        entry->dirty = false;
}

void xa_analyzer_refresh_file(XaAnalyzer *analyzer, const char *file, XrAstNode *ast,
                              uint64_t content_hash) {
    if (!analyzer || !ast)
        return;

    // Select the current analyzer-owned type pool
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->type_pool->next_type_id);

    // Find file entry
    XaFileEntry *entry = NULL;
    if (xa_analyzer_file_get_or_create(analyzer, file, analyzer->global_scope, &entry) != XR_COMPILE_RESOURCE_OK) return;

    // Check if content changed using provided hash (true incremental check)
    if (entry && entry->content_hash == content_hash && !entry->dirty) {
        // Content unchanged - skip re-analysis entirely
        return;
    }

    // Update hash
    if (entry) {
        entry->content_hash = content_hash;
    }

    XaIncrementalCtx *incr = (XaIncrementalCtx *) analyzer->incremental;

    // Collect symbols that will be removed (for dependency propagation)
    int old_sym_count = 0;
    XaSymbol **old_symbols = xa_scope_get_all_symbols(analyzer->global_scope, &old_sym_count);
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) return;

    // Build change set from symbols in this file
    XaChangeSet changes = {0};
    if (old_symbols && incr && old_sym_count > 0) {
        changes.modified_symbols = xr_malloc(sizeof(uint32_t) * old_sym_count);
        if (changes.modified_symbols) {
            for (int i = 0; i < old_sym_count; i++) {
                if (old_symbols[i]->location.file &&
                    strcmp(old_symbols[i]->location.file, file) == 0) {
                    changes.modified_symbols[changes.modified_count++] = old_symbols[i]->id;
                }
            }

            // Propagate dirty through dependency graph
            xa_propagate_dirty(incr, &changes);

            // Track statistics
            if (incr->dirty_count > changes.modified_count) {
                // Dependencies were affected
                incr->incremental_updates++;
            }

            xr_free(changes.modified_symbols);
        }
        xr_compile_state_free(old_symbols);
    } else if (old_symbols) {
        xr_compile_state_free(old_symbols);
    }

    // Clear diagnostics for this file
    xa_analyzer_clear_diagnostics(analyzer);

    // Exact type-reference facts may point through types to the class metadata
    // removed below. Invalidate the cache before replacing any declaration.
    xa_node_table_clear_type_ref_types((XaNodeTable *) analyzer->node_table);
    xa_node_table_clear_generic_specializations((XaNodeTable *) analyzer->node_table);

    // Remove old symbols from this file
    if (file) {
        remove_file_symbols(analyzer->global_scope, file, analyzer);
    }

    // Re-analyze this file
    xa_analyzer_analyze(analyzer, file, ast);
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) return;

    // Mark other files as dirty based on affected symbols
    if (incr && incr->dirty_count > 0) {
        for (int i = 0; i < incr->dirty_count; i++) {
            XaSymbol *sym = xa_scope_lookup_by_id(analyzer->global_scope, incr->dirty_symbols[i]);
            if (sym && sym->location.file && strcmp(sym->location.file, file) != 0) {
                // Symbol is in another file - mark that file as dirty
                xa_analyzer_mark_file_dirty(analyzer, sym->location.file);
                if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) return;
            }
        }
    }

    // Mark current file as clean
    if (entry)
        entry->dirty = false;
}

// AST -> inferred type side table convenience wrappers.
// These exist so callers (codegen, LSP, mono) do not need to include
// xa_node_table.h directly -- xanalyzer.h is enough. Both functions
// are NULL-safe in every direction.
void xa_analyzer_set_node_type(XaAnalyzer *analyzer, struct AstNode *node, struct XrType *type) {
    if (!analyzer || !node)
        return;
    xa_node_table_set_type((XaNodeTable *) analyzer->node_table, node, type);
}

struct XrType *xa_analyzer_get_node_type(XaAnalyzer *analyzer, const struct AstNode *node) {
    if (!analyzer || !node)
        return NULL;
    return xa_node_table_get_type((XaNodeTable *) analyzer->node_table, node);
}

bool xa_analyzer_bind_type_ref_type(XaAnalyzer *analyzer, const struct XrTypeRef *type_ref,
                                    struct XrType *type) {
    return analyzer && analyzer->node_table && type_ref && type &&
           xa_node_table_set_type_ref_type((XaNodeTable *) analyzer->node_table, type_ref, type);
}

struct XrType *xa_analyzer_get_type_ref_type(XaAnalyzer *analyzer,
                                             const struct XrTypeRef *type_ref) {
    if (!analyzer || !analyzer->node_table || !type_ref)
        return NULL;
    return xa_node_table_get_type_ref_type((const XaNodeTable *) analyzer->node_table, type_ref);
}

bool xa_analyzer_set_generic_specialization(XaAnalyzer *analyzer, const struct AstNode *node,
                                            const XaGenericSpecializationFact *fact) {
    return analyzer && analyzer->node_table && node && fact &&
           xa_node_table_set_generic_specialization((XaNodeTable *) analyzer->node_table, node,
                                                    fact);
}

bool xa_analyzer_get_generic_specialization(XaAnalyzer *analyzer, const struct AstNode *node,
                                            XaGenericSpecializationFact *out_fact) {
    return analyzer && analyzer->node_table && node &&
           xa_node_table_get_generic_specialization((const XaNodeTable *) analyzer->node_table,
                                                    node, out_fact);
}

void xa_analyzer_set_node_conversion(XaAnalyzer *analyzer, const struct AstNode *node,
                                     const XrConversionWitness *witness) {
    if (!analyzer || !analyzer->node_table || !node)
        return;
    xa_node_table_set_conversion((XaNodeTable *) analyzer->node_table, node, witness);
}

bool xa_analyzer_get_node_conversion(XaAnalyzer *analyzer, const struct AstNode *node,
                                     XrConversionWitness *out_witness) {
    if (!analyzer || !analyzer->node_table || !node)
        return false;
    return xa_node_table_get_conversion((XaNodeTable *) analyzer->node_table, node, out_witness);
}

bool xa_analyzer_set_call_error_effect(XaAnalyzer *analyzer, const struct AstNode *node,
                                       const XaCallErrorEffectFact *fact) {
    if (!analyzer || !analyzer->node_table || !node || !fact)
        return false;
    return xa_node_table_set_call_error_effect((XaNodeTable *) analyzer->node_table, node, fact);
}

bool xa_analyzer_get_call_error_effect(XaAnalyzer *analyzer, const struct AstNode *node,
                                       XaCallErrorEffectFact *out_fact) {
    if (!analyzer || !analyzer->node_table || !node)
        return false;
    return xa_node_table_get_call_error_effect((XaNodeTable *) analyzer->node_table, node,
                                               out_fact);
}

void xa_analyzer_clear_call_error_effect(XaAnalyzer *analyzer, const struct AstNode *node) {
    if (analyzer && analyzer->node_table && node)
        xa_node_table_clear_call_error_effect((XaNodeTable *) analyzer->node_table, node);
}

bool xa_analyzer_set_body_effect(XaAnalyzer *analyzer, const struct AstNode *node,
                                 const XaBodyEffectFact *fact) {
    return analyzer && analyzer->node_table && node && fact &&
           xa_node_table_set_body_effect((XaNodeTable *) analyzer->node_table, node, fact);
}

bool xa_analyzer_get_body_effect(XaAnalyzer *analyzer, const struct AstNode *node,
                                 XaBodyEffectFact *out_fact) {
    return analyzer && analyzer->node_table && node &&
           xa_node_table_get_body_effect((XaNodeTable *) analyzer->node_table, node, out_fact);
}

void xa_analyzer_clear_body_effect(XaAnalyzer *analyzer, const struct AstNode *node) {
    if (analyzer && analyzer->node_table && node)
        xa_node_table_clear_body_effect((XaNodeTable *) analyzer->node_table, node);
}

bool xa_analyzer_set_target_query(XaAnalyzer *analyzer, const struct AstNode *node,
                                  const XaTargetQueryFact *fact) {
    return analyzer && analyzer->node_table && node && fact &&
           xa_node_table_set_target_query((XaNodeTable *) analyzer->node_table, node, fact);
}

bool xa_analyzer_get_target_query(XaAnalyzer *analyzer, const struct AstNode *node,
                                  XaTargetQueryFact *out_fact) {
    return analyzer && analyzer->node_table && node &&
           xa_node_table_get_target_query((XaNodeTable *) analyzer->node_table, node, out_fact);
}

void xa_analyzer_clear_target_query(XaAnalyzer *analyzer, const struct AstNode *node) {
    if (analyzer && analyzer->node_table && node)
        xa_node_table_clear_target_query((XaNodeTable *) analyzer->node_table, node);
}

bool xa_analyzer_set_suspend_point(XaAnalyzer *analyzer, const struct AstNode *node,
                                   const XaSuspendPointFact *fact) {
    return analyzer && analyzer->node_table && node && fact &&
           xa_node_table_set_suspend_point((XaNodeTable *) analyzer->node_table, node, fact);
}

bool xa_analyzer_get_suspend_point(XaAnalyzer *analyzer, const struct AstNode *node,
                                   XaSuspendPointFact *out_fact) {
    return analyzer && analyzer->node_table && node &&
           xa_node_table_get_suspend_point((XaNodeTable *) analyzer->node_table, node, out_fact);
}

void xa_analyzer_clear_suspend_point(XaAnalyzer *analyzer, const struct AstNode *node) {
    if (analyzer && analyzer->node_table && node)
        xa_node_table_clear_suspend_point((XaNodeTable *) analyzer->node_table, node);
}

bool xa_analyzer_set_callable_target_set(XaAnalyzer *analyzer, const struct AstNode *node,
                                         const XaCallableTargetSetFact *fact) {
    return analyzer && analyzer->node_table &&
           xa_node_table_set_callable_target_set((XaNodeTable *) analyzer->node_table, node, fact);
}

bool xa_analyzer_get_callable_target_set(XaAnalyzer *analyzer, const struct AstNode *node,
                                         XaCallableTargetSetFact *out_fact) {
    return analyzer && analyzer->node_table &&
           xa_node_table_get_callable_target_set((XaNodeTable *) analyzer->node_table, node,
                                                 out_fact);
}

void xa_analyzer_clear_callable_target_set(XaAnalyzer *analyzer, const struct AstNode *node) {
    if (analyzer && analyzer->node_table && node)
        xa_node_table_clear_callable_target_set((XaNodeTable *) analyzer->node_table, node);
}

void xa_analyzer_set_node_ct_value(XaAnalyzer *analyzer, const struct AstNode *node,
                                   const XrCtValue *value) {
    if (!analyzer || !node)
        return;
    xa_node_table_set_ct_value((XaNodeTable *) analyzer->node_table, node, value);
}

bool xa_analyzer_get_node_ct_value(XaAnalyzer *analyzer, const struct AstNode *node,
                                   XrCtValue *out_value) {
    if (!analyzer || !node)
        return false;
    return xa_node_table_get_ct_value((XaNodeTable *) analyzer->node_table, node, out_value);
}

const struct XaSelection *xa_analyzer_get_selection(XaAnalyzer *analyzer,
                                                    const struct AstNode *node) {
    if (!analyzer || !node)
        return NULL;
    return xa_selection_table_get((XaSelectionTable *) analyzer->selection_table, node);
}

const XaParallelCallPlan *xa_analyzer_get_parallel_call_plan(XaAnalyzer *analyzer,
                                                             const struct AstNode *node) {
    if (!analyzer || !node)
        return NULL;
    return xa_parallel_call_plan_table_get(
        (XaParallelCallPlanTable *) analyzer->parallel_call_plan_table, node);
}

const XaEnumRecordPlan *xa_analyzer_get_enum_record_plan(XaAnalyzer *analyzer,
                                                         const struct AstNode *node) {
    if (!analyzer || !node)
        return NULL;
    return xa_enum_record_plan_table_get((XaEnumRecordPlanTable *) analyzer->enum_record_plan_table,
                                         node);
}

const XaResolvedCall *xa_analyzer_get_resolved_call(XaAnalyzer *analyzer,
                                                    const struct AstNode *node) {
    if (!analyzer || !node)
        return NULL;
    return xa_resolved_call_table_get((XaResolvedCallTable *) analyzer->resolved_call_table, node);
}

static const char *adt_subject_enum_name(struct XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_ENUM)
        return type->enum_type.enum_name;
    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
        type->instance.class_name)
        return type->instance.class_name;
    return NULL;
}

static const char *adt_variant_owner_name(const struct AstNode *variant) {
    if (!variant)
        return NULL;
    if (variant->type == AST_ENUM_ACCESS)
        return variant->as.enum_access.enum_name;
    if (variant->type == AST_MEMBER_ACCESS) {
        const AstNode *object = variant->as.member_access.object;
        if (object && object->type == AST_VARIABLE)
            return object->as.variable.name;
    }
    return NULL;
}

static const char *adt_variant_member_name(const struct AstNode *variant) {
    if (!variant)
        return NULL;
    if (variant->type == AST_ENUM_ACCESS)
        return variant->as.enum_access.member_name;
    if (variant->type == AST_MEMBER_ACCESS)
        return variant->as.member_access.name;
    return NULL;
}

static XaSymbol *adt_lookup_enum_symbol(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(analyzer, name);
    if (!sym || sym->kind != XA_SYM_ENUM)
        sym = xa_analyzer_lookup_in_scope(analyzer, name, analyzer->global_scope);
    if (!sym || sym->kind != XA_SYM_ENUM)
        sym = xa_analyzer_lookup_deep(analyzer, name);
    return (sym && sym->kind == XA_SYM_ENUM) ? sym : NULL;
}

struct XrType *xa_analyzer_resolve_adt_payload_type(XaAnalyzer *analyzer,
                                                    struct XrType *subject_type,
                                                    const struct AstNode *variant,
                                                    int payload_index) {
    if (!analyzer || !variant || payload_index < 0)
        return NULL;

    const char *subject_name = adt_subject_enum_name(subject_type);
    const char *owner_name = adt_variant_owner_name(variant);
    const char *member_name = adt_variant_member_name(variant);
    const char *enum_name = subject_name ? subject_name : owner_name;
    if (!enum_name || !member_name)
        return NULL;
    if (subject_name && owner_name && strcmp(subject_name, owner_name) != 0)
        return NULL;

    XaSymbol *enum_sym = adt_lookup_enum_symbol(analyzer, enum_name);
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, enum_sym);
    XaEnumInfo *info = links ? links->enum_info : NULL;
    if (!info || !info->variants)
        return NULL;

    int member_index = xa_enum_info_find_variant(info, member_name);
    if (member_index < 0 || payload_index >= (int) info->variants[member_index].payload_count)
        return NULL;

    XrType **payload_types = info->variants[member_index].payload_types;
    XrType *payload_type = payload_types ? payload_types[payload_index] : NULL;
    if (!payload_type)
        return NULL;

    int param_count = xa_symbol_links_get_type_param_count(links);
    XrType **type_args = NULL;
    int type_arg_count = 0;
    if (subject_type && subject_type->kind == XR_KIND_ENUM) {
        type_args = subject_type->enum_type.type_args;
        type_arg_count = subject_type->enum_type.type_arg_count;
    } else if (subject_type &&
               (subject_type->kind == XR_KIND_INSTANCE || subject_type->kind == XR_KIND_CLASS)) {
        type_args = subject_type->instance.type_args;
        type_arg_count = subject_type->instance.type_arg_count;
    }
    if (param_count <= 0 || type_arg_count != param_count || !type_args)
        return payload_type;

    const char *stack_names[8];
    const char **param_names = stack_names;
    if (param_count > 8) {
        param_names = (const char **) xr_malloc(sizeof(const char *) * (size_t) param_count);
        if (!param_names)
            return payload_type;
    }
    for (int i = 0; i < param_count; i++)
        param_names[i] = xa_symbol_links_get_type_param_name(links, i);

    XrType *resolved =
        xr_type_substitute(analyzer->isolate, payload_type, param_names, type_args, param_count);
    if (param_names != stack_names)
        xr_free((void *) param_names);
    return resolved ? resolved : payload_type;
}

void xa_analyzer_remove_file(XaAnalyzer *analyzer, const char *file) {
    if (!analyzer || !file)
        return;
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) return;

    // This is the unified file-removal entry. It must keep the
    // analyzer's three "size-of-everything" counters self-consistent:
    //
    //   file index size == file_count       (one entry per tracked file)
    //   dep_graph edges only reference live symbol IDs
    //   symbol_table holds no symbols owned by `file` after return
    //
    // Order matters: collect dependency-graph edges to drop FIRST (we
    // need the symbols' ids while they are still live), then remove the
    // symbols, then remove the file entry, then clear diagnostics.

    XaIncrementalCtx *incr = (XaIncrementalCtx *) analyzer->incremental;
    int edges_before = (incr && incr->deps) ? incr->deps->edge_count : 0;
    int file_count_before = analyzer->file_count;

    /* Collect all owned IDs before mutating the dependency graph. File scopes
     * are children of the builtin root and may retain hidden old declarations. */
    XaFileOwnedIds collect = {.analyzer = analyzer, .file = file};
    if (!collect_file_owned_ids(analyzer->global_scope, &collect)) return;
    if (collect.count > 0) {
        XrCompileState *state = xa_symbol_registry_state(analyzer->symbol_registry);
        int capacity = collect.count;
        if ((size_t)capacity > SIZE_MAX / sizeof(uint32_t)) {
            (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
            return;
        }
        void *memory = NULL;
        if (xr_compile_state_alloc(state, (size_t)capacity * sizeof(uint32_t), &memory) !=
            XR_COMPILE_RESOURCE_OK) return;
        collect.ids = memory;
        collect.capacity = capacity;
        collect.count = 0;
        if (!collect_file_owned_ids(analyzer->global_scope, &collect)) {
            xr_compile_state_free(memory);
            return;
        }
        if (incr) xa_dep_remove_symbols(incr, collect.ids, collect.count);
        xr_compile_state_free(collect.ids);
    }

    // Step 3: invalidate exact type references before their declaration
    // metadata is freed, then remove symbols owned by this file.
    xa_node_table_clear_type_ref_types((XaNodeTable *) analyzer->node_table);
    xa_node_table_clear_generic_specializations((XaNodeTable *) analyzer->node_table);
    remove_file_symbols(analyzer->global_scope, file, analyzer);
    if (xa_scope_resource_status(analyzer->global_scope) != XR_COMPILE_RESOURCE_OK) return;

    // Remove the owned record after semantic retirement succeeds.
    bool was_tracked = false;
    if (xa_analyzer_file_remove_record(analyzer, file, &was_tracked) != XR_COMPILE_RESOURCE_OK) return;

    // Step 5: clear diagnostics for any subsequent re-analysis.
    xa_analyzer_clear_diagnostics(analyzer);

    // Step 6: invariants. file_count drops by exactly one when the file
    // was tracked, otherwise stays put. Dep-graph edge count never grows
    // during a removal.
    XR_DCHECK(analyzer->file_count == (was_tracked ? file_count_before - 1 : file_count_before),
              "xa_analyzer_remove_file: file_count drift");
    XR_DCHECK(xa_analyzer_file_index_size(analyzer) == (uint32_t)analyzer->file_count,
              "xa_analyzer_remove_file: index/list count drift");
    XR_DCHECK(!incr || !incr->deps || incr->deps->edge_count <= edges_before,
              "xa_analyzer_remove_file: dep edge_count grew during removal");
    (void) was_tracked;
    (void) file_count_before;
    (void) edges_before;
}

// Helper: find symbol at position with file filter
static XaSymbol *find_symbol_at_position_in_file(XaScope *scope, const char *file, uint32_t line,
                                                 uint32_t column) {
    if (!scope)
        return NULL;

    int count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(scope, &count);
    if (xa_scope_resource_status(scope) != XR_COMPILE_RESOURCE_OK) return NULL;

    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym)
            continue;

        // Filter by file if specified
        if (file && sym->location.file && strcmp(sym->location.file, file) != 0) {
            continue;
        }

        // Check if position is within symbol
        if (sym->location.line == line) {
            uint32_t sym_end = sym->location.column + (sym->name ? strlen(sym->name) : 0);
            if (column >= sym->location.column && column <= sym_end) {
                xr_compile_state_free(symbols);
                return sym;
            }
        }
    }
    xr_compile_state_free(symbols);

    // Search child scopes
    for (int i = 0; i < scope->child_count; i++) {
        XaSymbol *found = find_symbol_at_position_in_file(scope->children[i], file, line, column);
        if (xa_scope_resource_status(scope) != XR_COMPILE_RESOURCE_OK) return NULL;
        if (found)
            return found;
    }

    return NULL;
}

XaSymbol *xa_analyzer_lookup_at(XaAnalyzer *analyzer, const char *file, uint32_t line,
                                uint32_t column) {
    if (!analyzer || !analyzer->global_scope)
        return NULL;

    return find_symbol_at_position_in_file(analyzer->global_scope, file, line, column);
}

XrType *xa_analyzer_get_type_at(XaAnalyzer *analyzer, const char *file, uint32_t line,
                                uint32_t column) {
    XaSymbol *sym = xa_analyzer_lookup_at(analyzer, file, line, column);
    if (!sym)
        return NULL;

    return xa_analyzer_get_type(analyzer, sym);
}

XrType *xa_analyzer_infer_expr_type(XaAnalyzer *analyzer, XrAstNode *expr) {
    if (!analyzer || !expr)
        return NULL;

    // Ensure type pool is set for this thread
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->type_pool->next_type_id);

    // Create temporary inference context
    XaInferContext *ctx = xa_infer_context_new(analyzer);
    if (!ctx)
        return xr_type_new_unknown(NULL);

    // Infer expression type
    XrType *type = xa_visit_infer_expr(ctx, expr);

    xa_infer_context_free(ctx);
    return type ? type : xr_type_new_unknown(NULL);
}

// Variable operations (compatible with ct_infer API)
XrType *xa_analyzer_lookup_var(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return NULL;

    XaSymbol *sym = xa_scope_lookup(analyzer->current_scope, name);
    if (!sym)
        return NULL;

    return xa_analyzer_get_type(analyzer, sym);
}

void xa_analyzer_define_var(XaAnalyzer *analyzer, const char *name, XrType *type) {
    if (!analyzer || !name)
        return;

    XaSymbol *sym = xa_symbol_new(analyzer->symbol_registry, name, XA_SYM_VARIABLE);
    if (!sym) return;
    if (xa_scope_add_symbol(analyzer->current_scope, sym) != XR_COMPILE_RESOURCE_OK) {
        xa_symbol_free(sym);
        return;
    }

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links) {
        links->type = type;
        links->declared_type = type;
    }
}

// ============================================================================
// LSP Support: Find References
// ============================================================================

// NOTE: Reference collection is done via links->references (stored during analysis)
// The old collect_refs_in_scope was dead code with incorrect logic (name-based instead of ID-based)

XaSymbolRef *xa_analyzer_find_references(XaAnalyzer *analyzer, const char *name,
                                         bool include_definition, int *count) {
    *count = 0;
    if (!analyzer || !name || !analyzer->global_scope)
        return NULL;

    XaSymbolRef *refs = NULL;

    // First find the symbol definition
    XaSymbol *sym = xa_scope_lookup(analyzer->global_scope, name);
    if (!sym)
        return NULL;

    // Add definition location if requested
    if (include_definition) {
        XaSymbolRef *def_ref = xr_calloc(1, sizeof(XaSymbolRef));
        if (def_ref) {
            def_ref->file = sym->location.file;
            def_ref->line = sym->location.line;
            def_ref->column = sym->location.column;
            def_ref->end_column = sym->location.column + (sym->name ? strlen(sym->name) : 0);
            def_ref->is_definition = true;
            def_ref->is_write = false;
            def_ref->next = refs;
            refs = def_ref;
            (*count)++;
        }
    }

    // Get collected references from symbol links
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links && links->references) {
        XaRefLocation *loc = links->references;
        while (loc) {
            XaSymbolRef *ref = xr_calloc(1, sizeof(XaSymbolRef));
            if (ref) {
                ref->file = sym->location.file;  // Same file as definition
                ref->line = loc->line;
                ref->column = loc->column;
                ref->end_column = loc->end_column;
                ref->is_definition = false;
                ref->is_write = loc->is_write;
                ref->next = refs;
                refs = ref;
                (*count)++;
            }
            loc = loc->next;
        }
    }

    return refs;
}

XaSymbolRef *xa_analyzer_find_references_at(XaAnalyzer *analyzer, const char *file, uint32_t line,
                                            uint32_t column, int *count) {
    *count = 0;
    if (!analyzer)
        return NULL;

    // Find symbol at position
    XaSymbol *sym = xa_analyzer_lookup_at(analyzer, file, line, column);
    if (!sym || !sym->name)
        return NULL;

    // Find all references to this symbol
    return xa_analyzer_find_references(analyzer, sym->name, true, count);
}

void xa_analyzer_free_references(XaSymbolRef *refs) {
    while (refs) {
        XaSymbolRef *next = refs->next;
        xr_free(refs);
        refs = next;
    }
}

bool xa_analyzer_can_rename(XaAnalyzer *analyzer, const char *file, uint32_t line, uint32_t column,
                            char **out_symbol_name) {
    if (!analyzer || !out_symbol_name)
        return false;
    *out_symbol_name = NULL;

    // Find symbol at position
    XaSymbol *sym = xa_analyzer_lookup_at(analyzer, file, line, column);
    if (!sym || !sym->name)
        return false;

    // Check if symbol can be renamed (not builtin)
    if (sym->is_builtin)
        return false;

    // Return symbol name
    *out_symbol_name = xr_strdup(sym->name);
    return true;
}

// ============================================================================
// Iterable/Iterator Structural Type Checking (with analyzer context)
// ============================================================================

// Helper: get method return type from class
static XrType *get_method_return_type(XaAnalyzer *analyzer, XrClassInfo *info,
                                      const char *method_name) {
    if (!analyzer || !info || !method_name)
        return NULL;

    // Search in class and base classes
    for (XrClassInfo *c = info; c != NULL; c = c->base) {
        for (int i = 0; i < c->method_count; i++) {
            XaSymbol *method = c->methods[i];
            if (method && method->name && strcmp(method->name, method_name) == 0) {
                // Get type from symbol links
                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, method);
                if (links && links->return_type) {
                    return links->return_type;
                }
                // Fallback to computed type
                if (links && links->type && (links->type->kind == XR_KIND_FUNCTION)) {
                    return links->type->function.return_type;
                }
            }
        }
    }
    return NULL;
}

// Helper: resolve XrClassInfo from a class or instance type via class_name lookup
static XrClassInfo *resolve_class_info(XaAnalyzer *analyzer, XrType *type) {
    if (!analyzer || !type)
        return NULL;
    if (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS)
        return NULL;
    if (type->instance.class_ref)
        return type->instance.class_ref;
    if (!type->instance.class_name)
        return NULL;
    // Search from current scope up (class may be in module or local scope)
    XaSymbol *sym = xa_analyzer_lookup(analyzer, type->instance.class_name);
    if (!sym || sym->kind != XA_SYM_CLASS)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    return (links && links->class_info) ? links->class_info : NULL;
}

// Check if type satisfies Iterator<T> (has hasNext(): bool and next(): T)
bool xa_analyzer_is_iterator(XaAnalyzer *analyzer, XrType *type, XrType **out_element_type) {
    if (!analyzer || !type)
        return false;

    // The builtin Iterator<T> type satisfies the protocol by construction;
    // its type argument is the element type. This is exactly what a
    // spec-conforming `iterator() -> Iterator<T>` declaration returns, so it
    // is accepted the same way a concrete iterator class is. The for-in
    // lowering drives it through the same hasNext/next protocol dispatch.
    if ((type->kind == XR_KIND_INTERFACE || type->kind == XR_KIND_INSTANCE ||
         type->kind == XR_KIND_CLASS) &&
        xr_type_is_builtin_named_type(type, "Iterator")) {
        if (out_element_type) {
            *out_element_type = (type->instance.type_arg_count >= 1 && type->instance.type_args &&
                                 type->instance.type_args[0])
                                    ? type->instance.type_args[0]
                                    : xr_type_new_unknown(NULL);
        }
        return true;
    }

    // Must be a class or instance type
    if (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS)
        return false;

    XrClassInfo *info = resolve_class_info(analyzer, type);
    if (!info)
        return false;

    // Check hasNext() method returns bool
    XrType *has_next_ret = get_method_return_type(analyzer, info, "hasNext");
    if (!has_next_ret || !(has_next_ret->kind == XR_KIND_BOOL)) {
        return false;
    }

    // Check next() method exists
    XrType *next_ret = get_method_return_type(analyzer, info, "next");
    if (!next_ret) {
        return false;
    }

    // Element type is the return type of next()
    if (out_element_type) {
        *out_element_type = next_ret;
    }
    return true;
}

// Check if type satisfies Iterable<T> (built-in or has iterator() -> Iterator<T>)
bool xa_analyzer_is_iterable(XaAnalyzer *analyzer, XrType *type, XrType **out_element_type) {
    if (!type)
        return false;

    // First check built-in iterable types (doesn't need analyzer)
    if (xr_type_is_iterable(type, out_element_type)) {
        return true;
    }

    // The two built-in iteration-protocol interfaces yield their single type
    // argument as the element type.
    //   Iterable<T> — the contract a for-in collection satisfies; also what a
    //                 `<T: Iterable<int>>` bound resolves to.
    //   Iterator<T> — its own iterable (e.g. the result of a generator call):
    //                 for-in drives it through iterator() (which returns self)
    //                 plus hasNext()/next().
    if ((type->kind == XR_KIND_INTERFACE || type->kind == XR_KIND_INSTANCE) &&
        (xr_type_is_builtin_named_type(type, "Iterable") ||
         xr_type_is_builtin_named_type(type, "Iterator"))) {
        if (out_element_type) {
            *out_element_type = (type->instance.type_arg_count >= 1 && type->instance.type_args &&
                                 type->instance.type_args[0])
                                    ? type->instance.type_args[0]
                                    : xr_type_new_unknown(NULL);
        }
        return true;
    }

    // Custom class: check if it has iterator() method returning Iterator<T>
    if (analyzer && (type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS)) {
        XrClassInfo *info = resolve_class_info(analyzer, type);
        if (!info)
            return false;

        XrType *iter_ret = get_method_return_type(analyzer, info, "iterator");
        if (iter_ret) {
            // Check if the return type satisfies Iterator<T>
            XrType *elem_type = NULL;
            if (xa_analyzer_is_iterator(analyzer, iter_ret, &elem_type)) {
                if (out_element_type) {
                    *out_element_type = elem_type;
                }
                return true;
            }
        }
    }

    return false;
}
