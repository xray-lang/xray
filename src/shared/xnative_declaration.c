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
        id == xr_native_string.id ? &xr_native_string :
        id == xr_native_atomic.id ? &xr_native_atomic :
        id == xr_native_ordering.id ? &xr_native_ordering : NULL;
}
typedef struct NativeDeclarationAdmission {
    const XrNativeDeclarationWork *work;
    XrNativeDeclarationStatus status;
} NativeDeclarationAdmission;
static bool native_work(NativeDeclarationAdmission *admission, uint64_t units) {
    if (admission->status != XR_NATIVE_DECLARATION_OK) return false;
    if (!admission->work->charge(admission->work->context, units)) {
        admission->status = XR_NATIVE_DECLARATION_WORK_LIMIT; return false;
    }
    return true;
}
static bool native_text_equal(NativeDeclarationAdmission *admission, const char *left, const char *right) {
    if (!left || !right) return false;
    for (size_t i = 0;; ++i) {
        if (!native_work(admission, 2)) return false;
        unsigned char a = (unsigned char)left[i], b = (unsigned char)right[i];
        if (a != b) return false;
        if (!a) return true;
    }
}
static bool native_fingerprint_equal(NativeDeclarationAdmission *admission,
    const XrFingerprint *left, const XrFingerprint *right) {
    const unsigned char *a = (const unsigned char *)left, *b = (const unsigned char *)right;
    for (size_t i = 0; i < sizeof(*left); ++i) {
        if (!native_work(admission, 2) || a[i] != b[i]) return false;
    }
    return true;
}
XR_FUNC XrNativeDeclarationStatus xr_native_declaration_find(
    const XrNativeDeclarationWork *work, const char *name, const XrNativeTypeDeclaration **output) {
    if (!work || !work->charge || !name || !output || *output) return XR_NATIVE_DECLARATION_INVALID;
    NativeDeclarationAdmission admission = {work, XR_NATIVE_DECLARATION_OK};
    const XrNativeTypeDeclaration *found = NULL;
    if (native_text_equal(&admission, name, xr_native_array.name)) found = &xr_native_array;
    else if (native_text_equal(&admission, name, xr_native_string.name)) found = &xr_native_string;
    else if (native_text_equal(&admission, name, xr_native_atomic.name)) found = &xr_native_atomic;
    else if (native_text_equal(&admission, name, xr_native_ordering.name)) found = &xr_native_ordering;
    if (admission.status != XR_NATIVE_DECLARATION_OK) return admission.status;
    if (!found) return XR_NATIVE_DECLARATION_NOT_FOUND;
    *output = found; return XR_NATIVE_DECLARATION_OK;
}
XR_FUNC XrNativeDeclarationStatus xr_native_declaration_find_member(
    const XrNativeDeclarationWork *work, const XrNativeTypeDeclaration *declaration,
    const char *name, const XrNativeMemberDeclaration **output) {
    if (!work || !work->charge || !declaration || !name || !output || *output)
        return XR_NATIVE_DECLARATION_INVALID;
    NativeDeclarationAdmission admission = {work, XR_NATIVE_DECLARATION_OK};
    if (!native_work(&admission, 1)) return admission.status;
    if (!declaration->members || declaration->member_count > 64) return XR_NATIVE_DECLARATION_INVALID;
    for (uint32_t i = 0; i < declaration->member_count; ++i) {
        if (!native_work(&admission, 1)) return admission.status;
        if (native_text_equal(&admission, declaration->members[i].name, name)) {
            *output = &declaration->members[i]; return XR_NATIVE_DECLARATION_OK;
        }
        if (admission.status != XR_NATIVE_DECLARATION_OK) return admission.status;
    }
    return XR_NATIVE_DECLARATION_NOT_FOUND;
}
static bool native_declaration_validate(NativeDeclarationAdmission *admission,
    const XrNativeTypeDeclaration *declaration) {
    if (!native_work(admission, 1)) return false;
    if (!declaration ||
        !declaration->name || !declaration->parameter_name || !declaration->source_path ||
        !declaration->identity || !declaration->members || !declaration->member_count ||
        declaration->member_count > 64) return false;
    const XrNativeTypeDeclaration *canonical = xr_native_declaration_by_id(declaration->id);
    if (!canonical || declaration->kind != canonical->kind ||
        declaration->parameter_count != canonical->parameter_count ||
        declaration->line != canonical->line || declaration->column != canonical->column ||
        !native_text_equal(admission, declaration->identity, canonical->identity) ||
        !native_text_equal(admission, declaration->source_path, canonical->source_path) ||
        !native_text_equal(admission, declaration->name, canonical->name) ||
        !native_text_equal(admission, declaration->parameter_name, canonical->parameter_name)) return false;
    if (!native_fingerprint_equal(admission, &declaration->source_fingerprint, &canonical->source_fingerprint) ||
        declaration->member_count != canonical->member_count)
        return false;
    for (uint32_t i = 0; i < declaration->member_count; ++i) {
        if (!native_work(admission, 1)) return false;
        const XrNativeMemberDeclaration *member = &declaration->members[i], *expected = &canonical->members[i];
        if (!member->name || !member->signature || !member->result_text || !member->failures ||
            member->id != expected->id || member->line != expected->line || member->column != expected->column ||
            !native_text_equal(admission, member->name, expected->name) ||
            !native_text_equal(admission, member->signature, expected->signature) ||
            !native_text_equal(admission, member->result_text, expected->result_text) ||
            member->receiver != expected->receiver || member->is_static != expected->is_static ||
            member->is_method != expected->is_method || member->lowered != expected->lowered ||
            member->is_public != expected->is_public ||
            member->operation != expected->operation || member->allocation != expected->allocation ||
            !native_text_equal(admission, member->failures, expected->failures) || member->ownership != expected->ownership ||
            member->result != expected->result || member->parameter_count != expected->parameter_count ||
            (member->parameter_count && !member->parameters)) return false;
        for (uint32_t p = 0; p < member->parameter_count; ++p) {
            if (!native_work(admission, 1)) return false;
            const XrNativeParameter *param = &member->parameters[p], *want = &expected->parameters[p];
            if (!param->name || !param->type_text || !native_text_equal(admission, param->name, want->name) ||
                !native_text_equal(admission, param->type_text, want->type_text) || param->type != want->type ||
                param->optional != want->optional || param->variadic != want->variadic) return false;
        }
    }
    return true;
}
XR_FUNC XrNativeDeclarationStatus xr_native_declaration_admit(
    const XrNativeDeclarationWork *work, const XrNativeTypeDeclaration *declaration) {
    if (!work || !work->charge || !declaration) return XR_NATIVE_DECLARATION_INVALID;
    NativeDeclarationAdmission admission = {work, XR_NATIVE_DECLARATION_OK};
    bool valid = native_declaration_validate(&admission, declaration);
    if (admission.status != XR_NATIVE_DECLARATION_OK) return admission.status;
    return valid ? XR_NATIVE_DECLARATION_OK : XR_NATIVE_DECLARATION_INVALID;
}
/* Pure borrowed queries use the same traversal without compiler accounting. */
static bool native_borrowed_work(void *context, uint64_t units) {
    (void)context; (void)units; return true;
}
XR_FUNC const XrNativeTypeDeclaration *xr_native_declaration_by_name(const char *name) {
    const XrNativeDeclarationWork work = {NULL, native_borrowed_work};
    const XrNativeTypeDeclaration *declaration = NULL;
    (void)xr_native_declaration_find(&work, name, &declaration); return declaration;
}
XR_FUNC const XrNativeMemberDeclaration *xr_native_declaration_member(
    const XrNativeTypeDeclaration *declaration, const char *name) {
    const XrNativeDeclarationWork work = {NULL, native_borrowed_work};
    const XrNativeMemberDeclaration *member = NULL;
    (void)xr_native_declaration_find_member(&work, declaration, name, &member); return member;
}
XR_FUNC bool xr_native_declaration_validate(const XrNativeTypeDeclaration *declaration) {
    const XrNativeDeclarationWork work = {NULL, native_borrowed_work};
    return xr_native_declaration_admit(&work, declaration) == XR_NATIVE_DECLARATION_OK;
}
