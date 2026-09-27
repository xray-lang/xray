/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_declaration.h - Source-backed immutable native declaration facts
 *
 * KEY CONCEPT:
 *   A governed declaration selects language identity before any lowering.
 */
#ifndef XNATIVE_DECLARATION_H
#define XNATIVE_DECLARATION_H
#include "../base/xdefs.h"
#include "../base/xstable_id.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum XrNativeDeclarationKind {
    XR_NATIVE_DECLARATION_UNSPECIFIED, XR_NATIVE_DECLARATION_VALUE,
    XR_NATIVE_DECLARATION_IDENTITY
} XrNativeDeclarationKind;
typedef enum XrNativeDeclarationId { XR_NATIVE_DECLARATION_ARRAY = 1 } XrNativeDeclarationId;
typedef enum XrNativeTypeTerm {
    XR_NATIVE_TERM_UNADMITTED, XR_NATIVE_TERM_UNIT, XR_NATIVE_TERM_I64,
    XR_NATIVE_TERM_ELEMENT
} XrNativeTypeTerm;
typedef enum XrNativeReceiverMode {
    XR_NATIVE_RECEIVER_READ, XR_NATIVE_RECEIVER_REF, XR_NATIVE_RECEIVER_MOVE
} XrNativeReceiverMode;
typedef enum XrNativeOperation {
    XR_NATIVE_OPERATION_NONE, XR_NATIVE_OPERATION_ARRAY_GET,
    XR_NATIVE_OPERATION_ARRAY_SET, XR_NATIVE_OPERATION_ARRAY_PUSH
} XrNativeOperation;
typedef enum XrNativeAllocation {
    XR_NATIVE_ALLOCATION_UNKNOWN, XR_NATIVE_ALLOCATION_NO_HEAP, XR_NATIVE_ALLOCATION_MAY_HEAP
} XrNativeAllocation;
typedef enum XrNativeResultOwnership {
    XR_NATIVE_OWNERSHIP_UNKNOWN, XR_NATIVE_OWNERSHIP_OWNED, XR_NATIVE_OWNERSHIP_UNIT
} XrNativeResultOwnership;
typedef struct XrNativeParameter {
    const char *name, *type_text;
    XrNativeTypeTerm type;
    bool optional, variadic;
} XrNativeParameter;
typedef struct XrNativeMemberDeclaration {
    uint32_t id;
    const char *name, *signature, *result_text;
    uint32_t line, column;
    XrNativeReceiverMode receiver;
    bool is_static, is_method, lowered, is_public;
    XrNativeOperation operation;
    XrNativeAllocation allocation;
    const char *failures;
    XrNativeResultOwnership ownership;
    const XrNativeParameter *parameters;
    uint32_t parameter_count;
    XrNativeTypeTerm result;
} XrNativeMemberDeclaration;
typedef struct XrNativeTypeDeclaration {
    uint32_t id;
    XrNativeDeclarationKind kind;
    const char *name, *parameter_name;
    uint32_t parameter_count;
    const char *source_path, *identity;
    XrFingerprint source_fingerprint;
    uint32_t line, column;
    const XrNativeMemberDeclaration *members;
    uint32_t member_count;
} XrNativeTypeDeclaration;
XR_FUNC const XrNativeTypeDeclaration *xr_native_declaration_by_id(uint32_t id);
XR_FUNC const XrNativeTypeDeclaration *xr_native_declaration_by_name(const char *name);
XR_FUNC const XrNativeMemberDeclaration *xr_native_declaration_member(
    const XrNativeTypeDeclaration *declaration, const char *name);
XR_FUNC bool xr_native_declaration_validate(const XrNativeTypeDeclaration *declaration);
#endif // XNATIVE_DECLARATION_H
