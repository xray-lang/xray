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
typedef enum XrNativeDeclarationId { XR_NATIVE_DECLARATION_ARRAY = 1, XR_NATIVE_DECLARATION_STRING = 2 } XrNativeDeclarationId;
typedef enum XrNativeTypeTerm {
    XR_NATIVE_TERM_UNADMITTED, XR_NATIVE_TERM_UNIT, XR_NATIVE_TERM_I64,
    XR_NATIVE_TERM_ELEMENT, XR_NATIVE_TERM_STRING, XR_NATIVE_TERM_BOOL,
    XR_NATIVE_TERM_RESULT_VARIABLE, XR_NATIVE_TERM_ARRAY_ELEMENT,
    XR_NATIVE_TERM_ARRAY_RESULT, XR_NATIVE_TERM_NULLABLE_ELEMENT,
    XR_NATIVE_TERM_CALLBACK_MAP, XR_NATIVE_TERM_CALLBACK_PREDICATE_INDEXED,
    XR_NATIVE_TERM_CALLBACK_REDUCE, XR_NATIVE_TERM_CALLBACK_VISIT,
    XR_NATIVE_TERM_CALLBACK_PREDICATE
} XrNativeTypeTerm;
/* RESULT_VARIABLE and the callback terms require an admitted recipe's exact
 * signature. They never grant authority to an arbitrary unparsed signature. */
typedef enum XrNativeReceiverMode {
    XR_NATIVE_RECEIVER_READ, XR_NATIVE_RECEIVER_REF, XR_NATIVE_RECEIVER_MOVE
} XrNativeReceiverMode;
typedef enum XrNativeOperation {
    XR_NATIVE_OPERATION_NONE, XR_NATIVE_OPERATION_ARRAY_GET,
    XR_NATIVE_OPERATION_ARRAY_SET, XR_NATIVE_OPERATION_ARRAY_PUSH,
    XR_NATIVE_OPERATION_STRING_CONTAINS, XR_NATIVE_OPERATION_STRING_STARTS_WITH,
    XR_NATIVE_OPERATION_STRING_ENDS_WITH, XR_NATIVE_OPERATION_STRING_INDEX_OF,
    XR_NATIVE_OPERATION_STRING_LAST_INDEX_OF,
    XR_NATIVE_OPERATION_ARRAY_MAP, XR_NATIVE_OPERATION_ARRAY_FILTER,
    XR_NATIVE_OPERATION_ARRAY_REDUCE, XR_NATIVE_OPERATION_ARRAY_FOR_EACH,
    XR_NATIVE_OPERATION_ARRAY_FIND, XR_NATIVE_OPERATION_ARRAY_FIND_INDEX,
    XR_NATIVE_OPERATION_ARRAY_EVERY, XR_NATIVE_OPERATION_ARRAY_SOME,
    XR_NATIVE_OPERATION_ARRAY_CONTAINS, XR_NATIVE_OPERATION_ARRAY_INDEX_OF,
    XR_NATIVE_OPERATION_ARRAY_JOIN, XR_NATIVE_OPERATION_ARRAY_CLEAR,
    XR_NATIVE_OPERATION_ARRAY_REVERSE, XR_NATIVE_OPERATION_ARRAY_UNSHIFT,
    XR_NATIVE_OPERATION_COUNT
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
typedef struct XrNativeDeclarationWork {
    void *context;
    bool (*charge)(void *context, uint64_t units);
} XrNativeDeclarationWork;
typedef enum XrNativeDeclarationStatus {
    XR_NATIVE_DECLARATION_OK, XR_NATIVE_DECLARATION_NOT_FOUND,
    XR_NATIVE_DECLARATION_INVALID, XR_NATIVE_DECLARATION_WORK_LIMIT
} XrNativeDeclarationStatus;
/* Work callbacks are borrowed for this synchronous operation. Every input
 * comparison is admitted before reading; failures leave output unchanged. */
XR_FUNC XrNativeDeclarationStatus xr_native_declaration_admit(
    const XrNativeDeclarationWork *work, const XrNativeTypeDeclaration *declaration);
XR_FUNC XrNativeDeclarationStatus xr_native_declaration_find(
    const XrNativeDeclarationWork *work, const char *name, const XrNativeTypeDeclaration **output);
XR_FUNC XrNativeDeclarationStatus xr_native_declaration_find_member(
    const XrNativeDeclarationWork *work, const XrNativeTypeDeclaration *declaration,
    const char *name, const XrNativeMemberDeclaration **output);
#endif // XNATIVE_DECLARATION_H
