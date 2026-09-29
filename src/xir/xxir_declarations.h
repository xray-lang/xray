/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_declarations.h - Module identities, literal bytes and instance slot types
 *
 * KEY CONCEPT:
 *   Compiler stages and sealed programs share one declaration schema.
 */
#ifndef XXIR_DECLARATIONS_H
#define XXIR_DECLARATIONS_H
#include "xxir_value.h"

typedef struct XrXirLiteral {
    const char *bytes;
    uint32_t length;
} XrXirLiteral;
typedef struct XrXirSourceModule {
    const char *name;
    uint32_t name_length;
    const uint32_t *dependencies;
    uint32_t dependency_count;
    uint32_t initializer;
} XrXirSourceModule;
typedef enum XrXirMemberAccess {
    XR_XIR_MEMBER_PUBLIC, XR_XIR_MEMBER_PRIVATE, XR_XIR_MEMBER_PROTECTED
} XrXirMemberAccess;

typedef struct XrXirFunctionIdentity {
    uint32_t module, exported, nominal_owner, member_access;
    /* Zero for ordinary functions; otherwise one plus the lexical owner index. */
    uint32_t cleanup_owner;
    uint32_t promises;
} XrXirFunctionIdentity;

#define XR_XIR_FUNCTION_NO_SUSPEND 1u
typedef struct XrXirSlot {
    uint32_t module;
    XrXirType type;
    uint32_t mutable;
} XrXirSlot;
typedef struct XrXirDeclarations {
    const XrXirSourceModule *modules;
    uint32_t module_count;
    const XrXirFunctionIdentity *functions;
    const XrXirSlot *slots;
    uint32_t slot_count;
    const XrXirLiteral *literals;
    uint32_t literal_count;
    uint32_t root_module, entry_function;
} XrXirDeclarations;
#endif // XXIR_DECLARATIONS_H
