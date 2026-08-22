/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_builtin_map_entry_iterator_shape.h - Typed Map entry-iterator authority
 *
 * The stable method symbol, operation shape and exact builtin types are the
 * authority. Source selector text is deliberately not consulted here.
 */

#ifndef XI_BUILTIN_MAP_ENTRY_ITERATOR_SHAPE_H
#define XI_BUILTIN_MAP_ENTRY_ITERATOR_SHAPE_H

#include "xi.h"
#include "../runtime/value/xtype.h"

static inline XiMethodSymbolId xi_call_method_symbol_id(const XiValue *value) {
    if (!value || (value->op != XI_CALL_METHOD && value->op != XI_CALL_METHOD_DIRECT) ||
        value->aux_int <= 0 || (value->aux_int & 1) != 0)
        return XI_METHOD_SYMBOL_INVALID;
    uint64_t symbol = (uint64_t) value->aux_int >> 1;
    return symbol <= UINT16_MAX ? (XiMethodSymbolId) symbol : XI_METHOD_SYMBOL_INVALID;
}

static inline const XrType *xi_builtin_iterator_element_type(const XrType *type) {
    return xr_type_is_builtin_named_type(type, "Iterator") &&
                   type->instance.type_arg_count == 1 && type->instance.type_args
               ? type->instance.type_args[0]
               : NULL;
}

static inline bool xi_map_entry_tuple_matches(const XrType *map, const XrType *entry) {
    return map && map->kind == XR_KIND_MAP && map->map.key_type && map->map.value_type && entry &&
           entry->kind == XR_KIND_TUPLE && entry->tuple.element_count == 2 &&
           entry->tuple.element_types && entry->tuple.element_types[0] &&
           entry->tuple.element_types[1] &&
           xr_type_equals(map->map.key_type, entry->tuple.element_types[0]) &&
           xr_type_equals(map->map.value_type, entry->tuple.element_types[1]);
}

static inline bool xi_map_entries_iterator_is_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 1 && value->args ? value->args[0] : NULL;
    const XrType *entry = xi_builtin_iterator_element_type(value ? value->type : NULL);
    return value && receiver && value->aux_kind == XI_AUX_KIND_NONE &&
           xi_call_method_symbol_id(value) == XI_METHOD_SYMBOL_ENTRIES_ITERATOR &&
           xi_map_entry_tuple_matches(receiver->type, entry);
}

static inline bool xi_map_entry_iterator_has_next_is_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 1 && value->args ? value->args[0] : NULL;
    return value && receiver && value->aux_kind == XI_AUX_KIND_NONE &&
           xi_call_method_symbol_id(value) == XI_METHOD_SYMBOL_HAS_NEXT && value->type &&
           value->type->kind == XR_KIND_BOOL && !value->type->is_nullable &&
           xi_map_entries_iterator_is_exact(receiver);
}

static inline bool xi_map_entry_iterator_next_is_exact(const XiValue *value) {
    const XiValue *receiver = value && value->nargs == 1 && value->args ? value->args[0] : NULL;
    const XrType *entry = xi_builtin_iterator_element_type(receiver ? receiver->type : NULL);
    return value && receiver && entry && value->aux_kind == XI_AUX_KIND_NONE &&
           xi_call_method_symbol_id(value) == XI_METHOD_SYMBOL_NEXT && value->type &&
           xr_type_equals(value->type, (XrType *) entry) &&
           xi_map_entries_iterator_is_exact(receiver);
}

#endif  // XI_BUILTIN_MAP_ENTRY_ITERATOR_SHAPE_H
