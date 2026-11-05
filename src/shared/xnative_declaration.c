/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_declaration.c - Canonical native value declaration lookup
 *
 * KEY CONCEPT:
 *   Every compiler consumer projects the same generated declaration rows.
 */
#include "xnative_declaration.h"
#include <string.h>
#include "xnative_declarations.inc.c"

XR_FUNC const XrNativeCallableDeclaration *xr_native_callable_by_id(uint32_t id) {
    for (size_t i = 0; i < sizeof(xr_native_callables) / sizeof(*xr_native_callables); ++i)
        if (id && xr_native_callables[i].id == id) return &xr_native_callables[i];
    return NULL;
}
XR_FUNC const XrNativeIntrinsicDeclaration *xr_native_intrinsic_by_id(uint32_t id) {
    if (!id) return NULL;
    for (size_t i = 0; i < sizeof(xr_native_intrinsics) / sizeof(*xr_native_intrinsics); ++i)
        if (xr_native_intrinsics[i].id == id) return &xr_native_intrinsics[i];
    return NULL;
}
XR_FUNC const XrNativeIntrinsicDeclaration *xr_native_intrinsic_for_member(
    const XrNativeTypeDeclaration *owner, const XrNativeMemberDeclaration *member) {
    if (!owner || !member) return NULL;
    for (size_t i = 0; i < sizeof(xr_native_intrinsics) / sizeof(*xr_native_intrinsics); ++i)
        if (xr_native_intrinsics[i].owner == owner && xr_native_intrinsics[i].member == member)
            return &xr_native_intrinsics[i];
    return NULL;
}

const XrNativeTypeDeclaration *xr_native_declaration_by_id(uint32_t id) {
    return id == xr_native_array.id ? &xr_native_array :
        id == xr_native_string.id ? &xr_native_string :
        id == xr_native_atomic.id ? &xr_native_atomic :
        id == xr_native_ordering.id ? &xr_native_ordering :
        id == xr_native_task.id ? &xr_native_task :
        id == xr_native_channel.id ? &xr_native_channel :
        id == xr_native_recv.id ? &xr_native_recv :
        id == xr_native_send_result.id ? &xr_native_send_result :
        id == xr_native_iterator.id ? &xr_native_iterator :
        id == xr_native_i64.id ? &xr_native_i64 :
        id == xr_native_f64.id ? &xr_native_f64 :
        id == xr_native_bool.id ? &xr_native_bool :
        id == xr_native_rune.id ? &xr_native_rune :
        id == xr_native_map.id ? &xr_native_map :
        id == xr_native_set.id ? &xr_native_set :
        id == xr_native_utf8_error.id ? &xr_native_utf8_error :
        id == xr_native_cancelled.id ? &xr_native_cancelled :
        id == xr_native_task_result.id ? &xr_native_task_result : NULL;
}
static bool native_text_equal(const char *left, const char *right) {
    return left && right && strcmp(left, right) == 0;
}
XR_FUNC const XrNativeTypeDeclaration *xr_native_declaration_by_name(const char *name) {
    if (!name) return NULL;
    const XrNativeTypeDeclaration *const declarations[] = {&xr_native_array,
        &xr_native_string, &xr_native_atomic, &xr_native_ordering, &xr_native_task,
        &xr_native_channel, &xr_native_recv, &xr_native_send_result, &xr_native_iterator,
        &xr_native_map, &xr_native_set, &xr_native_i64, &xr_native_f64,
        &xr_native_bool, &xr_native_rune, &xr_native_utf8_error, &xr_native_cancelled,
        &xr_native_task_result};
    for (size_t i = 0; i < sizeof(declarations) / sizeof(*declarations); ++i) {
        if (native_text_equal(name, declarations[i]->name)) return declarations[i];
    }
    return NULL;
}
XR_FUNC const XrNativeMemberDeclaration *xr_native_declaration_member(
    const XrNativeTypeDeclaration *declaration, const char *name) {
    if (!declaration || !name || !declaration->members || declaration->member_count > 64)
        return NULL;
    for (uint32_t i = 0; i < declaration->member_count; ++i) {
        if (native_text_equal(declaration->members[i].name, name))
            return &declaration->members[i];
    }
    return NULL;
}
XR_FUNC bool xr_native_declaration_validate(const XrNativeTypeDeclaration *declaration) {
    if (!declaration ||
        !declaration->name || !declaration->source_path ||
        (!!declaration->parameter_count != !!declaration->parameter_names) ||
        !declaration->identity || !declaration->members || !declaration->member_count ||
        declaration->member_count > 64) return false;
    const XrNativeTypeDeclaration *canonical = xr_native_declaration_by_id(declaration->id);
    if (!canonical || declaration->kind != canonical->kind ||
        declaration->parameter_count != canonical->parameter_count ||
        declaration->line != canonical->line || declaration->column != canonical->column ||
        !native_text_equal(declaration->identity, canonical->identity) ||
        !native_text_equal(declaration->source_path, canonical->source_path) ||
        !native_text_equal(declaration->name, canonical->name)) return false;
    for (uint32_t p = 0; p < declaration->parameter_count; ++p) {
        if (!native_text_equal(
            declaration->parameter_names[p], canonical->parameter_names[p])) return false;
    }
    if (memcmp(&declaration->source_fingerprint, &canonical->source_fingerprint, sizeof(declaration->source_fingerprint)) != 0 ||
        declaration->member_count != canonical->member_count)
        return false;
    for (uint32_t i = 0; i < declaration->member_count; ++i) {
        const XrNativeMemberDeclaration *member = &declaration->members[i], *expected = &canonical->members[i];
        if (!member->name || !member->signature || !member->result_text || !member->failures ||
            member->id != expected->id || member->line != expected->line || member->column != expected->column ||
            !native_text_equal(member->name, expected->name) ||
            !native_text_equal(member->signature, expected->signature) ||
            !native_text_equal(member->result_text, expected->result_text) ||
            member->receiver != expected->receiver || member->is_static != expected->is_static ||
            member->is_method != expected->is_method || member->lowered != expected->lowered ||
            member->is_public != expected->is_public ||
            member->operation != expected->operation || member->allocation != expected->allocation ||
            !native_text_equal(member->failures, expected->failures) || member->ownership != expected->ownership ||
            member->result != expected->result || member->parameter_count != expected->parameter_count ||
            (member->parameter_count && !member->parameters)) return false;
        for (uint32_t p = 0; p < member->parameter_count; ++p) {
            const XrNativeParameter *param = &member->parameters[p], *want = &expected->parameters[p];
            if (!param->name || !param->type_text || !native_text_equal(param->name, want->name) ||
                !native_text_equal(param->type_text, want->type_text) || param->type != want->type ||
                param->optional != want->optional || param->variadic != want->variadic) return false;
        }
    }
    return true;
}
