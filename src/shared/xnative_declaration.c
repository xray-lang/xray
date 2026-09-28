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

const XrNativeTypeDeclaration *xr_native_declaration_by_id(uint32_t id) {
    return id == xr_native_array.id ? &xr_native_array :
        id == xr_native_string.id ? &xr_native_string : NULL;
}
const XrNativeTypeDeclaration *xr_native_declaration_by_name(const char *name) {
    if (!name) return NULL;
    return !strcmp(name, xr_native_array.name) ? &xr_native_array :
        !strcmp(name, xr_native_string.name) ? &xr_native_string : NULL;
}
const XrNativeMemberDeclaration *xr_native_declaration_member(
    const XrNativeTypeDeclaration *declaration, const char *name) {
    if (!declaration || !name || !declaration->members || declaration->member_count > 64) return NULL;
    for (uint32_t i = 0; i < declaration->member_count; ++i)
        if (declaration->members[i].name && !strcmp(declaration->members[i].name, name))
            return &declaration->members[i];
    return NULL;
}
bool xr_native_declaration_validate(const XrNativeTypeDeclaration *declaration) {
    if (!declaration ||
        !declaration->name || !declaration->parameter_name || !declaration->source_path ||
        !declaration->identity || !declaration->members || !declaration->member_count ||
        declaration->member_count > 64) return false;
    const XrNativeTypeDeclaration *canonical = xr_native_declaration_by_id(declaration->id);
    if (!canonical || declaration->kind != canonical->kind ||
        declaration->parameter_count != canonical->parameter_count ||
        declaration->line != canonical->line || declaration->column != canonical->column ||
        strcmp(declaration->identity, canonical->identity) ||
        strcmp(declaration->source_path, canonical->source_path) ||
        strcmp(declaration->name, canonical->name) ||
        strcmp(declaration->parameter_name, canonical->parameter_name) ||
        memcmp(&declaration->source_fingerprint, &canonical->source_fingerprint,
            sizeof(declaration->source_fingerprint)) || declaration->member_count != canonical->member_count)
        return false;
    for (uint32_t i = 0; i < declaration->member_count; ++i) {
        const XrNativeMemberDeclaration *member = &declaration->members[i], *expected = &canonical->members[i];
        if (!member->name || !member->signature || !member->result_text || !member->failures ||
            member->id != expected->id || member->line != expected->line || member->column != expected->column ||
            strcmp(member->name, expected->name) ||
            strcmp(member->signature, expected->signature) || strcmp(member->result_text, expected->result_text) ||
            member->receiver != expected->receiver || member->is_static != expected->is_static ||
            member->is_method != expected->is_method || member->lowered != expected->lowered ||
            member->is_public != expected->is_public ||
            member->operation != expected->operation || member->allocation != expected->allocation ||
            strcmp(member->failures, expected->failures) || member->ownership != expected->ownership ||
            member->result != expected->result || member->parameter_count != expected->parameter_count ||
            (member->parameter_count && !member->parameters)) return false;
        for (uint32_t p = 0; p < member->parameter_count; ++p) {
            const XrNativeParameter *param = &member->parameters[p], *want = &expected->parameters[p];
            if (!param->name || !param->type_text || strcmp(param->name, want->name) ||
                strcmp(param->type_text, want->type_text) || param->type != want->type ||
                param->optional != want->optional || param->variadic != want->variadic) return false;
        }
    }
    return true;
}
