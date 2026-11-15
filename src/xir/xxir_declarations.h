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
typedef enum XrXirConstantKeyKind { XR_XIR_KEY_DYNAMIC,XR_XIR_KEY_SCALAR,XR_XIR_KEY_STRING,XR_XIR_KEY_ENUM } XrXirConstantKeyKind;
/* Eligibility comes from checked syntax, independently of optimizer folding.
 * A receiving declaration owner copies the complete String span. */
typedef struct XrXirConstantKey {
    XrXirConstantKeyKind kind;
    uint64_t bits;
    XrXirLiteral string;
} XrXirConstantKey;
typedef enum XrXirModuleStorage {
    XR_XIR_MODULE_FRESH_STORAGE,
    XR_XIR_MODULE_BIRTH_STORAGE
} XrXirModuleStorage;
typedef struct XrXirSourceModule {
    const char *name;
    uint32_t name_length;
    const uint32_t *dependencies;
    uint32_t dependency_count;
    uint32_t initializer;
    /* Zero owns a lexical scope; otherwise one plus its canonical module.
     * Storage and initialization retain this module's independent identity. */
    uint32_t lexical_scope;
    /* Birth storage cannot initialize in a fresh Instance. Actual receiving
     * must bind its published module and every slot before execution. This
     * requirement is independent of lexical visibility and Sendable. */
    uint32_t storage;
} XrXirSourceModule;
typedef enum XrXirMemberAccess {
    XR_XIR_MEMBER_PUBLIC, XR_XIR_MEMBER_PRIVATE, XR_XIR_MEMBER_PROTECTED
} XrXirMemberAccess;
typedef enum XrXirMethodKind {
    XR_XIR_NON_MEMBER, XR_XIR_READ_METHOD, XR_XIR_STATIC_METHOD,
    XR_XIR_CONSTRUCTOR, XR_XIR_MEMBER_HELPER, XR_XIR_MATERIALIZER,
    XR_XIR_CONSTRUCTOR_FIELDS
} XrXirMethodKind;
typedef enum XrXirTestRole {
    XR_XIR_TEST_ROLE_NONE, XR_XIR_TEST_ROLE_TEST, XR_XIR_TEST_ROLE_SKIP,
    XR_XIR_TEST_ROLE_BEFORE_ALL, XR_XIR_TEST_ROLE_AFTER_ALL,
    XR_XIR_TEST_ROLE_BEFORE_EACH, XR_XIR_TEST_ROLE_AFTER_EACH
} XrXirTestRole;
typedef struct XrXirImplementationTable XrXirImplementationTable;

typedef struct XrXirFunctionIdentity {
    uint32_t module, exported, nominal_owner, member_access;
    /* Zero for ordinary functions; otherwise one plus the lexical owner index. */
    uint32_t cleanup_owner;
    uint32_t promises;
    uint32_t method_kind;
    uint32_t test_role, test_timeout_seconds;
    /* Zero is ordinary code; otherwise the canonical callable selects the body. */
    uint32_t native_callable;
    /* Lexical environments are declaration-owned physical parameter prefixes.
     * Partial application of ordinary parameters never changes these fields. */
    uint32_t capture_owner, capture_count;
} XrXirFunctionIdentity;

#define XR_XIR_FUNCTION_NO_SUSPEND 1u
#define XR_XIR_FUNCTION_GENERATOR 2u
#define XR_XIR_FUNCTION_REST 4u
#define XR_XIR_FUNCTION_OVERRIDE 8u
typedef struct XrXirSlot {
    uint32_t module;
    XrXirType type;
    uint32_t mutable;
} XrXirSlot;

/* Literal provenance is typed metadata, independent of executable operands.
 * Kind zero is an explicit key/value pair; kind one is a Map spread. */
typedef struct XrXirMapLiteralOrigin { uint32_t kind,line,column; } XrXirMapLiteralOrigin;
typedef struct XrXirMapLiteral {
    XrXirLiteral file;
    const XrXirMapLiteralOrigin *items;
    uint32_t count,module;
} XrXirMapLiteral;
typedef enum XrXirParameterNameOwner {
    XR_XIR_PARAMETER_FUNCTION, XR_XIR_PARAMETER_REQUIREMENT
} XrXirParameterNameOwner;

/* Declaration names are API metadata, independently of callable type identity.
 * The public tail excludes physical receivers and lexical capture prefixes.
 * An empty name denotes a destructuring parameter without a callable label. */
typedef struct XrXirParameterNames {
    uint32_t kind, owner, member, parameter_offset;
    const XrXirLiteral *names;
    uint32_t count;
} XrXirParameterNames;

/* A candidate binds real nominal and method IDs. Receiving verification reads
 * the current function owner again; this row stores no proof result. */
typedef struct XrXirHashableWitness {
    XrXirType nominal_type;
    uint32_t equals_function, hash_function;
} XrXirHashableWitness;

typedef struct XrXirDeclarations {
    const XrXirSourceModule *modules;
    uint32_t module_count;
    const XrXirFunctionIdentity *functions;
    const XrXirSlot *slots;
    uint32_t slot_count;
    const XrXirLiteral *literals;
    uint32_t literal_count;
    uint32_t root_module, entry_function;
    const XrXirImplementationTable *implementations;
    const XrXirMapLiteral *map_literals;
    uint32_t map_literal_count;
    const XrXirParameterNames *parameter_names;
    uint32_t parameter_name_count;
    const XrXirHashableWitness *hashable_witnesses;
    uint32_t hashable_witness_count;
} XrXirDeclarations;

/* The receiving validator proves strict ordering before any lookup. */
static inline const XrXirParameterNames *xr_xir_parameter_names_find(
    const XrXirDeclarations *d, uint32_t kind, uint32_t owner, uint32_t member) {
    if (!d) return NULL;
    uint32_t low=0,high=d->parameter_name_count;
    while (low<high) {
        uint32_t mid=low+(high-low)/2;
        const XrXirParameterNames *row=&d->parameter_names[mid];
        int order=row->kind<kind?-1:row->kind>kind?1:row->owner<owner?-1:
            row->owner>owner?1:row->member<member?-1:row->member>member?1:0;
        if (!order) return row;
        if (order<0) low=mid+1; else high=mid;
    }
    return NULL;
}
#endif // XXIR_DECLARATIONS_H
