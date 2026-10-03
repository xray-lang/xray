/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source.c - Declaration checking directly into typed Built XIR
 *
 * KEY CONCEPT:
 *   Source names are resolved before lowering and never rediscovered at runtime.
 */
#include "xxir_source.h"
#include "xxir_operand_roles.h"
#include "xxir_library_catalog.h"
#include "xxir_constraint_proof.h"
#include "xxir_implementation.h"
#include "xxir_implementation_verify.h"
#include "xxir_interface_members.h"
#include "xxir_initialization.h"
#include "xxir_source_query_internal.h"
#include "xxir_generic.h"
#include "xxir_types.h"
#include "xxir_nominal.h"
#include "xxir_float.h"
#include "xxir_compile_memory.h"
#include "../shared/xr_decimal_float.h"
#include "../shared/xnative_declaration.h"
#include "../shared/xr_core_intrinsic.h"
#include "../shared/xr_type_names_core.h"
#include "../module/xmodule_graph.h"
#include "../module/xlockfile.h"
#include "../toolchain/xcompiler_session.h"
#include "../module/xdeclaration_load.h"
#include "../os/os_file_read.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xast_walk.h"
#include "../frontend/parser/xtype_ref.h"
#include "../frontend/xdiag_fmt.h"
#include "../base/xmalloc.h"
#include <stdio.h>
#include <limits.h>

typedef struct SourceMemory { struct SourceMemory *next; struct SourceMemory **previous; } SourceMemory;
typedef struct SourceManifest {
    struct SourceManifest *next;
    XrModuleIdentityAuthority authority;
    XrDeclarationManifest *declarations;
} SourceManifest;
typedef enum SourceKind { SOURCE_SLOT, SOURCE_UNIT_SLOT, SOURCE_FUNCTION, SOURCE_MODULE, SOURCE_IMPORT, SOURCE_LOCAL, SOURCE_UNIT_LOCAL, SOURCE_NOMINAL, SOURCE_INTERFACE, SOURCE_PENDING } SourceKind;
typedef struct SourceFact SourceFact;
typedef struct SourceName {
    struct SourceName *next;
    const char *name, *imported;
    AstNode *node;
    SourceKind kind;
    uint32_t index, module;
    XrXirType type;
    bool mutable, construction;
    uint32_t declaration;
} SourceName;
/* SOURCE_UNIT_LOCAL is an initialized logical declaration with no runtime
 * payload. Its index is never an executable value or a capture ordinal.
 * SOURCE_PENDING reserves a nested function name for its whole block before the
 * declaration runs, so an earlier use fails instead of resolving to an outer name. */
static bool source_local_name(const SourceName *name) {
    return name && (name->kind == SOURCE_LOCAL || name->kind == SOURCE_UNIT_LOCAL);
}
static bool source_name_ready(const SourceName *name) {
    return name && (name->kind == SOURCE_UNIT_LOCAL || name->kind == SOURCE_UNIT_SLOT ||
        ((name->kind == SOURCE_LOCAL || name->kind == SOURCE_SLOT) && name->type != XR_XIR_UNIT));
}
typedef struct SourceErrorContext SourceErrorContext;
/* `infer_result` leaves a callable context's result open so a closure's own body decides it. */
typedef struct SourceExpectedType { bool present; XrXirType type; bool infer_result; } SourceExpectedType;
typedef struct SourceConversionRecipe {
    bool needed;
    XrXirOp operation;
    XrXirType source, target;
} SourceConversionRecipe;

typedef struct SourceInteger { bool present, negative; uint64_t magnitude; } SourceInteger;

typedef struct SourceDecimal { AstNode *node; bool negative; } SourceDecimal;

typedef struct SourceNumericRecipe {
    bool ready;
    XrXirOp operation;
    XrXirType type;
    int64_t payload;
} SourceNumericRecipe;

typedef struct SourceValue { uint32_t id; XrXirType type; } SourceValue;
typedef struct SourceBlockReference { uint32_t owner, identity; } SourceBlockReference;

typedef enum SourceTermState { SOURCE_TERM_UNRESOLVED, SOURCE_TERM_CHECKING, SOURCE_TERM_GROUND, SOURCE_TERM_FAILED, SOURCE_TERM_DISCARDED } SourceTermState;
typedef struct SourceExpressionPlan {
    struct SourceExpressionPlan *parent, *left, *right, *condition;
    struct SourceConditionalRecipe *conditional;
    struct SourceMatchPlan *match;
    struct SourceBinaryRecipe *binary;
    AstNode *syntax;
    SourceName *binding;
    uint32_t owner, identity;
    SourceExpectedType expected;
    SourceBlockReference entry, exit;
    SourceTermState state;
    bool type_ready, conversion_ready, statement_match;
    XrXirType ground_type;
    SourceConversionRecipe conversion;
    SourceInteger integer;
    SourceDecimal decimal;
    SourceNumericRecipe numeric;
    SourceValue value;
} SourceExpressionPlan;
typedef struct SourceExpressionStorage {
    struct SourceExpressionStorage *next;
    uint32_t count;
    SourceExpressionPlan expressions[32];
} SourceExpressionStorage;
typedef struct SourceInstructionRecipe { XrXirInstruction instruction; uint32_t owner, value; } SourceInstructionRecipe;
typedef struct SourceBlockRecipe { XrXirBlock block; uint32_t owner, identity, runtime; } SourceBlockRecipe;
typedef struct SourceRecipeStorage { struct SourceRecipeStorage *next; } SourceRecipeStorage;
typedef struct SourceFunction {
    AstNode *node, *type_owner;
    bool checked_library;
    XrGenericParam **type_parameters;
    uint32_t type_parameter_count;
    bool infer_result, saw_return, constructor_captured, constructor_shared;
    uint32_t module, count, capacity;
    XrXirType *parameters;
    uint32_t *operands, operand_count, operand_capacity;
    uint32_t type_capacity;
    SourceBlockRecipe *blocks;
    uint32_t block_count, block_capacity, frontier, lexical_depth;
    SourceBlockReference current_block;
    SourceInstructionRecipe *recipes;
    SourceRecipeStorage *recipe_storage;
    bool region_sealed;
    SourceExpressionStorage *expressions;
    uint32_t expression_count;
    uint32_t declaration, generic_owner;
    uint32_t *constructor_places;
    AstNode *default_expression;
    SourceErrorContext *error_context;
    XrXirInitializationRegion *initialization_regions, *initialization_parent;
} SourceFunction;
typedef struct SourcePatch { struct SourcePatch *next; uint32_t instruction; } SourcePatch;
typedef struct SourceLoop { struct SourceLoop *parent; SourcePatch *breaks, *continues; uint32_t frontier; } SourceLoop;
typedef struct SourceErrorEdge {
    struct SourceErrorEdge *next;
    uint32_t block, jump;
    SourceValue value;
} SourceErrorEdge;
struct SourceErrorContext { SourceErrorEdge *edges; uint32_t count, frontier; };

typedef struct SourceTypeScope {
    bool active;
    AstNode *node;
    XrGenericParam **parameters;
    uint32_t count, generic_owner;
    uint32_t interface_member;
} SourceTypeScope;
typedef struct SourceContext {
    XrModuleGraph *graph;
    XrXirLinkageKind linkage_kind;
    XrXirCompileContext compile;
    uint32_t remaining_blocks, remaining_instructions;
    XrXirSourceDiagnostic diagnostic;
    SourceMemory *memory;
    SourceManifest *manifests;
    XrXirFunction *functions;
    SourceFunction *bodies;
    XrXirSourceModule *modules;
    XrXirFunctionIdentity *identities;
    XrXirGeneric *generics;
    XrXirTypes types;
    XrXirNominalTable nominals;
    SourceTypeScope type_scope;
    XrXirInterfaceTable interfaces;
    XrXirImplementationTable implementations;
    XrXirDefaultTable defaults;
    uint32_t default_capacity;
    bool implementations_ready, declarations_building;
    SourceName **interface_sources, **interface_members;
    uint32_t **interface_member_declarations;
    uint32_t **nominal_members, **nominal_variants;
    uint32_t **nominal_defaults;
    SourceName **nominal_sources, **nominal_methods;
    bool *nominal_defaultable;
    uint32_t *nominal_constructors;
    uint32_t type_capacity;
    XrXirSlot *slots;
    XrXirLiteral *literals;
    SourceName **names, *locals, *scope;
    SourceFact *facts;
    uint32_t flow_alternatives;
    uint32_t function_count, slot_count, literal_count, literal_capacity;
    uint32_t module_count, entry_function;
    bool core_factory;
    SourceName *assertion, *assert_panics, *assert_equal;
    uint32_t first_closure, next_closure, closure_limit, function_capacity;
    uint32_t function, module, depth;
    SourceExpressionPlan *active_expression;
    SourceLoop *loop;
    bool returned, has_generics, query_ready;
    XrXirSourceView query;
    uint32_t declaration_capacity, reference_capacity, expression_capacity;
    uint32_t query_module_capacity, array_module, array_declaration, array_members[XR_NATIVE_OPERATION_COUNT], length_declaration;
    uint32_t string_module, string_declaration, string_members[5];
} SourceContext;

static XrXirModule source_module_view(SourceContext *ctx, XrXirDeclarations *declarations) {
    *declarations = (XrXirDeclarations){ctx->modules,ctx->module_count,ctx->identities,
        ctx->slots,ctx->slot_count,ctx->literals,ctx->literal_count,
        ctx->linkage_kind == XR_XIR_LIBRARY ? UINT32_MAX : (uint32_t)ctx->graph->entry_index,
        ctx->linkage_kind == XR_XIR_LIBRARY ? UINT32_MAX : ctx->entry_function,
        ctx->implementations.count ? &ctx->implementations : NULL};
    return (XrXirModule){XR_XIR_BUILT,ctx->functions,ctx->function_count,declarations,
        ctx->has_generics ? ctx->generics : NULL,
        ctx->types.count || ctx->types.nominals || ctx->types.interfaces ? &ctx->types : NULL,NULL,ctx->linkage_kind,
        ctx->defaults.count ? &ctx->defaults : NULL};
}
static bool source_format_work(void *context, uint64_t units) {
    return xir_compile_work(context, units);
}
static XrDiagStatus source_format(SourceContext *ctx, char *output, size_t capacity,
    const char *format, ...) {
    XrDiagBuffer buffer = {output, capacity, 0, &ctx->compile, source_format_work};
    XrDiagPolicy policy = {&buffer, xr_diag_direct_read, xr_diag_buffer_write,
        xr_diag_buffer_work, XR_DIAG_OK, false};
    va_list arguments;
    va_start(arguments, format);
    XrDiagStatus status = xr_diag_format_v(&policy, format, arguments);
    va_end(arguments);
    return status == XR_DIAG_OK ? xr_diag_buffer_finish(&policy, &buffer) : status;
}
static bool source_fail(SourceContext *ctx, AstNode *node, XrXirStatus status, const char *message) {
    if (ctx->diagnostic.status == XR_XIR_OK) {
        if (!node && ctx->type_scope.active) node = ctx->type_scope.node;
        ctx->diagnostic.status = status;
        ctx->diagnostic.module = ctx->module;
        ctx->diagnostic.line = node ? node->line : 0;
        ctx->diagnostic.column = node ? node->column : 0;
        /* Resource failure needs no further work to remain observable. Invalid
         * requests have no owner from which diagnostic work could be charged. */
        if (ctx->compile.resources && status != XR_XIR_BUDGET && status != XR_XIR_OUT_OF_MEMORY) {
            XrDiagStatus formatted = source_format(ctx, ctx->diagnostic.message,
                sizeof(ctx->diagnostic.message), "%s", message);
            if (formatted != XR_DIAG_OK)
                ctx->diagnostic.status = formatted == XR_DIAG_RESOURCE ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
        }
    }
    return false;
}
/* Names the syntax family and the parser node kind so an unadmitted construct is traceable. */
static bool source_fail_syntax(SourceContext *ctx, AstNode *node, const char *family) {
    char message[128];
    if (source_format(ctx, message, sizeof(message), "%s syntax is not implemented in XIR (node %d)",
            family, node ? (int) node->type : -1) != XR_DIAG_OK)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "syntax is not implemented in XIR");
    return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, message);
}
static bool source_module_fail(SourceContext *ctx, AstNode *node, XrModuleStatus status, const char *message) {
    XrXirStatus mapped = status == XR_MODULE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
        status == XR_MODULE_BUDGET ? XR_XIR_BUDGET :
        status == XR_MODULE_IO ? XR_XIR_IO :
        status == XR_MODULE_NOT_FOUND ? XR_XIR_UNRESOLVED : XR_XIR_BAD_STRUCTURE;
    return source_fail(ctx, node, mapped, message);
}
static void *source_alloc(SourceContext *ctx, size_t count, size_t size) {
    if (ctx->diagnostic.status != XR_XIR_OK) return NULL;
    if (!size || count > (SIZE_MAX - sizeof(SourceMemory)) / size) {
        source_fail(ctx, NULL, XR_XIR_BUDGET, "source metadata budget exhausted"); return NULL;
    }
    size_t bytes = sizeof(SourceMemory) + count * size;
    XrXirStatus status = XR_XIR_OK;
    SourceMemory *memory = xir_compile_calloc(&ctx->compile, 1, bytes, &status);
    if (!memory) { source_fail(ctx, NULL, status, "source allocation failed"); return NULL; }
    memory->next = ctx->memory; memory->previous = &ctx->memory;
    if (ctx->memory) ctx->memory->previous = &memory->next;
    ctx->memory = memory;
    return memory + 1;
}
static void source_release_private(void *pointer) {
    SourceMemory *memory=(SourceMemory *)pointer-1;
    *memory->previous=memory->next;
    if (memory->next) memory->next->previous=memory->previous;
    xr_compile_resources_free(memory);
}
static void *source_recipe_storage(SourceContext *ctx, uint32_t count, size_t size) {
    if (count>(SIZE_MAX-sizeof(SourceRecipeStorage))/size) {
        source_fail(ctx,NULL,XR_XIR_BUDGET,"source region storage exhausted"); return NULL;
    }
    SourceFunction *body=&ctx->bodies[ctx->function];
    SourceRecipeStorage *storage=source_alloc(ctx,1,sizeof(*storage)+(size_t)count*size);
    if (!storage) return NULL;
    storage->next=body->recipe_storage; body->recipe_storage=storage;
    return storage+1;
}
static bool source_work_units(SourceContext *ctx, AstNode *node, uint64_t units) {
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    return xir_compile_work(&ctx->compile, units) ||
        source_fail(ctx, node, XR_XIR_BUDGET, "source work budget exhausted");
}
static bool source_work(SourceContext *ctx, AstNode *node) {
    return source_work_units(ctx, node, 1);
}
static bool source_text_length(SourceContext *ctx, AstNode *node, const char *text, size_t *output) {
    if (!text) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "source text is missing");
    for (size_t length = 0;; ++length) {
        if (!source_work(ctx, node)) return false;
        if (!text[length]) { *output = length; return true; }
        if (length == SIZE_MAX - 1)
            return source_fail(ctx, node, XR_XIR_BUDGET, "source text size exhausted");
    }
}
/* Names are encoded with a u32 byte length. A failed scan records the precise
 * failure on the construction context; no later owner may be published. */
static size_t source_text_size(SourceContext *ctx, const char *text) {
    size_t length = 0;
    if (!source_text_length(ctx, NULL, text, &length)) return 0;
    if (length > UINT32_MAX) {
        source_fail(ctx, NULL, XR_XIR_BUDGET, "source name length exhausted"); return 0;
    }
    return length;
}
static bool source_copy_bytes(SourceContext *ctx, AstNode *node, void *destination,
    const void *source, size_t bytes) {
    if (!source_work_units(ctx, node, bytes)) return false;
    if (bytes) memcpy(destination, source, bytes);
    return true;
}
static const char *source_text_find(SourceContext *ctx, const char *text, char needle) {
    if (!text) return NULL;
    for (size_t i = 0;; ++i) {
        if (!source_work(ctx, NULL)) return NULL;
        if (text[i] == needle) return text + i;
        if (!text[i]) return NULL;
        if (i == SIZE_MAX - 1) {
            source_fail(ctx, NULL, XR_XIR_BUDGET, "source text size exhausted"); return NULL;
        }
    }
}
static bool source_text_same(SourceContext *ctx, AstNode *node, const char *first, const char *second) {
    if (!first || !second) return first == second;
    for (size_t i = 0;; ++i) {
        if (!source_work_units(ctx, node, 2)) return false;
        char a = first[i], b = second[i];
        if (a != b) return false;
        if (!a) return true;
    }
}
static bool source_span_same(SourceContext *ctx, AstNode *node,
    const void *first, const void *second, size_t length) {
    const unsigned char *a = first, *b = second;
    for (size_t i = 0; i < length; ++i) {
        if (!source_work_units(ctx, node, 2) || a[i] != b[i]) return false;
    }
    return true;
}
static bool source_algorithm_work(void *owner, uint64_t units) {
    return source_work_units(owner, NULL, units);
}
static bool source_native_admit(SourceContext *ctx, const XrNativeTypeDeclaration *declaration) {
    XrNativeDeclarationWork work = {ctx, source_algorithm_work};
    XrNativeDeclarationStatus status = xr_native_declaration_admit(&work, declaration);
    return status == XR_NATIVE_DECLARATION_OK || source_fail(ctx, NULL,
        status == XR_NATIVE_DECLARATION_WORK_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE,
        "native declaration authority is inconsistent");
}
static const XrNativeTypeDeclaration *source_native_find(SourceContext *ctx, const char *name) {
    XrNativeDeclarationWork work = {ctx, source_algorithm_work};
    const XrNativeTypeDeclaration *found = NULL;
    XrNativeDeclarationStatus status = xr_native_declaration_find(&work, name, &found);
    if (status != XR_NATIVE_DECLARATION_OK && status != XR_NATIVE_DECLARATION_NOT_FOUND)
        source_fail(ctx, NULL, status == XR_NATIVE_DECLARATION_WORK_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE,
            "native declaration lookup failed");
    return found;
}
static const XrNativeMemberDeclaration *source_native_member(SourceContext *ctx,
    const XrNativeTypeDeclaration *declaration, const char *name) {
    XrNativeDeclarationWork work = {ctx, source_algorithm_work};
    const XrNativeMemberDeclaration *found = NULL;
    XrNativeDeclarationStatus status = xr_native_declaration_find_member(&work, declaration, name, &found);
    if (status != XR_NATIVE_DECLARATION_OK && status != XR_NATIVE_DECLARATION_NOT_FOUND)
        source_fail(ctx, NULL, status == XR_NATIVE_DECLARATION_WORK_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE,
            "native member lookup failed");
    return found;
}
static void *source_scratch(SourceContext *ctx, size_t count, size_t size, bool clear) {
    if (ctx->diagnostic.status != XR_XIR_OK) return NULL;
    if (!size || count > SIZE_MAX / size) {
        source_fail(ctx, NULL, XR_XIR_BUDGET, "source temporary storage exhausted"); return NULL;
    }
    XrXirStatus status = XR_XIR_OK;
    void *memory = clear ? xir_compile_calloc(&ctx->compile, count, size, &status) :
        xir_compile_alloc(&ctx->compile, count * size, &status);
    if (status != XR_XIR_OK) source_fail(ctx, NULL, status, "source temporary allocation failed");
    return memory;
}
static void *source_query_append(SourceContext *ctx, const void *array, uint32_t *count,
    uint32_t *capacity, size_t size) {
    if (!source_work(ctx, NULL)) return NULL;
    if (*count == *capacity) {
        if (*capacity > UINT32_MAX / 2) { source_fail(ctx, NULL, XR_XIR_BUDGET, "source query identity exhausted"); return NULL; }
        uint32_t next = *capacity ? *capacity * 2 : 16;
        void *grown = source_alloc(ctx, next, size);
        if (!grown) return NULL;
        if (!source_copy_bytes(ctx, NULL, grown, array, (size_t) *count * size)) return NULL;
        array = grown; *capacity = next;
    }
    ++*count;
    return (void *) array;
}
static XrXirSourceRange source_query_range(SourceContext *ctx, AstNode *node, const char *name) {
    XrXirSourceRange range = {ctx->module, 0, 0, 0, 0};
    if (!node) return range;
    range.line = node->line; range.column = node->column;
    range.end_line = node->end_line; range.end_column = node->end_column;
    if (name && range.line > 0 && range.column > 0 && source_text_size(ctx, name) <= (size_t) (INT_MAX - range.column)) {
        range.end_line = range.line; range.end_column = range.column + (int) source_text_size(ctx, name);
    }
    return range;
}
static XrXirSourceType source_query_type(SourceContext *ctx, XrXirType type) {
    uint32_t owner = 0;
    if (xr_xir_type_span(&ctx->types, type))
        owner = ctx->type_scope.active ? ctx->type_scope.generic_owner : ctx->bodies[ctx->function].generic_owner;
    return (XrXirSourceType) {type, owner, true};
}
static bool source_query_declare(SourceContext *ctx, SourceName *symbol,
    XrXirSourceDeclarationKind kind, uint32_t parent, XrXirSourceRange range) {
    XrXirSourceDeclaration *records = source_query_append(ctx, ctx->query.declarations,
        &ctx->query.declaration_count, &ctx->declaration_capacity, sizeof(*records));
    if (!records) return false;
    ctx->query.declarations = records;
    XrXirSourceDeclaration *record = &records[ctx->query.declaration_count - 1];
    symbol->declaration = ctx->query.declaration_count;
    *record = (XrXirSourceDeclaration) {symbol->declaration, parent, 0, kind, symbol->name, range,
        {0}, NULL, 0, symbol->mutable,
        (kind == XR_XIR_SOURCE_FUNCTION || kind == XR_XIR_SOURCE_TYPE) && !parent && symbol->node && symbol->node->is_exported, 0, NULL, 0, 0, 0, NULL, NULL};
    return true;
}
static void source_query_binding_type(SourceContext *ctx, SourceName *symbol) {
    XrXirSourceDeclaration *records = (XrXirSourceDeclaration *) ctx->query.declarations;
    if (symbol->declaration) records[symbol->declaration - 1].type = source_query_type(ctx, symbol->type);
}
static bool source_query_reference(SourceContext *ctx, AstNode *node, SourceName *binding,
    SourceName *target, XrXirSourceAccess access) {
    if (!binding || !target || !binding->declaration || !target->declaration) return true;
    XrXirSourceReference *records = source_query_append(ctx, ctx->query.references,
        &ctx->query.reference_count, &ctx->reference_capacity, sizeof(*records));
    if (!records) return false;
    ctx->query.references = records;
    XrXirSourceReference *record = &records[ctx->query.reference_count - 1];
    *record = (XrXirSourceReference) {source_query_range(ctx, node, NULL), binding->declaration, target->declaration, access};
    return true;
}
static bool source_query_expression(SourceContext *ctx, AstNode *node, XrXirType type) {
    XrXirSourceExpression *records = source_query_append(ctx, ctx->query.expressions,
        &ctx->query.expression_count, &ctx->expression_capacity, sizeof(*records));
    if (!records) return false;
    ctx->query.expressions = records;
    XrXirSourceExpression *record = &records[ctx->query.expression_count - 1];
    *record = (XrXirSourceExpression) {node->node_id, source_query_range(ctx, node, NULL), source_query_type(ctx, type)};
    return true;
}
static bool source_query_parameters(SourceContext *ctx, uint32_t declaration) {
    const XrXirFunction *function = &ctx->functions[ctx->function];
    XrXirSourceType *parameters = function->parameter_count ? source_alloc(ctx, function->parameter_count, sizeof(*parameters)) : NULL;
    if (function->parameter_count && !parameters) return false;
    for (uint32_t p = 0; p < function->parameter_count; ++p) {
        if (!source_work(ctx, NULL)) return false;
        XrXirType shown = function->parameters[p];
        if (xr_xir_type_is_cell(&ctx->types, shown)) shown = xr_xir_cell_element(&ctx->types, shown);
        parameters[p] = source_query_type(ctx, shown);
    }
    XrXirSourceDeclaration *record = (XrXirSourceDeclaration *) &ctx->query.declarations[declaration - 1];
    record->type = source_query_type(ctx, function->result);
    record->parameters = parameters; record->parameter_count = function->parameter_count;
    record->generic_parameter_count = ctx->generics[ctx->function].parameter_count;
    record->generic_constraints = ctx->generics[ctx->function].constraints;
    record->type_parameter_kinds = ctx->generics[ctx->function].parameter_kinds;
    uint32_t owner = ctx->bodies[ctx->function].generic_owner;
    if (owner && owner != declaration) {
        record->generic_parent = owner;
        record->generic_parent_count = record->generic_parameter_count;
    }
    return true;
}
static SourceName *find_name(SourceContext *ctx, SourceName *names, const char *name) {
    for (SourceName *p = names; p; p = p->next) {
        if (!source_work(ctx, p->node)) return NULL;
        if (source_text_same(ctx, NULL, p->name, name)) return p;
    }
    return NULL;
}
static SourceName *add_name(SourceContext *ctx, SourceName **head, const char *name, AstNode *node) {
    if (!name || !*name || find_name(ctx, *head, name)) {
        source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate or empty declaration name"); return NULL;
    }
    SourceName *symbol = source_alloc(ctx, 1, sizeof(*symbol));
    if (symbol) { symbol->name = name; symbol->node = node; symbol->next = *head; *head = symbol; }
    return symbol;
}
static bool source_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type);
static bool source_nominal_type(SourceContext *ctx, const char *name, XrXirType *type);
static SourceName *source_nominal_name(SourceContext *ctx, const char *name);
static bool source_nominal_arguments(SourceContext *ctx, const char *name, XrTypeRef **arguments,
    uint32_t count, XrXirType *type);
static bool source_intern_type(SourceContext *ctx, XrXirTypeNode node, XrXirType *type) {
    uint32_t span = xr_xir_type_span(&ctx->types,
        node.kind == XR_XIR_TYPE_CALLABLE ? node.result : node.element);
    for (uint32_t p = 0; p < node.parameter_count; ++p) {
        if (!source_work(ctx, NULL)) return false;
        if (!node.parameters[p].type) return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "callable parameter must be a value type");
        uint32_t component = xr_xir_type_span(&ctx->types, node.parameters[p].type);
        if (component > span) span = component;
    }
    node.parameter_span = span;
    if (node.kind == XR_XIR_TYPE_NOMINAL) {
        for (uint32_t a = 0; a < node.nominal.argument_count; ++a) {
            if (!source_work(ctx, NULL)) return false;
            uint32_t component = xr_xir_type_span(&ctx->types, node.nominal.arguments[a]);
            if (component > node.parameter_span) node.parameter_span = component;
        }
    }
    for (uint32_t i = 0; i < ctx->types.count; ++i) {
        const XrXirTypeNode *s = &ctx->types.nodes[i];
        if (!source_work(ctx, NULL)) return false;
        if (s->kind != node.kind || s->element != node.element || s->parameter_count != node.parameter_count ||
            s->result != node.result || s->flags != node.flags) continue;
        if (node.kind == XR_XIR_TYPE_NOMINAL && (s->nominal.declaration != node.nominal.declaration ||
            s->nominal.argument_count != node.nominal.argument_count)) continue;
        bool same = true;
        for (uint32_t a = 0; a < node.nominal.argument_count; ++a) {
            if (!source_work(ctx, NULL)) return false;
            if (s->nominal.arguments[a] != node.nominal.arguments[a]) same = false;
        }
        for (uint32_t p = 0; p < node.parameter_count; ++p) {
            if (!source_work(ctx, NULL)) return false;
            if (s->parameters[p].type != node.parameters[p].type ||
                s->parameters[p].mode != node.parameters[p].mode) same = false;
        }
        if (same) { *type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + i); return true; }
    }
    const uint32_t limit = XR_XIR_CONSTRUCTED_TYPE_LIMIT - XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (ctx->types.count == limit)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "constructed type identity budget exhausted");
    if (ctx->types.count == ctx->type_capacity) {
        uint32_t capacity = ctx->type_capacity ? ctx->type_capacity * 2 : 8;
        if (capacity > limit) capacity = limit;
        XrXirTypeNode *table = source_alloc(ctx, capacity, sizeof(*table));
        if (!table) return false;
        if (!source_copy_bytes(ctx, NULL, table, ctx->types.nodes, ctx->types.count * sizeof(*table))) return false;
        ctx->types.nodes = table; ctx->type_capacity = capacity;
    }
    XrXirTypeNode *table = (XrXirTypeNode *) ctx->types.nodes;
    table[ctx->types.count] = node;
    *type = (XrXirType) (XR_XIR_CONSTRUCTED_TYPE_BASE + ctx->types.count++); return true;
}
static bool source_signature(SourceContext *ctx, const XrXirCallableParameter *parameters,
    uint32_t count, XrXirType result, XrXirType *type) {
    return source_intern_type(ctx, (XrXirTypeNode) {XR_XIR_TYPE_CALLABLE, XR_XIR_UNIT,
        parameters, count, result, 0, 0, {0}}, type);
}
static bool source_reference_promise(SourceContext *ctx, AstNode *node,
    uint32_t function, SourceExpectedType expected, XrXirType *type) {
    const XrXirTypeNode *context = expected.present ? xr_xir_callable_signature(&ctx->types, expected.type) : NULL;
    if (!context || context->flags != XR_XIR_CALLABLE_NO_SUSPEND) return true;
    if (!(ctx->identities[function].promises & XR_XIR_FUNCTION_NO_SUSPEND))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "qualified reference requires an explicit target promise");
    XrXirTypeNode signature = *xr_xir_callable_signature(&ctx->types, *type);
    signature.flags = XR_XIR_CALLABLE_NO_SUSPEND;
    return source_intern_type(ctx, signature, type);
}
static bool source_cell_type(SourceContext *ctx, XrXirType element, XrXirType *type) {
    return source_intern_type(ctx, (XrXirTypeNode) {XR_XIR_TYPE_CELL, element,
        NULL, 0, XR_XIR_UNIT, 0, 0, {0}}, type);
}
static bool source_nullable_type(SourceContext *ctx, XrXirType element, XrXirType *type) {
    if (!element || xr_xir_type_is_cell(&ctx->types,element) || xr_xir_type_is_nullable(&ctx->types,element))
        return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"nullable element must be an ordinary nonnullable value type");
    return source_intern_type(ctx,(XrXirTypeNode){XR_XIR_TYPE_NULLABLE,element,NULL,0,XR_XIR_UNIT,0,0,{0}},type);
}
static XrGenericParam **source_type_parameters(SourceContext *ctx, int *count) {
    if (ctx->type_scope.active) {
        *count = (int)ctx->type_scope.count;
        return ctx->type_scope.parameters;
    }
    SourceFunction *body = &ctx->bodies[ctx->function];
    *count = (int)body->type_parameter_count;
    return body->type_parameters;
}
#include "xxir_source_native.inc.c"
typedef struct SourceSubstitution { const XrXirType *types; uint32_t count; } SourceSubstitution;
static bool source_substitute(SourceContext *ctx, const SourceSubstitution *sub,
    XrXirType type, uint32_t depth, XrXirType *output) {
    if (!source_work(ctx, NULL)) return false;
    if (depth == 128) return source_fail(ctx, NULL, XR_XIR_BUDGET, "constructed type substitution depth exhausted");
    if ((uint32_t) type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t) type < XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t index = (uint32_t) type - XR_XIR_TYPE_PARAMETER_BASE;
        if (index >= sub->count) return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "type parameter escapes declaration");
        *output = sub->types[index]; return true;
    }
    const XrXirTypeNode *found = xr_xir_type_node(&ctx->types, type);
    if (!found || !found->parameter_span) { *output = type; return true; }
    XrXirTypeNode node = *found;
    if (node.kind == XR_XIR_TYPE_NOMINAL) {
        XrXirType *arguments = source_alloc(ctx, node.nominal.argument_count, sizeof(*arguments));
        if (!arguments) return false;
        for (uint32_t a = 0; a < node.nominal.argument_count; ++a)
            if (!source_substitute(ctx, sub, node.nominal.arguments[a], depth + 1, &arguments[a])) return false;
        node.nominal.arguments = arguments;
        /* Derived fields belong to the substituted identity, never its template. */
        node.nominal.fields = NULL; node.nominal.field_count = 0;
        return source_intern_type(ctx, node, output);
    }
    if (node.kind != XR_XIR_TYPE_CALLABLE) {
        if (!source_substitute(ctx, sub, node.element, depth + 1, &node.element)) return false;
        return source_intern_type(ctx, node, output);
    }
    XrXirCallableParameter *parameters = node.parameter_count ? source_alloc(ctx, node.parameter_count, sizeof(*parameters)) : NULL;
    if (node.parameter_count && !parameters) return false;
    for (uint32_t p = 0; p < node.parameter_count; ++p) {
        parameters[p].mode = node.parameters[p].mode;
        if (!source_substitute(ctx, sub, node.parameters[p].type, depth + 1, &parameters[p].type)) return false;
    }
    node.parameters = parameters;
    if (!source_substitute(ctx, sub, node.result, depth + 1, &node.result)) return false;
    return source_intern_type(ctx, node, output);
}
static bool source_callable_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    if (!ref->nchildren || !ref->children || ref->requires_nothrow || ref->borrow_origin_syntax || ref->borrow_origin_count)
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "callable promises require an implemented contract");
    if (ctx->depth >= 128) return source_fail(ctx, NULL, XR_XIR_BUDGET, "source type depth exhausted");
    ++ctx->depth;
    uint32_t count = ref->nchildren - 1;
    XrXirCallableParameter *parameters = count ? source_alloc(ctx, count, sizeof(*parameters)) : NULL;
    if (count && !parameters) return false;
    for (uint32_t p = 0; p < count; ++p) {
        if (ref->function_param_modes && ref->function_param_modes[p] != XR_PARAM_READ)
            return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "callable mode requires an implemented contract");
        if (!source_work(ctx, NULL) || !source_type(ctx, ref->children[p], &parameters[p].type)) return false;
    }
    XrXirType result;
    if (!source_type(ctx, ref->children[count], &result)) return false;
    --ctx->depth; return source_signature(ctx, parameters, count, result, type);
}
static bool source_type_ref(SourceContext *ctx, XrTypeRef *ref, XrXirType *type);
/* A type failure raised without an owning AST node reports the annotation that caused it. */
static bool source_type(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    bool valid = source_type_ref(ctx, ref, type);
    if (!valid && ctx->diagnostic.status != XR_XIR_OK && !ctx->diagnostic.line) {
        /* Shared primitive references carry no position; the owning declaration is the nearest one. */
        const AstNode *owner = ctx->type_scope.active ? ctx->type_scope.node : ctx->bodies[ctx->function].node;
        int line = ref && ref->line ? ref->line : owner ? owner->line : 0;
        int column = ref && ref->line ? ref->column : owner ? owner->column : 0;
        if (line) {
            ctx->diagnostic.module = ctx->module;
            ctx->diagnostic.line = line;
            ctx->diagnostic.column = column;
        }
    }
    return valid;
}
static bool source_type_ref(SourceContext *ctx, XrTypeRef *ref, XrXirType *type) {
    if (!ref) { *type = XR_XIR_UNIT; return true; }
    switch (ref->kind) {
    case XR_TREF_OPTIONAL: {
        if (ref->nchildren != 1 || !ref->children || ctx->depth >= 128)
            return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"nullable type requires one bounded element");
        XrXirType element; ++ctx->depth;
        bool valid = source_type(ctx,ref->children[0],&element); --ctx->depth;
        return valid && source_nullable_type(ctx,element,type);
    }
    case XR_TREF_FUNCTION: return source_callable_type(ctx, ref, type);
    case XR_TREF_UNIT: *type = XR_XIR_UNIT; return true;
    case XR_TREF_BOOL: *type = XR_XIR_BOOL; return true;
    case XR_TREF_STRING: *type = XR_XIR_STRING; return true;
    case XR_TREF_NAMED: case XR_TREF_TYPE_PARAM: {
        AstNode *node = ctx->type_scope.active ? ctx->type_scope.node : ctx->bodies[ctx->function].type_owner;
        if (!ref->name) break;
        int count;
        XrGenericParam **parameters = source_type_parameters(ctx, &count);
        for (int i = 0; i < count; ++i) {
            if (!source_work(ctx, node)) return false;
            if (source_text_same(ctx, NULL, ref->name, parameters[i]->name)) {
                *type = (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + (uint32_t) i); return true;
            }
        }
        if (source_text_same(ctx, NULL, ref->name, "Error") && !source_nominal_name(ctx, ref->name)) {
            *type = XR_XIR_ERROR; return true;
        }
        if (source_text_same(ctx, NULL, ref->name, "PanicInfo") && !source_nominal_name(ctx, ref->name)) {
            *type = XR_XIR_PANIC_INFO; return true;
        }
        return source_nominal_type(ctx, ref->name, type);
    }
    case XR_TREF_SCALAR:
        switch (ref->scalar_rep) {
        case XR_NATIVE_I8: *type = XR_XIR_I8; return true;
        case XR_NATIVE_I16: *type = XR_XIR_I16; return true;
        case XR_NATIVE_I32: *type = XR_XIR_I32; return true;
        case XR_NATIVE_I64: *type = XR_XIR_I64; return true;
        case XR_NATIVE_U8: *type = XR_XIR_U8; return true;
        case XR_NATIVE_U16: *type = XR_XIR_U16; return true;
        case XR_NATIVE_U32: *type = XR_XIR_U32; return true;
        case XR_NATIVE_F32: *type = XR_XIR_F32; return true;
        case XR_NATIVE_F64: *type = XR_XIR_F64; return true;
        case XR_NATIVE_U64: *type = XR_XIR_U64; return true;
        default: break;
        }
        break;
    case XR_TREF_GENERIC:
        if (source_nominal_name(ctx, ref->name))
            return source_nominal_arguments(ctx, ref->name, ref->children, ref->nchildren, type);
        if (ref->name && source_native_find(ctx,ref->name))
            return source_native_array_type(ctx, ref, type);
        if (ref->name && source_text_same(ctx, NULL, ref->name, "Atomic") && ref->nchildren == 1 &&
            ref->children[0]->kind == XR_TREF_SCALAR && ref->children[0]->scalar_rep == XR_NATIVE_I64) {
            *type = XR_XIR_ATOMIC_I64; return true;
        }
        return source_nominal_arguments(ctx, ref->name, ref->children, ref->nchildren, type);
    default: break;
    }
    char message[96];
    if (source_format(ctx, message, sizeof(message), "type declaration is not admitted by XIR (kind %d)",
            (int) ref->kind) != XR_DIAG_OK)
        return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, "type declaration is not admitted by XIR");
    return source_fail(ctx, NULL, XR_XIR_BAD_TYPE, message);
}
#include "xxir_source_region_emit.inc.c"
static bool begin_block(SourceContext *ctx) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (body->region_sealed) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source region is sealed");
    if (!ctx->remaining_blocks) return source_fail(ctx, body->node, XR_XIR_BUDGET, "block budget exhausted");
    if (body->block_count == body->block_capacity) {
        uint32_t capacity = body->block_capacity ? body->block_capacity * 2 : 4;
        if (capacity < body->block_capacity) return source_fail(ctx, NULL, XR_XIR_BUDGET, "block capacity overflow");
        SourceBlockRecipe *blocks = source_recipe_storage(ctx, capacity, sizeof(*blocks));
        if (!blocks) return false;
        if (!source_copy_bytes(ctx, NULL, blocks, body->blocks, body->block_count * sizeof(*blocks))) return false;
        body->blocks = blocks; body->block_capacity = capacity;
    }
    body->blocks[body->block_count] = (SourceBlockRecipe){{body->count,0,0,body->frontier},ctx->function,body->block_count,body->block_count};
    body->current_block=(SourceBlockReference){ctx->function,body->block_count};
    ++body->block_count;
    --ctx->remaining_blocks; ctx->returned = false; return true;
}
static bool source_recipe_append(SourceContext *ctx, XrXirInstruction op, SourceValue *result) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (body->count>UINT32_MAX-ctx->functions[ctx->function].parameter_count)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"source region value identity exhausted");
    if (body->region_sealed) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source region is sealed");
    if (!body->block_count) {
        bool terminated = ctx->returned;
        if (!begin_block(ctx)) return false;
        ctx->returned = terminated;
    }
    if (body->current_block.owner!=ctx->function || body->current_block.identity>=body->block_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source block reference has a foreign owner");
    if (!ctx->remaining_instructions) return source_fail(ctx, body->node, XR_XIR_BUDGET, "instruction budget exhausted");
    if (body->count == body->capacity) {
        uint32_t capacity = body->capacity ? body->capacity * 2 : 16;
        if (capacity < body->capacity) return source_fail(ctx, NULL, XR_XIR_BUDGET, "instruction capacity overflow");
        SourceInstructionRecipe *ops = source_recipe_storage(ctx, capacity, sizeof(*ops));
        if (!ops) return false;
        if (!source_copy_bytes(ctx, NULL, ops, body->recipes, body->count * sizeof(*ops))) return false;
        body->recipes = ops; body->capacity = capacity;
    }
    if (result) *result = (SourceValue) {ctx->functions[ctx->function].parameter_count + body->count, op.type};
    body->recipes[body->count] = (SourceInstructionRecipe){op,ctx->function,ctx->functions[ctx->function].parameter_count+body->count};
    ++body->count; ++body->blocks[body->current_block.identity].block.count; --ctx->remaining_instructions;
    return true;
}
#include "xxir_source_invoke.inc.c"
static bool expression(SourceContext *ctx, AstNode *node, SourceValue *value);
static bool source_plan_expression(SourceContext *ctx, AstNode *node, SourceExpectedType context, SourceValue *value);
static SourceExpressionPlan *source_plan_collect(SourceContext *ctx, AstNode *node, SourceExpectedType context);
static bool source_plan_complete(SourceContext *ctx, SourceExpressionPlan *plan, SourceValue *value);
static bool source_plan_discard(SourceContext *ctx,uint32_t first);
static bool source_plan_binary_prepare(SourceContext *ctx,SourceExpressionPlan *plan,SourceExpectedType hint,bool default_literals);
static bool source_plan_numeric_prepare(SourceContext *ctx,SourceExpressionPlan *plan,SourceExpectedType hint,bool defaults);
static bool source_plan_soft_numeric(SourceContext *ctx,SourceExpressionPlan *plan,bool *soft);
#include "xxir_source_conversion.inc.c"

static bool source_direct_integer(SourceContext *ctx, AstNode *node, SourceInteger *literal) {
    *literal = (SourceInteger) {0};
    bool negative = false;
    for (uint32_t depth = ctx->depth; node; ++depth) {
        if (!source_work(ctx, node)) return false;
        if (depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "integer literal depth exhausted");
        if (node->type == AST_GROUPING) { node = node->as.grouping; continue; }
        if (node->type == AST_UNARY_NEG && !negative) { negative = true; node = node->as.unary.operand; continue; }
        if (node->type != AST_LITERAL_INT) return true;
        uint64_t magnitude = node->as.literal.int_bits;
        if (!node->as.literal.int_overflows_i64 && magnitude > INT64_MAX) {
            magnitude = UINT64_C(0) - magnitude; negative = !negative;
        }
        *literal = (SourceInteger) {true, negative, magnitude}; return true;
    }
    return true;
}
static bool source_direct_decimal(SourceContext *ctx, AstNode *node, SourceDecimal *literal) {
    *literal = (SourceDecimal) {0}; bool negative = false;
    for (unsigned depth = 0; node; ++depth) {
        if (!source_work(ctx, node)) return false;
        if (depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "decimal literal depth exhausted");
        if (node->type == AST_GROUPING) { node = node->as.grouping; continue; }
        if (node->type == AST_UNARY_NEG && !negative) { negative = true; node = node->as.unary.operand; continue; }
        if (node->type == AST_LITERAL_FLOAT) *literal = (SourceDecimal) {node, negative};
        return true;
    }
    return true;
}
static bool source_decimal_payload(SourceContext *ctx, const SourceDecimal *literal, XrXirType type, uint64_t *bits) {
    AstNode *node = literal->node;
    size_t length = node->as.literal.decimal_length;
    if (!node->as.literal.decimal_text || !length) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "decimal spelling is missing");
    XrDecimalWork work = {ctx,source_algorithm_work,false};
    XrDecimalStatus status = xr_decimal_float_parse_work(&work,node->as.literal.decimal_text,
        length,xr_xir_float_bits(type),bits);
    if (status != XR_DECIMAL_OK) return source_fail(ctx,node,
        status == XR_DECIMAL_WORK_LIMIT ? XR_XIR_BUDGET : XR_XIR_BAD_TYPE,"invalid decimal literal");
    if (literal->negative) *bits ^= UINT64_C(1) << (xr_xir_float_bits(type) - 1);
    return true;
}
static bool source_integer_float_payload(SourceContext *ctx, AstNode *node, const SourceInteger *literal,
    XrXirType type, uint64_t *bits) {
    int64_t magnitude;
    if (!source_copy_bytes(ctx, node, &magnitude, &literal->magnitude, sizeof(magnitude))) return false;
    *bits = 0; int64_t exact = 0;
    XrXirIntegerFormat format = {64, false};
    if (xr_xir_integer_to_float(format, xr_xir_float_bits(type), magnitude, bits) != XR_XIR_NUMERIC_OK ||
        xr_xir_float_to_integer(xr_xir_float_bits(type), format, *bits, &exact) != XR_XIR_NUMERIC_OK ||
        (uint64_t) exact != literal->magnitude)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "integer literal is not exactly representable in its floating context");
    if (literal->negative) *bits ^= UINT64_C(1) << (xr_xir_float_bits(type) - 1);
    return true;
}
static bool source_integer_payload(SourceContext *ctx, AstNode *node, const SourceInteger *literal,
    XrXirType type, uint64_t *bits) {
    uint32_t width = xr_xir_integer_bits(type);
    bool sign = xr_xir_integer_signed(type);
    uint64_t limit = sign ? (UINT64_C(1) << (width - 1)) - (literal->negative ? 0 : 1) :
        (width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1);
    if (literal->magnitude > limit || (!sign && literal->negative && literal->magnitude))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "integer literal is outside its contextual type");
    *bits = literal->negative ? UINT64_C(0) - literal->magnitude : literal->magnitude;
    return true;
}
#include "xxir_source_numeric_plan.inc.c"
static bool source_recipe_group(SourceContext *ctx, XrXirInstruction op, const SourceValue *args,
                       uint32_t count, SourceValue *value) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (count > UINT32_MAX - body->operand_count)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "operand capacity overflow");
    uint32_t needed = body->operand_count + count;
    if (needed > body->operand_capacity) {
        uint32_t capacity = needed <= UINT32_MAX / 2 ? needed * 2 : needed;
        uint32_t *operands = source_alloc(ctx, capacity, sizeof(*operands));
        if (!operands) return false;
        if (!source_copy_bytes(ctx, NULL, operands, body->operands, body->operand_count * sizeof(*operands))) return false;
        body->operands = operands; body->operand_capacity = capacity;
    }
    op.args[0] = count ? body->operand_count : 0; op.args[1] = count;
    for (uint32_t i = 0; i < count; ++i) body->operands[body->operand_count++] = args[i].id;
    return source_recipe_record(ctx, op, value);
}
static bool statement(SourceContext *ctx, AstNode *node, bool top);
static SourceName *visible_name(SourceContext *ctx, const char *name) {
    SourceName *symbol = find_name(ctx, ctx->locals, name);
    return symbol ? symbol : find_name(ctx, ctx->names[ctx->module], name);
}
static SourceName *imported_declaration(SourceContext *ctx, SourceName *symbol, const char *name) {
    SourceName *target = find_name(ctx, ctx->names[symbol->module], name);
    if (!target || (target->kind != SOURCE_FUNCTION && target->kind != SOURCE_NOMINAL &&
        target->kind != SOURCE_INTERFACE) || !(target->kind == SOURCE_FUNCTION ? ctx->identities[target->index].exported : (target->node && target->node->is_exported))) {
        source_fail(ctx, symbol->node, XR_XIR_BAD_STRUCTURE, "import requires an exported declaration"); return NULL;
    }
    if (symbol->kind == SOURCE_IMPORT && symbol->declaration) {
        XrXirSourceDeclaration *declarations = (XrXirSourceDeclaration *) ctx->query.declarations;
        declarations[symbol->declaration - 1].target = target->declaration;
    }
    return target;
}
#include "xxir_source_constraints.inc.c"
#include "xxir_source_interfaces.inc.c"
static uint32_t stream_primitive(SourceContext *ctx, const char *name) {
    const XrModuleSpec *spec = &ctx->graph->specs[ctx->module];
    if (spec->authority.kind != XR_MODULE_IDENTITY_STDLIB || !spec->authority.namespace_id ||
        !source_text_same(ctx, NULL, spec->authority.namespace_id, "io") || !spec->logical_path ||
        !source_text_same(ctx, NULL, spec->logical_path, "io/output.xr")) return 0;
    if (source_text_same(ctx, NULL, name, "__writeStdout")) return 1;
    if (source_text_same(ctx, NULL, name, "__writeStderr")) return 2;
    return 0;
}
/* The five host time leaves are private to the standard time module, selected by
 * its exact stdlib authority and logical path, never by a user spelling. Codes 1-3
 * are the clock kinds; 4 is the UTC offset query; 5 is the suspending timer. */
static uint32_t host_time_primitive(SourceContext *ctx, const char *name) {
    const XrModuleSpec *spec = &ctx->graph->specs[ctx->module];
    if (spec->authority.kind != XR_MODULE_IDENTITY_STDLIB || !spec->authority.namespace_id ||
        !source_text_same(ctx, NULL, spec->authority.namespace_id, "time") || !spec->logical_path ||
        !source_text_same(ctx, NULL, spec->logical_path, "time/time.xr")) return 0;
    if (source_text_same(ctx, NULL, name, "__realtimeNanos")) return XR_XIR_CLOCK_REALTIME;
    if (source_text_same(ctx, NULL, name, "__cpuNanos")) return XR_XIR_CLOCK_CPU;
    if (source_text_same(ctx, NULL, name, "__monotonicNanos")) return XR_XIR_CLOCK_MONOTONIC;
    if (source_text_same(ctx, NULL, name, "__utcOffsetAt")) return 4;
    if (source_text_same(ctx, NULL, name, "__sleep")) return 5;
    return 0;
}
typedef struct SourceTypeArguments {
    XrTypeRef **refs;
    uint32_t count;
    SourceSubstitution substitution;
} SourceTypeArguments;
static bool source_type_arguments(SourceContext *ctx, AstNode *node, const XrXirType *types,
    uint32_t count, XrXirInstruction *op) {
    XrXirGeneric *caller = &ctx->generics[ctx->function];
    if (count > UINT32_MAX - caller->argument_count)
        return source_fail(ctx, node, XR_XIR_BUDGET, "type argument table overflow");
    uint32_t needed = caller->argument_count + count;
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (needed > body->type_capacity) {
        uint32_t capacity = needed <= UINT32_MAX / 2 ? needed * 2 : needed;
        XrXirType *table = source_alloc(ctx, capacity, sizeof(*table));
        if (!table) return false;
        if (!source_copy_bytes(ctx, node, table, caller->arguments, caller->argument_count * sizeof(*table))) return false;
        caller->arguments = table; body->type_capacity = capacity;
    }
    op->type_arguments[0] = count ? caller->argument_count : 0; op->type_arguments[1] = count;
    if (count && !source_copy_bytes(ctx, node, (XrXirType *) caller->arguments + caller->argument_count, types, count * sizeof(*types))) return false;
    caller->argument_count = needed; return true;
}
#include "xxir_source_flow.inc.c"
static bool source_instantiation_prove(SourceContext *ctx, AstNode *node,
    XrXirDeclarationContext callee, SourceSubstitution substitution) {
    XrXirDeclarations declarations;
    XrXirModule view = source_module_view(ctx,&declarations);
    XrXirProofContext context = {&view,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
    for (uint32_t i = 0; i < substitution.count; ++i) {
        if (!source_work(ctx, node)) return false;
        XrXirConstraintUse use = {&view,callee,i,substitution.types,substitution.count};
        bool result = callee.kind == XR_XIR_CONTEXT_FUNCTION &&
            xr_xir_binder_kind(&ctx->generics[callee.declaration],i) == XR_XIR_BINDER_RESULT_VARIABLE;
        XrXirStatus status = result ? xr_xir_compile_result_argument(&ctx->compile, &view, ctx->function, substitution.types[i]) : XR_XIR_OK;
        if (status == XR_XIR_OK) status = xr_xir_compile_constraints_prove(&ctx->compile, &context, &use);
        if (status == XR_XIR_OK)
            status = xr_xir_compile_type_access(&ctx->compile, &view, ctx->function, substitution.types[i]);
        if (status != XR_XIR_OK)
            return source_fail(ctx,node,status,"type argument does not prove the declared constraint");
    }
    return true;
}
static bool source_instantiation(SourceContext *ctx, AstNode *node, XrXirDeclarationContext callee,
    SourceTypeArguments *arguments) {
    uint32_t expected = callee.kind == XR_XIR_CONTEXT_FUNCTION ? ctx->generics[callee.declaration].parameter_count :
        ctx->nominals.declarations[callee.declaration].parameter_count;
    uint32_t count = arguments->count;
    if (count > 65536 || count != expected || (count && !arguments->refs))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"declaration requires its exact explicit type arguments");
    XrXirType *types = count ? source_alloc(ctx,count,sizeof(*types)) : NULL;
    if (count && !types) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!source_work(ctx,node) || !source_type(ctx,arguments->refs[i],&types[i])) return false;
    SourceSubstitution substitution = {types,count};
    if (!source_instantiation_prove(ctx,node,callee,substitution)) return false;
    arguments->substitution = substitution; return true;
}
#include "xxir_source_arguments.inc.c"
#include "xxir_source_inference_result.inc.c"
#include "xxir_source_call_plan.inc.c"
#include "xxir_source_direct_arguments.inc.c"
static bool source_value_place(SourceContext *ctx, AstNode *node, SourceValue *place);
typedef enum SourceArrayRecipe {
    SOURCE_ARRAY_NONE, SOURCE_ARRAY_MAP, SOURCE_ARRAY_FILTER, SOURCE_ARRAY_REDUCE, SOURCE_ARRAY_FOR_EACH,
    SOURCE_ARRAY_FIND, SOURCE_ARRAY_FIND_INDEX, SOURCE_ARRAY_EVERY, SOURCE_ARRAY_SOME,
    SOURCE_ARRAY_CONTAINS, SOURCE_ARRAY_INDEX_OF, SOURCE_ARRAY_JOIN, SOURCE_ARRAY_CLEAR,
    SOURCE_ARRAY_REVERSE, SOURCE_ARRAY_UNSHIFT
} SourceArrayRecipe;
static SourceArrayRecipe source_array_recipe(XrNativeOperation operation);
static bool source_array_recipe_call(SourceContext *ctx, AstNode *node, SourceArrayRecipe recipe,
    const SourceValue *evaluated, SourceValue *value);
#include "xxir_source_array.inc.c"
#include "xxir_source_string.inc.c"
static bool source_constructor_receiver(SourceContext *ctx, AstNode *node);
static bool source_constructor_field(SourceContext *ctx, AstNode *node, const char *name, SourceValue *value, AstNode *incoming);
#include "xxir_source_struct.inc.c"
#include "xxir_source_path.inc.c"
static bool source_literal_append(SourceContext *ctx, AstNode *node, const char *bytes,
    size_t length, uint32_t *literal) {
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    if (!literal || (length && !bytes))
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "string literal bytes are missing");
    if (length > UINT32_MAX || ctx->literal_count == UINT32_MAX)
        return source_fail(ctx, node, XR_XIR_BUDGET, "string literal exceeds format limit");
    bool grow = ctx->literal_count == ctx->literal_capacity;
    uint32_t capacity = ctx->literal_capacity;
    if (grow) {
        if (capacity > UINT32_MAX / 2)
            return source_fail(ctx, node, XR_XIR_BUDGET, "literal capacity overflow");
        capacity = capacity ? capacity * 2 : 16;
    }
    if (!source_work(ctx, node)) return false;
    if (grow) {

        XrXirLiteral *literals = source_alloc(ctx, capacity, sizeof(*literals));
        if (!literals) return false;
        if (!source_work_units(ctx, node, (uint64_t)ctx->literal_count * sizeof(*literals))) return false;
        if (!source_copy_bytes(ctx, NULL, literals, ctx->literals, ctx->literal_count * sizeof(*literals))) return false;
        ctx->literals = literals; ctx->literal_capacity = capacity;
    }
    uint32_t id = ctx->literal_count;
    ctx->literals[id] = (XrXirLiteral){bytes, (uint32_t)length};
    ++ctx->literal_count; *literal = id;
    return true;
}
static bool source_string_literal(SourceContext *ctx, AstNode *node, const char *bytes,
    size_t length, SourceValue *value) {
    uint32_t id;
    return source_literal_append(ctx, node, bytes, length, &id) &&
        source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_STRING, XR_XIR_STRING, {0, 0}, {0, 0}, id, {0}}, value);
}
#include "xxir_source_enum.inc.c"
#include "xxir_source_enum_text.inc.c"
#include "xxir_source_type_name.inc.c"
#include "xxir_source_defaults.inc.c"
#include "xxir_source_constructors.inc.c"
#include "xxir_source_panic.inc.c"
static bool finish_body(SourceContext *ctx);
#include "xxir_source_requirement_calls.inc.c"
#include "xxir_source_requirement_values.inc.c"
#include "xxir_source_methods.inc.c"
static SourceName *source_core_assertion(SourceContext *ctx, AstNode *site, uint32_t intrinsic);
static bool source_text_conversion(SourceContext *ctx, AstNode *node, SourceValue input, SourceValue *value);
static bool source_call(SourceContext *ctx, AstNode *node, SourceExpectedType result_context, SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    if (call->arg_count < 0 || call->arg_count > 65536 || call->type_arg_count < 0 ||
        call->type_arg_count > 65536 || call->default_arg_count)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "call arity or type arguments are not admitted");
    SourceName *target = NULL, *binding = NULL;
    bool print = false, atomic = false;
    uint32_t stream = 0, host_time = 0;
    AstNode *callee = call->callee;
    SourceValue receiver = {0}, indirect = {0};
    bool indirect_ready = false;
    SourceStaticMethod selected = {0};
    XrXirOp method = XR_XIR_INVALID;
    if (callee->type == AST_VARIABLE) {
        const char *name = callee->as.variable.name;
        target = ctx->active_expression->binding;
        binding = target;
        if (!target) {
            const XrCoreIntrinsicDesc *intrinsic = NULL;
            size_t name_length = source_text_size(ctx, name);
            if (ctx->diagnostic.status != XR_XIR_OK) return false;
            XrCoreIntrinsicQueryStatus query = xr_core_intrinsic_by_source_name_work(
                ctx, source_algorithm_work, name, name_length, &intrinsic);
            if (query != XR_CORE_INTRINSIC_QUERY_OK)
                return source_fail(ctx, node, query == XR_CORE_INTRINSIC_QUERY_WORK_LIMIT ?
                    XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE, "core intrinsic query failed");
            if (intrinsic && (intrinsic->id == XR_CORE_BUILTIN_ASSERT || intrinsic->id == XR_CORE_BUILTIN_ASSERT_PANICS || intrinsic->id == XR_CORE_BUILTIN_ASSERT_EQUAL)) {
                target = binding = source_core_assertion(ctx, node, intrinsic->id);
                if (!target) return false;
            }
            if (intrinsic && intrinsic->id == XR_CORE_BUILTIN_LEN)
                return source_length(ctx, node, intrinsic, value);
            if (source_text_same(ctx, NULL, name, "PanicInfo"))
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "PanicInfo construction requires class support");
            if (source_text_same(ctx, NULL, name, "string")) {
                SourceValue input;
                if (call->arg_count != 1 || call->type_arg_count || !call->arguments ||
                    (call->arg_accesses && call->arg_accesses[0] != XR_CALL_ARG_PLAIN))
                    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "string conversion takes one value");
                return source_plan_expression(ctx, call->arguments[0], (SourceExpectedType){false, XR_XIR_UNIT, false}, &input) &&
                    source_text_conversion(ctx, node, input, value);
            }
            if (source_text_same(ctx, NULL, name, "typeName")) {
                SourceValue input;
                if (call->arg_count != 1 || call->type_arg_count || !call->arguments ||
                    (call->arg_accesses && call->arg_accesses[0] != XR_CALL_ARG_PLAIN))
                    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "typeName takes one value");
                AstNode *argument = call->arguments[0];
                while (argument && argument->type == AST_GROUPING) argument = argument->as.grouping;
                if (argument && argument->type == AST_LITERAL_NULL)
                    return source_string_literal(ctx, node, TYPE_NAME_NULL, strlen(TYPE_NAME_NULL), value);
                return source_plan_expression(ctx, call->arguments[0], (SourceExpectedType){false, XR_XIR_UNIT, false}, &input) &&
                    source_type_name(ctx, node, input, value);
            }
            print = source_text_same(ctx, NULL, name, "print"); atomic = source_text_same(ctx, NULL, name, "Atomic"); stream = stream_primitive(ctx, name);
            host_time = host_time_primitive(ctx, name);
        }
        if (target && target->kind == SOURCE_IMPORT) target = imported_declaration(ctx, target, target->imported);
    } else if (callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *member = &callee->as.member_access;
        SourceName *base = member->object->type == AST_VARIABLE ?
            visible_name(ctx, member->object->as.variable.name) : NULL;
        if (!base && member->object->type == AST_VARIABLE && source_text_same(ctx, NULL, member->object->as.variable.name, "Coro") &&
            source_text_same(ctx, NULL, member->name, "yield")) {
            if (call->arg_count || call->type_arg_count)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Coro.yield accepts no value or type arguments");
            return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_SUSPEND, XR_XIR_UNIT, {0}, {0}, 0, {0}}, value);
        }
        if (!source_static_select(ctx, callee, &selected)) return false;
        if (selected.method) {
            binding = target = selected.method;
        } else if (base && base->kind == SOURCE_MODULE) { binding = base; target = imported_declaration(ctx, base, member->name); }
        else if (source_constructor_receiver(ctx, member->object) &&
            !source_method_find(ctx, ctx->functions[ctx->function].result, member->name)) {
            if (!expression(ctx, callee, &indirect)) return false;
            indirect_ready = true;
        } else {
            if (base && xr_xir_type_is_array(&ctx->types, base->type))
                return source_array_call(ctx, node, NULL, value);
            XrXirType path_type;
            if (!source_path_type(ctx,member->object,&path_type)) return false;
            if (xr_xir_type_is_array(&ctx->types,path_type)) {
                const XrNativeMemberDeclaration *native_member = source_array_member(ctx, callee, member->name);
                if (!native_member) return false;
                /* A REF member binds the logical path without evaluating its selectors twice. */
                if (native_member->receiver == XR_NATIVE_RECEIVER_REF)
                    return source_array_call(ctx,node,NULL,value);
            }
            if (xr_xir_type_is_nominal(&ctx->types, path_type)) {
                SourceName *ref_method = source_method_find(ctx, path_type, member->name);
                if (ref_method && !ref_method->node->as.method_decl.is_static &&
                    ref_method->node->as.method_decl.receiver_mode == XR_PARAM_REF)
                    return source_ref_method_call(ctx, node, member->object, ref_method, result_context, value);
            }
            if (!expression(ctx, member->object, &receiver)) return false;
            if (xr_xir_type_is_array(&ctx->types, receiver.type))
                return source_array_call(ctx, node, &receiver, value);
            if ((receiver.type == XR_XIR_BOOL || xr_xir_type_is_number(receiver.type)) &&
                source_text_same(ctx, NULL, member->name, "toString")) {
                if (call->arg_count || call->type_arg_count)
                    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "toString accepts no value or type arguments");
                return source_text_conversion(ctx, node, receiver, value);
            }
            if (receiver.type == XR_XIR_STRING) return source_string_call(ctx, node, receiver, value);
            if (xr_xir_type_is_enum(&ctx->types, receiver.type) && source_text_same(ctx, NULL, member->name, "toString")) {
                if (call->arg_count || call->type_arg_count)
                    return source_fail(ctx, node, XR_XIR_BAD_TYPE, "enum toString accepts no value or type arguments");
                return source_enum_text(ctx, callee, receiver, true, value);
            }
            if (receiver.type == XR_XIR_PANIC_INFO)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "PanicInfo methods require class support");
            if ((uint32_t)receiver.type >= XR_XIR_TYPE_PARAMETER_BASE &&
                (uint32_t)receiver.type < XR_XIR_TYPE_PARAMETER_LIMIT)
                return source_requirement_call(ctx,node,receiver,result_context,value);
            if (xr_xir_type_is_nominal(&ctx->types, receiver.type)) {
                SourceName *member_method = source_method_find(ctx, receiver.type, member->name);
                if (member_method) return source_method_call(ctx, node, receiver, member_method, result_context, value);
                if (!source_struct_get_value(ctx, callee, receiver, &indirect) ||
                    !source_query_expression(ctx, callee, indirect.type)) return false;
                indirect_ready = true;
            }
            if (receiver.type == XR_XIR_ATOMIC_I64) {
                if (source_text_same(ctx, NULL, member->name, "load") && !call->arg_count) method = XR_XIR_ATOMIC_I64_LOAD;
                if (source_text_same(ctx, NULL, member->name, "fetchAdd") && call->arg_count == 1) method = XR_XIR_ATOMIC_I64_FETCH_ADD;
            }
        }
    }
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    if (target && target->kind == SOURCE_NOMINAL) {
        if (call->type_arg_count < 0)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "invalid constructor type arguments");
        XrXirType type;
        return source_nominal_apply(ctx, target, call->type_args, (uint32_t)call->type_arg_count, &type) &&
            source_constructor_call(ctx, node, type, binding, target, value);
    }
    if (target && target->kind == SOURCE_FUNCTION) {
        SourceDirectRequest direct = {target->index,selected.method ? selected.substitution : (SourceSubstitution){0},NULL,result_context};
        SourceDirectArguments prepared = {0};
        if (!source_direct_arguments(ctx,node,&direct,&prepared)) return false;
        XrXirInstruction call_op = {XR_XIR_CALL,prepared.result,{0},{0},target->index,{0}};
        return source_type_arguments(ctx,node,prepared.substitution.types,prepared.substitution.count,&call_op) &&
            source_query_reference(ctx,callee,binding,target,XR_XIR_SOURCE_CALL) &&
            source_recipe_group(ctx,call_op,prepared.values,prepared.count,value);
    }
    XrXirTypeNode signature = {0};
    if (!print && !atomic && !stream && !host_time && method == XR_XIR_INVALID && (!target || target->kind != SOURCE_FUNCTION)) {
        if (call->type_arg_count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "indirect call has no generic declaration parameters");
        if (!indirect_ready && !expression(ctx, callee, &indirect)) return false;
        const XrXirTypeNode *found = xr_xir_callable_signature(&ctx->types, indirect.type);
        if (!found || found->parameter_count != (uint32_t) call->arg_count)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "indirect call requires its declared signature");
        signature = *found;
    }
    if ((print || atomic || stream || host_time || method != XR_XIR_INVALID) && call->type_arg_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "primitive does not admit explicit type arguments");
    XrXirInstruction op = {0};
    uint32_t argument_count = (uint32_t)call->arg_count;
    SourceValue *args = argument_count ? source_alloc(ctx, argument_count, sizeof(*args)) : NULL;
    if (argument_count && !args) return false;
    for (int i = 0; i < call->arg_count; ++i) {
        if (call->arg_accesses && call->arg_accesses[i] != XR_CALL_ARG_PLAIN)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "ref and move arguments require an implemented contract");
        XrXirType expected = XR_XIR_UNIT;
        if (indirect.type) expected = signature.parameters[i].type;
        if (atomic || method == XR_XIR_ATOMIC_I64_FETCH_ADD) expected = XR_XIR_I64;
        if (stream) expected = XR_XIR_STRING;
        if (host_time > (uint32_t) XR_XIR_CLOCK_MONOTONIC) expected = XR_XIR_I64;
        if (!source_plan_expression(ctx, call->arguments[i], (SourceExpectedType){expected != XR_XIR_UNIT,expected, false}, &args[i])) return false;
        if (args[i].type == XR_XIR_UNIT) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "unit argument is not admitted");
    }
    if (indirect.type) {
        for (uint32_t p = 0; p < (uint32_t) call->arg_count; ++p)
            if (args[p].type != signature.parameters[p].type)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "indirect argument type mismatch");
        return source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_CALL_INDIRECT, signature.result, {0}, {0}, indirect.id, {0}},
            args, (uint32_t) call->arg_count, value);
    }
    if (stream) {
        if (call->arg_count != 1 || args[0].type != XR_XIR_STRING)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "stream write requires one string");
        op = (XrXirInstruction) {XR_XIR_WRITE_STREAM, XR_XIR_BOOL, {args[0].id, 0}, {0, 0}, stream, {0}};
    } else if (host_time) {
        bool clock = host_time <= (uint32_t) XR_XIR_CLOCK_MONOTONIC;
        if (call->arg_count != (clock ? 0 : 1) || (!clock && args[0].type != XR_XIR_I64))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "host time primitive argument mismatch");
        op = clock ? (XrXirInstruction) {XR_XIR_CLOCK_NANOS, XR_XIR_I64, {0, 0}, {0, 0}, host_time, {0}} :
            host_time == 4 ? (XrXirInstruction) {XR_XIR_UTC_OFFSET_AT, XR_XIR_I64, {args[0].id, 0}, {0, 0}, 0, {0}} :
            (XrXirInstruction) {XR_XIR_TIMER_AFTER_MS, XR_XIR_UNIT, {args[0].id, 0}, {0, 0}, 0, {0}};
    } else if (print) {
        for (int i = 0; i < call->arg_count; ++i)
            if (args[i].type != XR_XIR_BOOL && !xr_xir_type_is_number(args[i].type) && args[i].type != XR_XIR_STRING)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "print requires an admitted display type");
        op = (XrXirInstruction) {XR_XIR_PRINT, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0}};
    } else if (atomic) {
        if (call->arg_count != 1 || args[0].type != XR_XIR_I64)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Atomic requires one i64 initializer");
        op = (XrXirInstruction) {XR_XIR_ATOMIC_I64_NEW, XR_XIR_ATOMIC_I64, {args[0].id, 0}, {0, 0}, 0, {0}};
    } else if (method != XR_XIR_INVALID) {
        if (method == XR_XIR_ATOMIC_I64_FETCH_ADD && args[0].type != XR_XIR_I64)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Atomic fetchAdd requires i64");
        op = (XrXirInstruction) {method, XR_XIR_I64, {receiver.id, call->arg_count ? args[0].id : 0}, {0, 0}, 0, {0}};
    }
    if (op.op == XR_XIR_PRINT)
        return source_recipe_group(ctx, op, args, argument_count, value);
    return source_recipe_record(ctx, op, value);
}
static bool source_literal(SourceContext *ctx, AstNode *node, SourceValue *value) {
    if (node->type == AST_LITERAL_STRING)
        return source_string_literal(ctx, node, node->as.literal.raw_value.string_val,
            node->as.literal.string_length, value);
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0, 0}, {0, 0},
        node->as.literal.raw_value.bool_val, {0}}, value);
}
/* An interpolated template is the left-to-right concatenation of its parts; a bool or
 * number part is spelled exactly as `print` writes it. */
static bool source_template(SourceContext *ctx, AstNode *node, SourceValue *value) {
    TemplateStringNode *template = &node->as.template_str;
    if (template->part_count < 0 || (template->part_count && !template->parts))
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "template string parts are malformed");
    SourceValue result = {0};
    bool started = false;
    for (int i = 0; i < template->part_count; ++i) {
        AstNode *part = template->parts[i];
        SourceValue piece;
        if (!part || !source_work(ctx, node)) return false;
        if (part->type == AST_LITERAL_STRING) {
            if (!source_literal(ctx, part, &piece)) return false;
        } else {
            if (!expression(ctx, part, &piece)) return false;
            if (piece.type == XR_XIR_BOOL || xr_xir_type_is_number(piece.type)) {
                if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_TO_STRING, XR_XIR_STRING, {piece.id, 0}, {0}, 0, {0}}, &piece))
                    return false;
            } else if (piece.type != XR_XIR_STRING)
                return source_fail(ctx, part, XR_XIR_BAD_TYPE, "interpolated value has no admitted string conversion");
        }
        if (!started) { result = piece; started = true; continue; }
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONCAT_STRING, XR_XIR_STRING, {result.id, piece.id}, {0}, 0, {0}}, &result))
            return false;
    }
    if (!started) return source_string_literal(ctx, node, "", 0, value);
    *value = result;
    return true;
}
static bool source_logic(SourceContext *ctx, AstNode *node, SourceValue *value) {
    bool negate = node->type == AST_UNARY_NOT;
    SourceValue left, initial, place, right;
    if (!expression(ctx, negate ? node->as.unary.operand : node->as.binary.left, &left)) return false;
    if (left.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "logical operand must be bool");
    initial = left;
    if (negate && !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 0, {0}}, &initial)) return false;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_LOCAL_NEW, XR_XIR_BOOL, {initial.id, 0}, {0}, 0, {0}}, &place)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t branch = body->count, rhs = body->block_count;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {left.id, 0}, {0}, 0, {0}}, NULL) || !begin_block(ctx)) return false;
    /* The right operand runs only when the left was true for && and false for || (N-5). */
    SourceFact *entry_facts = ctx->facts;
    if (!negate) {
        SourceFactSet right_facts = {0};
        if (!source_condition_facts(ctx, node->as.binary.left, node->type == AST_BINARY_AND, &right_facts, 0) ||
            !source_facts_apply(ctx, &right_facts)) return false;
    }
    if (negate) {
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}}, &right)) return false;
    } else if (!expression(ctx, node->as.binary.right, &right)) return false;
    ctx->facts = entry_facts;
    if (right.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "logical operand must be bool");
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_LOCAL_WRITE, XR_XIR_UNIT, {place.id, right.id}, {0}, 0, {0}}, NULL)) return false;
    uint32_t join = body->block_count;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {join, 0}, 0, {0}}, NULL) || !begin_block(ctx)) return false;
    bool conjunction = node->type == AST_BINARY_AND;
    body->recipes[branch].instruction.targets[0] = conjunction ? rhs : join;
    body->recipes[branch].instruction.targets[1] = conjunction ? join : rhs;
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_LOCAL_READ, XR_XIR_BOOL, {place.id, 0}, {0}, 0, {0}}, value);
}
#include "xxir_source_nullable.inc.c"
/* The text of a bool or number, as `print` writes it; a string converts to itself. */
static bool source_text_conversion(SourceContext *ctx, AstNode *node, SourceValue input, SourceValue *value) {
    if (input.type == XR_XIR_STRING) { *value = input; return true; }
    if (input.type != XR_XIR_BOOL && !xr_xir_type_is_number(input.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "value has no admitted string conversion");
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_TO_STRING, XR_XIR_STRING, {input.id, 0}, {0}, 0, {0}}, value);
}
static bool source_number_cast(SourceContext *ctx, AstNode *node, SourceValue *value) {
    AsExprNode *cast = &node->as.as_expr;
    XrXirType target; SourceValue input;
    if (!cast->is_safe && source_type(ctx, cast->type, &target) && target == XR_XIR_STRING) {
        return source_plan_expression(ctx, cast->expr, (SourceExpectedType){false, XR_XIR_UNIT, false}, &input) &&
            source_text_conversion(ctx, node, input, value);
    }
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    if (cast->is_safe || !source_type(ctx, cast->type, &target) || !xr_xir_type_is_number(target))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "numeric cast requires a concrete nonnullable numeric target");
    SourceInteger literal;
    if (!source_direct_integer(ctx, cast->expr, &literal)) return false;
    SourceExpectedType context = {literal.present && !literal.negative && literal.magnitude > INT64_MAX,XR_XIR_U64, false};
    if (!source_plan_expression(ctx, cast->expr, context, &input)) return false;
    if (!xr_xir_type_is_number(input.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "numeric cast requires a concrete numeric operand");
    return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONVERT_NUMBER, target, {input.id, 0}, {0}, 0, {0}}, value);
}
#include "xxir_source_binary_plan.inc.c"
static bool source_arithmetic(SourceContext *ctx, AstNode *node, SourceExpectedType expected, SourceValue *value) {
    SourceValue left, right;
    if ((node->type == AST_BINARY_EQ || node->type == AST_BINARY_NE) && source_null_comparison(ctx->active_expression))
        return source_null_test(ctx, node, value);
    if (node->type == AST_UNARY_NEG || node->type == AST_UNARY_BNOT) {
        SourceInteger literal;
        if (!source_direct_integer(ctx, node->as.unary.operand, &literal) ||
            !source_plan_expression(ctx, node->as.unary.operand, (SourceExpectedType){literal.present && expected.present && xr_xir_type_is_integer(expected.type),
                expected.present ? expected.type : XR_XIR_UNIT, false}, &right)) return false;
        if (node->type == AST_UNARY_NEG && xr_xir_float_bits(right.type))
            return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NEG_FLOAT, right.type, {right.id, 0}, {0}, 0, {0}}, value);
        if (!xr_xir_type_is_integer(right.type)) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "unary arithmetic requires an integer");
        int64_t initial = 0;
        if (node->type == AST_UNARY_BNOT) initial = xr_xir_integer_signed(right.type) || xr_xir_integer_bits(right.type) == 64 ?
            -1 : (int64_t) ((UINT64_C(1) << xr_xir_integer_bits(right.type)) - 1);
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, right.type, {0}, {0}, initial, {0}}, &left)) return false;
    } else {
        SourceExpressionPlan *plan=ctx->active_expression;
        if (!plan->left || !plan->right)
            return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"binary expression has no collected operands");
        if (!source_plan_binary_prepare(ctx,plan,expected,true)) return false;
        SourceExpressionPlan *first=plan->left,*second=plan->right;
        bool shift=node->type==AST_BINARY_LSHIFT || node->type==AST_BINARY_RSHIFT;
        bool first_literal=first->integer.present || first->decimal.node || first->syntax->type==AST_LITERAL_NULL;
        bool second_literal=second->integer.present || second->decimal.node || second->syntax->type==AST_LITERAL_NULL;
        if (!shift && first_literal && !second_literal && !first->numeric.ready) {
            if (!source_plan_complete(ctx,second,&right)) return false;
            first->expected=(SourceExpectedType){true,right.type, false};
            if (!source_plan_complete(ctx,first,&left)) return false;
        } else {
            if (!source_plan_complete(ctx,first,&left)) return false;
            if (!second->numeric.ready && second_literal && !shift)
                second->expected=(SourceExpectedType){true,left.type, false};
            if (!source_plan_complete(ctx,second,&right)) return false;
        }
        return plan->binary ? source_binary_emit(ctx,plan->binary,left,right,value) :
            source_binary_apply(ctx,node,node->type,left,right,value);
    }
    return source_binary_apply(ctx,node,node->type,left,right,value);
}
#include "xxir_source_update.inc.c"
typedef struct SourceConditionalRecipe {
    SourceConversionRecipe yes,no;
    XrXirType result;
} SourceConditionalRecipe;
static bool source_conditional_plan(SourceContext *ctx,AstNode *node,XrXirType yes,XrXirType no,
    SourceExpectedType hint,SourceConditionalRecipe *output) {
    XrXirType common=hint.present ? hint.type : yes;
    if (!hint.present && yes!=no) {
        if (xr_xir_float_bits(yes) && xr_xir_float_bits(no)) common=XR_XIR_F64;
        else if (!xr_xir_type_is_integer(yes) || !xr_xir_type_is_integer(no) ||
            xr_xir_integer_signed(yes)!=xr_xir_integer_signed(no))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"conditional requires a common admitted type");
        if (xr_xir_integer_bits(no)>xr_xir_integer_bits(common)) common=no;
    }
    SourceConditionalRecipe recipe={0};recipe.result=common;
    if (!source_conversion_plan(ctx,node,yes,(SourceExpectedType){true,common, false},&recipe.yes) ||
        !source_conversion_plan(ctx,node,no,(SourceExpectedType){true,common, false},&recipe.no)) return false;
    *output=recipe;return true;
}
static bool source_conditional(SourceContext *ctx, AstNode *node, SourceExpectedType expected, SourceValue *value) {
    SourceValue condition, yes, no;
    SourceExpressionPlan *plan=ctx->active_expression;
    if (!plan->condition || !plan->left || !plan->right)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"conditional has no collected operands");
    if (!source_plan_binary_prepare(ctx,plan,expected,true)) return false;
    if (!plan->conditional) {
        if (!plan->left->type_ready) plan->left->expected=expected;
        if (!plan->right->type_ready) plan->right->expected=expected;
    }
    if (!source_plan_complete(ctx,plan->condition,&condition)) return false;
    if (condition.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "conditional requires bool");
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t branch = body->count, yes_block = body->block_count;
    SourceFact *entry_facts = ctx->facts; SourceFactSet arm_facts = {0};
    if (!source_condition_facts(ctx, node->as.ternary.condition, true, &arm_facts, 0)) return false;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id}, {0}, 0, {0}}, NULL) ||
        !begin_block(ctx) || !source_facts_apply(ctx, &arm_facts) || !source_plan_complete(ctx,plan->left,&yes)) return false;
    ctx->facts = entry_facts;
    uint32_t yes_end = body->current_block.identity, yes_jump = body->count;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    uint32_t no_block = body->block_count;
    arm_facts = (SourceFactSet) {0};
    if (!source_condition_facts(ctx, node->as.ternary.condition, false, &arm_facts, 0)) return false;
    if (!begin_block(ctx) || !source_facts_apply(ctx, &arm_facts) || !source_plan_complete(ctx,plan->right,&no)) return false;
    ctx->facts = entry_facts;
    SourceConditionalRecipe local;
    const SourceConditionalRecipe *recipe=plan->conditional;
    if (!recipe) {
        if (!source_conditional_plan(ctx,node,yes.type,no.type,expected,&local)) return false;
        recipe=&local;
    }
    if (!source_conversion_emit(ctx,&recipe->no,&no)) return false;
    uint32_t no_end = body->current_block.identity, no_jump = body->count;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    if (recipe->yes.needed) {
        body->recipes[yes_jump].instruction.targets[0] = body->block_count;
        if (!begin_block(ctx) || !source_conversion_emit(ctx,&recipe->yes,&yes)) return false;
        yes_end = body->current_block.identity; yes_jump = body->count;
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    }
    uint32_t join = body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[0] = yes_block; body->recipes[branch].instruction.targets[1] = no_block;
    body->recipes[yes_jump].instruction.targets[0] = join; body->recipes[no_jump].instruction.targets[0] = join;
    if (yes.type == XR_XIR_UNIT) { *value = (SourceValue) {0, XR_XIR_UNIT}; return true; }
    SourceValue inputs[] = {{yes_end, XR_XIR_UNIT}, yes, {no_end, XR_XIR_UNIT}, no};
    if (yes_end > no_end) {
        inputs[0].id = no_end; inputs[1] = no; inputs[2].id = yes_end; inputs[3] = yes;
    }
    return source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_PHI, yes.type, {0}, {0}, 0, {0}}, inputs, 4, value);
}
#include "xxir_source_match.inc.c"
static bool source_function_value(SourceContext *ctx, AstNode *node, SourceName *binding, SourceName *symbol, SourceExpectedType expected, SourceValue *value) {
    if (symbol && symbol->kind == SOURCE_IMPORT) symbol = imported_declaration(ctx, symbol, symbol->imported);
    if (!symbol || symbol->kind != SOURCE_FUNCTION)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "function value requires a declared function");
    if (!source_query_reference(ctx, node, binding, symbol, XR_XIR_SOURCE_FUNCTION_VALUE)) return false;
    const XrXirFunction *function = &ctx->functions[symbol->index];
    for (uint32_t p = 0; p < function->parameter_count; ++p) {
        if (!source_work(ctx, node)) return false;
        if (xr_xir_type_is_cell(&ctx->types, function->parameters[p]))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "a function with ref parameters cannot be used as a value");
    }
    SourceTypeArguments arguments = {0};
    if (node->type == AST_FUNCTION_REF) {
        arguments.refs = node->as.function_ref.type_args;
        arguments.count = (uint32_t) node->as.function_ref.type_arg_count;
    }
    XrXirInstruction op = {XR_XIR_FUNCTION_REF,XR_XIR_UNIT,{0},{0},symbol->index, {0}};
    if (!source_instantiation(ctx,node,(XrXirDeclarationContext){XR_XIR_CONTEXT_FUNCTION,symbol->index,0},&arguments)) return false;
    uint32_t count = function->parameter_count;
    XrXirCallableParameter *parameters = count ? source_alloc(ctx,count,sizeof(*parameters)) : NULL;
    if (count && !parameters) return false;
    for (uint32_t p = 0; p < count; ++p)
        if (!source_substitute(ctx,&arguments.substitution,function->parameters[p],0,&parameters[p].type)) return false;
    XrXirType result;
    if (!source_substitute(ctx,&arguments.substitution,function->result,0,&result) ||
        !source_signature(ctx,parameters,count,result,&op.type)) return false;
    if (!source_reference_promise(ctx, node, symbol->index, expected, &op.type)) return false;
    return source_type_arguments(ctx, node, arguments.substitution.types, arguments.count, &op) && source_recipe_record(ctx,op,value);
}
static bool source_explicit_reference(SourceContext *ctx, AstNode *node, SourceExpectedType expected, SourceValue *value) {
    AstNode *callee = node->as.function_ref.callee;
    SourceName *symbol = NULL, *binding = NULL;
    if (callee && callee->type == AST_VARIABLE) binding = symbol = visible_name(ctx,callee->as.variable.name);
    else if (callee && callee->type == AST_MEMBER_ACCESS) {
        MemberAccessNode *member = &callee->as.member_access;
        SourceName *base = member->object->type == AST_VARIABLE ? visible_name(ctx,member->object->as.variable.name) : NULL;
        if (base && base->kind == SOURCE_MODULE) { binding = base; symbol = imported_declaration(ctx,base,member->name); }
        else {
            SourceTypeArguments arguments = {node->as.function_ref.type_args, (uint32_t)node->as.function_ref.type_arg_count, {0}};
            return source_member_value(ctx, callee, &arguments, expected, value);
        }
    }
    return source_function_value(ctx,node,binding,symbol,expected,value);
}
static bool source_closure(SourceContext *ctx, AstNode *node, SourceExpectedType expected, SourceValue *value);
static bool source_defer(SourceContext *ctx, AstNode *node);
static bool expression_body(SourceContext *ctx, AstNode *node, SourceExpectedType context, SourceValue *value) {
    switch (node->type) {
    case AST_LITERAL_NULL:
        if (!context.present || !xr_xir_type_is_nullable(&ctx->types,context.type))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"null requires an exact nullable context");
        return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NULLABLE_NONE,context.type,{0},{0},0,{0}},value);
    case AST_ARRAY_LITERAL:
        if (context.present && xr_xir_type_is_nullable(&ctx->types,context.type))
            context.type = xr_xir_nullable_element(&ctx->types,context.type);
        return source_array_literal(ctx, node, context, value);
    case AST_INDEX_GET: return source_array_get(ctx, node, node->as.index_get.array,
        node->as.index_get.index, NULL, NULL, value);
    case AST_INDEX_SET: return source_array_set(ctx, node, node->as.index_set.array,
        node->as.index_set.index, node->as.index_set.value, NULL, true, value);
    case AST_LITERAL_TRUE: case AST_LITERAL_FALSE: case AST_LITERAL_STRING:
        return source_literal(ctx, node, value);
    case AST_TEMPLATE_STRING: return source_template(ctx, node, value);
    case AST_NEW_EXPR: return source_array_construct(ctx, node, value);
    case AST_AS_EXPR: return source_number_cast(ctx, node, value);
    case AST_TERNARY: return source_conditional(ctx, node, context, value);
    case AST_MATCH_EXPR: {
        ++ctx->flow_alternatives;
        bool matched = source_facts_kill(ctx, node) &&
            source_match(ctx, node, context, !ctx->active_expression->statement_match, value);
        --ctx->flow_alternatives;
        return matched && source_facts_kill(ctx, node);
    }
    case AST_GROUPING: {
        SourceExpressionPlan *child=ctx->active_expression->left;
        if (!child) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"group has no collected expression");
        if (!child->type_ready) child->expected=context;
        return source_plan_complete(ctx,child,value);
    }
    case AST_CALL_EXPR: return source_call(ctx, node, context, value);
    case AST_FUNCTION_REF: return source_explicit_reference(ctx, node, context, value);
    case AST_FUNCTION_EXPR:
        if (context.present && xr_xir_type_is_nullable(&ctx->types,context.type))
            context.type=xr_xir_nullable_element(&ctx->types,context.type);
        return source_closure(ctx, node, context, value);
    case AST_STRUCT_LITERAL: return source_struct_literal(ctx, node, value);
    case AST_ENUM_CONSTRUCT: return source_enum_literal(ctx, node, value);
    case AST_MEMBER_SET: return source_struct_set(ctx, node, value);
    case AST_UNARY_NOT: case AST_BINARY_AND: case AST_BINARY_OR: return source_logic(ctx, node, value);
    case AST_NULLISH_COALESCE: return source_coalesce(ctx, node, context, value);
    case AST_FORCE_UNWRAP: return source_force_unwrap(ctx, node, value);
    case AST_MEMBER_ACCESS: {
        MemberAccessNode *member = &node->as.member_access;
        SourceName *base = member->object->type == AST_VARIABLE ? visible_name(ctx, member->object->as.variable.name) : NULL;
        SourceTypeArguments arguments = {0};
        if (!base || base->kind != SOURCE_MODULE) return source_member_value(ctx, node, &arguments, context, value);
        return source_function_value(ctx, node, base, imported_declaration(ctx, base, member->name), context, value);
    }
    case AST_THIS_EXPR: case AST_VARIABLE: {
        SourceName *symbol = ctx->active_expression->binding;
        if (source_local_name(symbol) && symbol->declaration) {
            SourceName *live_binding=ctx->locals;
            for (;live_binding;live_binding=live_binding->next) {
                if (!source_work(ctx,node)) return false;
                if (live_binding->declaration==symbol->declaration) break;
            }
            if (!source_name_ready(live_binding) || live_binding->kind!=symbol->kind || live_binding->type!=symbol->type)
                return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"declaration value is not ready in its lexical owner");
            symbol=live_binding;
        }
        if (symbol && (symbol->kind == SOURCE_FUNCTION || symbol->kind == SOURCE_IMPORT))
            return source_function_value(ctx, node, symbol, symbol, context, value);
        if (!source_name_ready(symbol))
            return source_fail(ctx, node, XR_XIR_BAD_VALUE, "name is not an initialized value");
        if (!source_query_reference(ctx, node, symbol, symbol, XR_XIR_SOURCE_READ)) return false;
        if (symbol->kind == SOURCE_UNIT_LOCAL) {
            *value = (SourceValue){UINT32_MAX, XR_XIR_UNIT}; return true;
        }
        if (symbol->kind == SOURCE_LOCAL) {
            if (symbol->construction) {
                if (xr_xir_type_is_class(&ctx->types,symbol->type)) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"class this cannot escape before constructor completion");
                return source_constructor_value(ctx,node,value);
            }
            SourceValue read = {symbol->index, symbol->type};
            if (symbol->mutable && !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_READ, symbol->type,
                {symbol->index, 0}, {0}, 0, {0}}, &read)) return false;
            /* A binding known to hold a value reads as its element. */
            if (source_fact_known(ctx, symbol)) return source_unwrap_value(ctx, node, read, value);
            *value = read; return true;
        }
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_SLOT_LOAD, symbol->type, {0, 0}, {0, 0}, symbol->index, {0}}, value);
    }
    case AST_UNARY_BNOT: case AST_BINARY_BAND: case AST_BINARY_BOR: case AST_BINARY_BXOR:
    case AST_BINARY_LSHIFT: case AST_BINARY_RSHIFT:
    case AST_UNARY_NEG: case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
    case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_EQ: case AST_BINARY_NE:
    case AST_BINARY_LT: case AST_BINARY_LE: case AST_BINARY_GT: case AST_BINARY_GE:
        return source_arithmetic(ctx, node, context, value);
    case AST_COMPOUND_ASSIGNMENT: return source_compound(ctx, node, value);
    case AST_ASSIGNMENT: {
        SourceName *symbol = visible_name(ctx, node->as.assignment.name);
        SourceValue assigned;
        if (!symbol || !symbol->mutable || (!source_local_name(symbol) && symbol->kind != SOURCE_SLOT && symbol->kind != SOURCE_UNIT_SLOT))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "assignment requires a mutable binding");
        if (!source_query_reference(ctx, node, symbol, symbol, XR_XIR_SOURCE_WRITE)) return false;
        if (!source_plan_expression(ctx, node->as.assignment.value, (SourceExpectedType){true,symbol->type, false}, &assigned)) return false;
        if (assigned.type != symbol->type) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "assignment type mismatch");
        if (symbol->kind == SOURCE_LOCAL && !source_facts_assign(ctx, symbol, source_value_is_some(ctx, assigned))) return false;
        if (symbol->kind == SOURCE_UNIT_LOCAL) {
            *value = (SourceValue){UINT32_MAX, XR_XIR_UNIT}; return true;
        }
        if (symbol->kind == SOURCE_LOCAL) {
            if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT,
                {symbol->index, assigned.id}, {0}, 0, {0}}, NULL)) return false;
        }
        else if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {assigned.type == XR_XIR_UNIT ? 0 : assigned.id, 0}, {0, 0}, symbol->index, {0}}, NULL)) return false;
        *value = assigned; return true;
    }
    default: return source_fail_syntax(ctx, node, "expression");
    }
}
static bool expression(SourceContext *ctx, AstNode *node, SourceValue *value) {
    return source_plan_expression(ctx, node, (SourceExpectedType){false,XR_XIR_UNIT, false}, value);
}
static bool source_plan_soft_numeric(SourceContext *ctx,SourceExpressionPlan *plan,bool *soft) {
    if (!source_work(ctx,plan->syntax)) return false;
    *soft=plan->integer.present || plan->decimal.node || plan->syntax->type==AST_TERNARY;
    if (*soft || !plan->left) return true;
    bool left=false,right=false;
    if (!source_plan_soft_numeric(ctx,plan->left,&left)) return false;
    if (plan->syntax->type==AST_GROUPING) {*soft=left;return true;}
    if (!plan->right || (plan->syntax->type>=AST_BINARY_EQ && plan->syntax->type<=AST_BINARY_GE)) return true;
    if (!source_plan_soft_numeric(ctx,plan->right,&right)) return false;
    *soft=left && right;return true;
}
static bool source_plan_numeric_prepare(SourceContext *ctx,SourceExpressionPlan *plan,SourceExpectedType hint,bool defaults) {
    if (plan->type_ready) return true;
    if (!plan->integer.present && !plan->decimal.node) return true;
    SourceNumericRequest request={plan->integer.present ? &plan->integer : NULL,
        plan->decimal.node ? &plan->decimal : NULL,hint,defaults};
    if (!source_numeric_plan(ctx,plan->syntax,&request,&plan->numeric)) return false;
    if (plan->numeric.ready) {plan->type_ready=true;plan->ground_type=plan->numeric.type;}
    return true;
}
static bool source_plan_binary_prepare(SourceContext *ctx,SourceExpressionPlan *plan,SourceExpectedType hint,bool defaults) {
    if (plan->syntax->type==AST_TERNARY && plan->left && plan->right) {
        if (plan->conditional) return true;
        if (!source_work(ctx,plan->syntax)) return false;
        if (!source_plan_binary_prepare(ctx,plan->left,hint,defaults) ||
            !source_plan_numeric_prepare(ctx,plan->left,hint,defaults) ||
            !source_plan_binary_prepare(ctx,plan->right,hint,defaults) ||
            !source_plan_numeric_prepare(ctx,plan->right,hint,defaults)) return false;
        if (!plan->left->type_ready || !plan->right->type_ready) return true;
        SourceConditionalRecipe recipe;
        if (!source_conditional_plan(ctx,plan->syntax,plan->left->ground_type,plan->right->ground_type,hint,&recipe)) return false;
        SourceConditionalRecipe *recorded_recipe=source_recipe_storage(ctx,1,sizeof(*recorded_recipe));
        if (!recorded_recipe) return false;
        *recorded_recipe=recipe;plan->conditional=recorded_recipe;plan->type_ready=true;plan->ground_type=recipe.result;
        return true;
    }
    if (plan->syntax->type==AST_GROUPING && plan->left) {
        if (!source_plan_binary_prepare(ctx,plan->left,hint,defaults) ||
            !source_plan_numeric_prepare(ctx,plan->left,hint,defaults)) return false;
        if (plan->left->type_ready) {plan->type_ready=true;plan->ground_type=plan->left->ground_type;}
        return true;
    }
    if (plan->binary || !plan->left || !plan->right) return true;
    if (!source_work(ctx,plan->syntax)) return false;
    SourceExpressionPlan *left=plan->left,*right=plan->right;
    bool left_literal=left->integer.present || left->decimal.node;
    bool right_literal=right->integer.present || right->decimal.node;
    if (defaults && !hint.present && !left->type_ready && !right->type_ready && left_literal!=right_literal)
        return true;
    bool shift=plan->syntax->type==AST_BINARY_LSHIFT || plan->syntax->type==AST_BINARY_RSHIFT;
    SourceExpectedType left_hint=hint,right_hint=hint;
    if (left->type_ready) right_hint=(SourceExpectedType){!shift,left->ground_type, false};
    if (right->type_ready && !shift) left_hint=(SourceExpectedType){true,right->ground_type, false};
    if (!hint.present && !left->type_ready && !right->type_ready && (left->decimal.node || right->decimal.node))
        left_hint=right_hint=(SourceExpectedType){defaults,XR_XIR_F64, false};
    if (!source_plan_binary_prepare(ctx,left,left_hint,defaults) ||
        !source_plan_numeric_prepare(ctx,left,left_hint,defaults)) return false;
    if (left->type_ready && !shift) right_hint=(SourceExpectedType){true,left->ground_type, false};
    if (!source_plan_binary_prepare(ctx,right,right_hint,defaults) ||
        !source_plan_numeric_prepare(ctx,right,right_hint,defaults)) return false;
    if (!left->type_ready || !right->type_ready) return true;
    SourceBinaryRecipe recipe;
    if (!source_binary_plan(ctx,plan->syntax,plan->syntax->type,left->ground_type,right->ground_type,&recipe)) return false;
    SourceBinaryRecipe *recorded_recipe=source_recipe_storage(ctx,1,sizeof(*recorded_recipe));
    if (!recorded_recipe) return false;
    *recorded_recipe=recipe;plan->binary=recorded_recipe;plan->type_ready=true;plan->ground_type=recipe.result;
    return true;
}
#include "xxir_source_ground_type.inc.c"
static SourceExpressionPlan *source_plan_collect(SourceContext *ctx, AstNode *node, SourceExpectedType context) {
    if (!node || !source_work(ctx,node)) return NULL;
    SourceFunction *body=&ctx->bodies[ctx->function];
    if (body->region_sealed || body->expression_count==UINT32_MAX) {
        source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"expression region is not open"); return NULL;
    }
    if (!body->expressions || body->expressions->count==32) {
        SourceExpressionStorage *storage=source_recipe_storage(ctx,1,sizeof(*storage));
        if (!storage) return NULL;
        storage->next=body->expressions;body->expressions=storage;
    }
    SourceExpressionPlan *parent=ctx->active_expression;
    SourceExpressionPlan *plan=&body->expressions->expressions[body->expressions->count++];
    plan->parent=parent && parent->owner==ctx->function ? parent : NULL;
    plan->syntax=node;plan->owner=ctx->function;plan->identity=body->expression_count++;
    if (node->type==AST_THIS_EXPR || node->type==AST_VARIABLE) {
        plan->binding=visible_name(ctx,node->type==AST_THIS_EXPR ? "this" : node->as.variable.name);
        if (source_name_ready(plan->binding)) {
            plan->type_ready=true;plan->ground_type=source_symbol_type(ctx,plan->binding);
        }
    } else if (node->type==AST_LITERAL_TRUE || node->type==AST_LITERAL_FALSE || node->type==AST_LITERAL_STRING) {
        plan->type_ready=true;plan->ground_type=node->type==AST_LITERAL_STRING ? XR_XIR_STRING : XR_XIR_BOOL;
    } else if (node->type==AST_CALL_EXPR && node->as.call_expr.callee && node->as.call_expr.callee->type==AST_VARIABLE) {
        plan->binding=visible_name(ctx,node->as.call_expr.callee->as.variable.name);
        if (plan->binding && plan->binding->kind==SOURCE_FUNCTION && !ctx->generics[plan->binding->index].parameter_count) {
            plan->type_ready=true;plan->ground_type=ctx->functions[plan->binding->index].result;
        }
    }
    if (!plan->type_ready) {
        SourceExpectedType ground;
        if (!source_ground_type(ctx,node,0,&ground)) return NULL;
        if (ground.present) {plan->type_ready=true;plan->ground_type=ground.type;}
    }
    plan->expected=context;
    plan->entry=body->block_count ? body->current_block : (SourceBlockReference){ctx->function,0};
    plan->state=SOURCE_TERM_UNRESOLVED;
    if (!source_direct_integer(ctx,node,&plan->integer) || !source_direct_decimal(ctx,node,&plan->decimal)) {
        plan->state=SOURCE_TERM_FAILED;return NULL;
    }
    switch(node->type) {
    case AST_MATCH_EXPR:
        if (ctx->depth>=128) {source_fail(ctx,node,XR_XIR_BUDGET,"expression collection depth exhausted");return NULL;}
        ctx->active_expression=plan;++ctx->depth;
        plan->left=source_plan_collect(ctx,node->as.match_expr.expr,(SourceExpectedType){false,XR_XIR_UNIT, false});
        bool match_ok=plan->left && source_match_prepare(ctx,plan);
        --ctx->depth;ctx->active_expression=parent;
        if (!match_ok) return NULL;
        break;
    case AST_TERNARY:
        if (ctx->depth>=128) {source_fail(ctx,node,XR_XIR_BUDGET,"expression collection depth exhausted");return NULL;}
        ctx->active_expression=plan;++ctx->depth;
        plan->condition=source_plan_collect(ctx,node->as.ternary.condition,(SourceExpectedType){false,XR_XIR_UNIT, false});
        {
            /* Each arm is typed under the facts of its own direction (N-7). */
            SourceFact *entry_facts=ctx->facts; SourceFactSet arm_facts={0};
            if (plan->condition && source_condition_facts(ctx,node->as.ternary.condition,true,&arm_facts,0) &&
                source_facts_apply(ctx,&arm_facts))
                plan->left=source_plan_collect(ctx,node->as.ternary.true_expr,(SourceExpectedType){false,XR_XIR_UNIT, false});
            ctx->facts=entry_facts; arm_facts=(SourceFactSet){0};
            if (plan->left && source_condition_facts(ctx,node->as.ternary.condition,false,&arm_facts,0) &&
                source_facts_apply(ctx,&arm_facts))
                plan->right=source_plan_collect(ctx,node->as.ternary.false_expr,(SourceExpectedType){false,XR_XIR_UNIT, false});
            ctx->facts=entry_facts;
        }
        --ctx->depth;ctx->active_expression=parent;
        if (!plan->condition || !plan->left || !plan->right || !source_plan_binary_prepare(ctx,plan,(SourceExpectedType){false,XR_XIR_UNIT, false},false)) return NULL;
        break;
    case AST_GROUPING:
        if (plan->integer.present || plan->decimal.node) break;
        /* Nonliteral grouping owns its child; no re-collection during completion. */
        /* fall through */
    case AST_BINARY_ADD:case AST_BINARY_SUB:case AST_BINARY_MUL:case AST_BINARY_DIV:case AST_BINARY_MOD:
    case AST_BINARY_BAND:case AST_BINARY_BOR:case AST_BINARY_BXOR:case AST_BINARY_LSHIFT:case AST_BINARY_RSHIFT:
    case AST_BINARY_EQ:case AST_BINARY_NE:case AST_BINARY_LT:case AST_BINARY_LE:case AST_BINARY_GT:case AST_BINARY_GE:
        if (ctx->depth>=128) {source_fail(ctx,node,XR_XIR_BUDGET,"expression collection depth exhausted");return NULL;}
        ctx->active_expression=plan;++ctx->depth;
        plan->left=source_plan_collect(ctx,node->type==AST_GROUPING ? node->as.grouping : node->as.binary.left,(SourceExpectedType){false,XR_XIR_UNIT, false});
        if (plan->left && node->type!=AST_GROUPING) plan->right=source_plan_collect(ctx,node->as.binary.right,(SourceExpectedType){false,XR_XIR_UNIT, false});
        --ctx->depth;ctx->active_expression=parent;
        if (!plan->left || (node->type!=AST_GROUPING && !plan->right) || !source_plan_binary_prepare(ctx,plan,(SourceExpectedType){false,XR_XIR_UNIT, false},false)) return NULL;
        if (node->type>=AST_BINARY_EQ && node->type<=AST_BINARY_GE) {plan->type_ready=true;plan->ground_type=XR_XIR_BOOL;}
        break;
    default:break;
    }
    return plan;
}
static bool source_plan_complete(SourceContext *ctx, SourceExpressionPlan *plan, SourceValue *value) {
    if (!plan || plan->owner!=ctx->function || ctx->bodies[ctx->function].region_sealed)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"expression plan has no active owner");
    if (plan->state==SOURCE_TERM_GROUND) { *value=plan->value; return true; }
    if (plan->state!=SOURCE_TERM_UNRESOLVED)
        return source_fail(ctx,plan->syntax,XR_XIR_BAD_STRUCTURE,"expression plan cannot be retried");
    AstNode *node=plan->syntax;
    if (ctx->depth>=128) return source_fail(ctx,node,XR_XIR_BUDGET,"source expression depth exhausted");
    SourceExpectedType context=plan->expected;
    SourceConversionRecipe checked_conversion;
    SourceExpressionPlan *parent=ctx->active_expression;
    SourceFunction *body=&ctx->bodies[plan->owner];
    plan->entry=body->block_count ? body->current_block : (SourceBlockReference){ctx->function,0};
    plan->state=SOURCE_TERM_CHECKING;ctx->active_expression=plan;++ctx->depth;
    bool result=plan->numeric.ready ? source_numeric_emit(ctx,&plan->numeric,&plan->value) :
        (plan->integer.present ? source_integer(ctx,node,&plan->integer,context,&plan->value) : plan->decimal.node ?
        source_decimal(ctx,&plan->decimal,context,&plan->value) : expression_body(ctx,node,context,&plan->value));
    --ctx->depth;ctx->active_expression=parent;
    if (result && plan->conversion_ready) {
        checked_conversion=plan->conversion;
        if (context.present && context.type!=checked_conversion.target)
            result=source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"prepared expression context changed");
    } else if (result) result=source_conversion_plan(ctx,node,plan->value.type,context,&checked_conversion);
    result=result && source_conversion_emit(ctx,&checked_conversion,&plan->value) && source_query_expression(ctx,node,plan->value.type);
    if (result) {
        SourceFunction *owner=&ctx->bodies[plan->owner];
        plan->exit=owner->block_count ? owner->current_block : plan->entry;
        plan->state=SOURCE_TERM_GROUND;plan->type_ready=true;plan->ground_type=plan->value.type;*value=plan->value;
    } else plan->state=SOURCE_TERM_FAILED;
    return result;
}
static bool source_plan_expression(SourceContext *ctx, AstNode *node, SourceExpectedType context, SourceValue *value) {
    SourceExpressionPlan *plan=source_plan_collect(ctx,node,context);
    return plan && source_plan_complete(ctx,plan,value);
}

static bool source_plan_discard(SourceContext *ctx,uint32_t first) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    for (SourceExpressionStorage *storage=body->expressions;storage;storage=storage->next) {
        for (uint32_t e=storage->count;e;--e) {
            SourceExpressionPlan *plan=&storage->expressions[e-1];
            if (!source_work(ctx,plan->syntax)) return false;
            if (plan->identity<first) return true;
            if (plan->owner!=ctx->function || (plan->state!=SOURCE_TERM_GROUND && plan->state!=SOURCE_TERM_DISCARDED))
                return source_fail(ctx,plan->syntax,XR_XIR_BAD_STRUCTURE,"uncompleted expression cannot be discarded");
            plan->state=SOURCE_TERM_DISCARDED;
        }
    }
    return true;
}

static bool source_binding(SourceContext *ctx, AstNode *node, bool top) {
    VarDeclNode *decl = &node->as.var_decl;
    if (decl->attr_count || (!decl->initializer && (decl->is_const || !decl->type_annotation)))
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "binding needs an initializer or explicit mutable type and no attributes");
    SourceValue initial;
    XrXirType annotation = XR_XIR_UNIT;
    if (decl->type_annotation && !source_type(ctx, decl->type_annotation, &annotation)) return false;
    if (decl->initializer) {
        if (!source_plan_expression(ctx, decl->initializer, (SourceExpectedType){decl->type_annotation != NULL,annotation, false}, &initial)) return false;
    } else if (!source_default_value(ctx, node, annotation, &initial)) return false;
    if (decl->type_annotation && annotation != initial.type)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "binding annotation mismatch");
    SourceName *symbol;
    bool initial_some = false;
    if (top) {
        symbol = find_name(ctx, ctx->names[ctx->module], decl->name);
        if (!symbol || symbol->kind != SOURCE_SLOT) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "binding declaration missing");
        symbol->type = initial.type; ctx->slots[symbol->index].type = initial.type;
        if (initial.type == XR_XIR_UNIT) symbol->kind = SOURCE_UNIT_SLOT;
        source_query_binding_type(ctx, symbol);
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_SLOT_INIT, XR_XIR_UNIT, {initial.type == XR_XIR_UNIT ? 0 : initial.id, 0}, {0, 0}, symbol->index, {0}}, NULL);
    }
    for (SourceName *p = ctx->locals; p != ctx->scope; p = p->next) {
        if (!source_work(ctx, node)) return false;
        if (source_text_same(ctx, NULL, p->name, decl->name)) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate local name");
    }
    symbol = source_alloc(ctx, 1, sizeof(*symbol));
    if (!symbol) return false;
    XrXirType logical_type = initial.type;
    if (!decl->is_const && logical_type != XR_XIR_UNIT) {
        XrXirType cell;
        if (!source_cell_type(ctx, initial.type, &cell) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CELL_NEW, cell, {initial.id, 0}, {0}, 0, {0}}, &initial)) return false;
    }
    *symbol = (SourceName) {ctx->locals, decl->name, NULL, node,
        logical_type == XR_XIR_UNIT ? SOURCE_UNIT_LOCAL : SOURCE_LOCAL,
        logical_type == XR_XIR_UNIT ? UINT32_MAX : initial.id,
        ctx->module, logical_type, !decl->is_const, false, 0};
    ctx->locals = symbol;
    if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_BINDING, ctx->bodies[ctx->function].declaration,
        source_query_range(ctx, node, symbol->name))) return false;
    source_query_binding_type(ctx, symbol);
    return source_facts_assign(ctx, symbol, initial_some);
}
/* A nested function is a closure value bound immutably from its declaration on. */
static bool source_nested_function(SourceContext *ctx, AstNode *node) {
    FunctionDeclNode *decl = &node->as.function_decl;
    SourceName *symbol = NULL;
    for (SourceName *p = ctx->locals; p != ctx->scope; p = p->next) {
        if (!source_work(ctx, node)) return false;
        if (p->kind == SOURCE_PENDING && p->node == node) symbol = p;
        else if (source_text_same(ctx, NULL, p->name, decl->name))
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate local name");
    }
    SourceValue value;
    if (!source_closure(ctx, node, (SourceExpectedType){false, XR_XIR_UNIT, false}, &value)) return false;
    if (!symbol) {
        symbol = source_alloc(ctx, 1, sizeof(*symbol));
        if (!symbol) return false;
        *symbol = (SourceName) {ctx->locals, decl->name, NULL, node, SOURCE_PENDING, UINT32_MAX, ctx->module, XR_XIR_UNIT, false, false, 0};
        ctx->locals = symbol;
    }
    symbol->kind = SOURCE_LOCAL; symbol->index = value.id; symbol->type = value.type;
    symbol->module = ctx->module; symbol->mutable = false; symbol->construction = false;
    if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_BINDING, ctx->bodies[ctx->function].declaration,
        source_query_range(ctx, node, symbol->name))) return false;
    source_query_binding_type(ctx, symbol);
    return true;
}
/* Reserve every nested function name of a block before its first statement runs. */
static bool source_block_functions(SourceContext *ctx, AstNode *block) {
    for (int i = 0; i < block->as.block.count; ++i) {
        AstNode *node = block->as.block.statements[i];
        if (!source_work(ctx, block)) return false;
        if (!node || node->type != AST_FUNCTION_DECL) continue;
        for (SourceName *p = ctx->locals; p != ctx->scope; p = p->next) {
            if (!source_work(ctx, node)) return false;
            if (source_text_same(ctx, NULL, p->name, node->as.function_decl.name))
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate local name");
        }
        SourceName *symbol = source_alloc(ctx, 1, sizeof(*symbol));
        if (!symbol) return false;
        *symbol = (SourceName) {ctx->locals, node->as.function_decl.name, NULL, node, SOURCE_PENDING, UINT32_MAX,
            ctx->module, XR_XIR_UNIT, false, false, 0};
        ctx->locals = symbol;
    }
    return true;
}
static bool scoped_statement(SourceContext *ctx, AstNode *node) {
    if (ctx->depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "source control depth exhausted");
    SourceName *saved = ctx->locals, *scope = ctx->scope;
    ctx->scope = saved; ++ctx->depth;
    bool result = statement(ctx, node, false);
    --ctx->depth; ctx->locals = saved; ctx->scope = scope; return result;
}
static bool source_if(SourceContext *ctx, AstNode *node) {
    SourceValue condition;
    SourceFactSet yes_facts = {0}, no_facts = {0};
    if (!source_condition_facts(ctx, node->as.if_stmt.condition, true, &yes_facts, 0) ||
        !source_condition_facts(ctx, node->as.if_stmt.condition, false, &no_facts, 0)) return false;
    if (!expression(ctx, node->as.if_stmt.condition, &condition)) return false;
    if (condition.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "if requires bool");
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t branch = body->count, yes = body->block_count;
    SourceFact *entry_facts = ctx->facts;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id, 0}, {0}, 0, {0}}, NULL) ||
        !begin_block(ctx) || !source_facts_apply(ctx, &yes_facts) ||
        !scoped_statement(ctx, node->as.if_stmt.then_branch)) return false;
    SourceFact *yes_end = ctx->facts; bool yes_returned = ctx->returned;
    uint32_t yes_jump = UINT32_MAX, no_jump = UINT32_MAX;
    if (!ctx->returned) {
        yes_jump = body->count;
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    }
    uint32_t no = body->block_count;
    if (!begin_block(ctx)) return false;
    ctx->facts = entry_facts;
    if (!source_facts_apply(ctx, &no_facts)) return false;
    if (node->as.if_stmt.else_branch && !scoped_statement(ctx, node->as.if_stmt.else_branch)) return false;
    SourceFact *no_end = ctx->facts; bool no_returned = ctx->returned;
    if (!ctx->returned) {
        no_jump = body->count;
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    }
    body->recipes[branch].instruction.targets[0] = yes; body->recipes[branch].instruction.targets[1] = no;
    ctx->returned = yes_jump == UINT32_MAX && no_jump == UINT32_MAX;
    /* A path that exits leaves its opposite's facts behind (N-8); otherwise both paths must agree (N-9). */
    if (yes_returned && no_returned) ctx->facts = entry_facts;
    else if (yes_returned) ctx->facts = no_end;
    else if (no_returned) ctx->facts = yes_end;
    else if (!source_facts_join(ctx, yes_end, no_end, &ctx->facts)) return false;
    if (!ctx->returned) {
        uint32_t join = body->block_count;
        if (!begin_block(ctx)) return false;
        if (yes_jump != UINT32_MAX) body->recipes[yes_jump].instruction.targets[0] = join;
        if (no_jump != UINT32_MAX) body->recipes[no_jump].instruction.targets[0] = join;
    }
    return true;
}
static bool source_increment(SourceContext *ctx, AstNode *node) {
    const char *name = node->type == AST_INC ? node->as.inc.name : node->as.dec.name;
    SourceName *symbol = visible_name(ctx, name);
    if (!symbol || !symbol->mutable || !xr_xir_type_is_integer(symbol->type) ||
        (symbol->kind != SOURCE_LOCAL && symbol->kind != SOURCE_SLOT && symbol->kind != SOURCE_UNIT_SLOT))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "increment requires a mutable integer binding");
    if (!source_query_reference(ctx, node, symbol, symbol, XR_XIR_SOURCE_READ_WRITE)) return false;
    SourceValue old, one, result;
    XrXirInstruction read = symbol->kind == SOURCE_LOCAL ?
        (XrXirInstruction) {XR_XIR_CELL_READ, symbol->type, {symbol->index, 0}, {0}, 0, {0}} :
        (XrXirInstruction) {XR_XIR_SLOT_LOAD, symbol->type, {0}, {0}, symbol->index, {0}};
    if (!source_recipe_record(ctx, read, &old) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, symbol->type, {0}, {0}, 1, {0}}, &one) ||
        !source_recipe_record(ctx, (XrXirInstruction) {node->type == AST_INC ? XR_XIR_ADD_INT : XR_XIR_SUB_INT, symbol->type, {old.id, one.id}, {0}, 0, {0}}, &result)) return false;
    XrXirInstruction write = symbol->kind == SOURCE_LOCAL ?
        (XrXirInstruction) {XR_XIR_CELL_WRITE, XR_XIR_UNIT, {symbol->index, result.id}, {0}, 0, {0}} :
        (XrXirInstruction) {XR_XIR_SLOT_STORE, XR_XIR_UNIT, {result.id, 0}, {0}, symbol->index, {0}};
    return source_recipe_record(ctx, write, NULL);
}
static bool source_step(SourceContext *ctx, AstNode *node) {
    if (!node) return true;
    if (!source_work(ctx, node)) return false;
    if (node->type == AST_INC || node->type == AST_DEC) return source_increment(ctx, node);
    SourceValue ignored; return expression(ctx, node, &ignored);
}
static bool check_dead_step(SourceContext *ctx, AstNode *node) {
    if (!node) return true;
    SourceFunction *body = &ctx->bodies[ctx->function];
    SourceFunction saved = *body;
    XrXirGeneric generic = ctx->generics[ctx->function];
    bool returned = ctx->returned;
    bool result = begin_block(ctx) && source_step(ctx, node);
    if (result) result=source_plan_discard(ctx,saved.expression_count);
    saved.recipe_storage=body->recipe_storage;
    saved.expressions=body->expressions;saved.expression_count=body->expression_count;
    *body = saved; ctx->generics[ctx->function] = generic; ctx->returned = returned;
    return result;
}
static bool patch_exits(SourceContext *ctx, SourcePatch *patch, uint32_t target) {
    for (; patch; patch = patch->next) {
        if (!source_work(ctx, NULL)) return false;
        ctx->bodies[ctx->function].recipes[patch->instruction].instruction.targets[0] = target;
    }
    return true;
}
static bool source_loop(SourceContext *ctx, AstNode *node, AstNode *condition_node, AstNode *loop_body, AstNode *step) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    /* The header joins the entry edge and the back edge, so facts about bindings the loop writes are gone. */
    SourceFactSet yes_facts = {0}, no_facts = {0};
    bool has_break;
    if ((condition_node && !source_facts_kill(ctx, condition_node)) || (loop_body && !source_facts_kill(ctx, loop_body)) ||
        (step && !source_facts_kill(ctx, step)) || !source_has_break(ctx, loop_body, &has_break)) return false;
    if (condition_node && (!source_condition_facts(ctx, condition_node, true, &yes_facts, 0) ||
        !source_condition_facts(ctx, condition_node, false, &no_facts, 0))) return false;
    SourceFact *header_facts = ctx->facts;
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t condition_block = body->block_count;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {condition_block, 0}, 0, {0}}, NULL) || !begin_block(ctx)) return false;
    SourceValue condition;
    if (condition_node) { if (!expression(ctx, condition_node, &condition)) return false; }
    else if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_BOOL, XR_XIR_BOOL, {0}, {0}, 1, {0}}, &condition)) return false;
    if (condition.type != XR_XIR_BOOL) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "loop condition requires bool");
    uint32_t branch = body->count, entry = body->block_count;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id, 0}, {entry, 0}, 0, {0}}, NULL) || !begin_block(ctx) ||
        !source_facts_apply(ctx, &yes_facts)) return false;
    SourceLoop loop = {ctx->loop, NULL, NULL, body->frontier}; ctx->loop = &loop;
    bool ok = scoped_statement(ctx, loop_body); ctx->loop = loop.parent;
    if (!ok) return false;
    ctx->facts = header_facts;
    if (!ctx->returned || loop.continues) {
        uint32_t next = step ? body->block_count : condition_block;
        if (!ctx->returned && !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {next, 0}, 0, {0}}, NULL)) return false;
        if (!patch_exits(ctx, loop.continues, next)) return false;
        if (step && (!begin_block(ctx) || !source_step(ctx, step) ||
            !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {condition_block, 0}, 0, {0}}, NULL))) return false;
    } else if (!check_dead_step(ctx, step)) return false;
    uint32_t exit = body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[1] = exit;
    ctx->facts = header_facts;
    if (!has_break && !source_facts_apply(ctx, &no_facts)) return false;
    return patch_exits(ctx, loop.breaks, exit);
}
static bool source_for(SourceContext *ctx, AstNode *node) {
    ForStmtNode *loop = &node->as.for_stmt;
    if (loop->label) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "labelled loops are not admitted");
    SourceName *saved = ctx->locals, *scope = ctx->scope; ctx->scope = saved;
    bool ok = !loop->initializer || statement(ctx, loop->initializer, false);
    if (ok) ok = source_loop(ctx, node, loop->condition, loop->body, loop->increment);
    ctx->locals = saved; ctx->scope = scope; return ok;
}
static bool source_loop_exit(SourceContext *ctx, AstNode *node) {
    bool stop = node->type == AST_BREAK_STMT;
    const char *label = stop ? node->as.break_stmt.label : node->as.continue_stmt.label;
    if (!ctx->loop || label) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE,
        ctx->identities[ctx->function].cleanup_owner ? "E0395: defer cannot exit an enclosing loop outside its body" :
        "loop exit requires an unlabelled enclosing loop");
    SourcePatch **head = stop ? &ctx->loop->breaks : &ctx->loop->continues;
    SourcePatch *patch = source_alloc(ctx, 1, sizeof(*patch)); if (!patch) return false;
    *patch = (SourcePatch) {*head, ctx->bodies[ctx->function].count}; *head = patch;
    ctx->returned = true;
    uint32_t target = ctx->loop->frontier;
    bool leaving = ctx->bodies[ctx->function].frontier != target;
    return source_recipe_record(ctx, (XrXirInstruction) {leaving ? XR_XIR_CLEANUP_LEAVE : XR_XIR_JUMP,
        XR_XIR_UNIT, {0}, {0}, leaving ? target : 0, {0}}, NULL);
}
#include "xxir_source_iteration.inc.c"
#include "xxir_source_catch.inc.c"
static bool source_unreachable_statement(SourceContext *ctx, AstNode *node);
static bool statement(SourceContext *ctx, AstNode *node, bool top) {
    SourceConversionRecipe checked_conversion;
    if (!node || !source_work(ctx, node)) return false;
    if (ctx->returned) return source_unreachable_statement(ctx, node);
    switch (node->type) {
    case AST_DEFER_STMT: return source_defer(ctx, node);
    case AST_TRY_CATCH: {
        /* Handlers are alternative paths: no fact about a binding they write survives, and none is learned inside. */
        ++ctx->flow_alternatives;
        bool tried = source_facts_kill(ctx, node) && source_try(ctx, node);
        --ctx->flow_alternatives;
        return tried && source_facts_kill(ctx, node);
    }
    case AST_IF_STMT: return source_if(ctx, node);
    case AST_WHILE_STMT:
        if (node->as.while_stmt.label) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "labelled loops are not admitted");
        return source_loop(ctx, node, node->as.while_stmt.condition, node->as.while_stmt.body, NULL);
    case AST_FOR_STMT: return source_for(ctx, node);
    case AST_FOR_IN_STMT: {
        ++ctx->flow_alternatives;
        bool iterated = source_facts_kill(ctx, node) && source_for_in(ctx, node);
        --ctx->flow_alternatives;
        return iterated && source_facts_kill(ctx, node);
    }
    case AST_INC: case AST_DEC: return source_increment(ctx, node);
    case AST_BREAK_STMT: case AST_CONTINUE_STMT: return source_loop_exit(ctx, node);
    case AST_FUNCTION_DECL: return top || source_nested_function(ctx, node);
    case AST_TYPE_ALIAS: return true;
    case AST_IMPORT_STMT: case AST_STRUCT_DECL: case AST_CLASS_DECL: case AST_ENUM_DECL: case AST_INTERFACE_DECL:
        return top || source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "nested declarations are not admitted");
    case AST_VAR_DECL: case AST_CONST_DECL: return source_binding(ctx, node, top);
    case AST_EXPR_STMT: {
        SourceValue value; AstNode *expr=node->as.expr_stmt;
        if (expr->type!=AST_MATCH_EXPR) return expression(ctx,expr,&value);
        SourceExpressionPlan *plan=source_plan_collect(ctx,expr,(SourceExpectedType){false,XR_XIR_UNIT, false});
        if (!plan) return false;
        plan->statement_match=true;
        return source_plan_complete(ctx,plan,&value);
    }
    case AST_THROW_STMT: {
        SourceValue value = {0};
        if (!expression(ctx, node->as.throw_stmt.expression, &value)) return false;
        XrXirDeclarations declarations;
        XrXirModule module = source_module_view(ctx,&declarations);
        XrXirProofContext context = {&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
        XrXirStatus status = xr_xir_compile_type_markers_prove(&ctx->compile, &context, value.type, XR_XIR_CONSTRAINT_ERROR);
        if (status != XR_XIR_OK)
            return source_fail(ctx, node, status, "throw requires a proved enum error value");
        if (ctx->bodies[ctx->function].error_context) {
            if (!(source_conversion_plan(ctx,node,(&value)->type,(SourceExpectedType){XR_XIR_ERROR != XR_XIR_UNIT,XR_XIR_ERROR, false},&checked_conversion) && source_conversion_emit(ctx,&checked_conversion,&value)) || !source_error_edge(ctx,value)) return false;
            ctx->returned = true; return true;
        }
        ctx->returned = true;
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_THROW, XR_XIR_UNIT, {value.id, 0}, {0, 0}, 0, {0}}, NULL);
    }
    case AST_RETURN_STMT: {
        if (ctx->identities[ctx->function].cleanup_owner)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "E0395: defer cannot return from its owning function");
        if (source_constructor_active(ctx)) return source_constructor_return(ctx, node);
        if (top || !ctx->bodies[ctx->function].node || node->as.return_stmt.value_count > 1)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "invalid return placement or arity");
        SourceValue value = {0};
        SourceFunction *body = &ctx->bodies[ctx->function];
        XrXirType expected = body->infer_result ? XR_XIR_UNIT : ctx->functions[ctx->function].result;
        if (node->as.return_stmt.value_count && !source_plan_expression(ctx, node->as.return_stmt.values[0], (SourceExpectedType){!body->infer_result,expected, false}, &value)) return false;
        if (body->infer_result && !body->saw_return) ctx->functions[ctx->function].result = value.type;
        body->saw_return = true;
        if (value.type != ctx->functions[ctx->function].result)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "return type does not match declaration");
        ctx->returned = true;
        return source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.type == XR_XIR_UNIT ? 0 : value.id, 0}, {0, 0}, 0, {0}}, NULL);
    }
    case AST_BLOCK: {
        if (ctx->depth >= 128) return source_fail(ctx, node, XR_XIR_BUDGET, "source block depth exhausted");
        SourceName *saved = ctx->locals, *scope = ctx->scope;
        SourceFunction *body = &ctx->bodies[ctx->function]; uint32_t entry = body->frontier;
        ctx->scope = saved; ++ctx->depth; ++body->lexical_depth;
        bool ok = source_block_functions(ctx, node);
        for (int i = 0; ok && i < node->as.block.count; ++i)
            ok = statement(ctx, node->as.block.statements[i], false);
        --ctx->depth; --body->lexical_depth; ctx->locals = saved; ctx->scope = scope;
        if (ok && !ctx->returned && body->frontier != entry) {
            ok = source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CLEANUP_LEAVE, XR_XIR_UNIT, {0}, {body->block_count}, entry, {0}}, NULL);
            body->frontier = entry;
            if (ok) ok = begin_block(ctx);
        } else body->frontier = entry;
        return ok;
    }
    default: return source_fail_syntax(ctx, node, "statement");
    }
}
static bool source_core_result_scope(SourceContext *ctx, AstNode *node, uint32_t index) {
    FunctionDeclNode *decl = &node->as.function_decl;
    if (decl->type_param_count || decl->param_count != 2 || !decl->params || !decl->params[0] || !decl->params[0]->type)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"core action result scope is invalid");
    XrTypeRef *action = decl->params[0]->type;
    XrTypeRef *result = action->kind == XR_TREF_FUNCTION && action->nchildren == 1 && action->children ? action->children[0] : NULL;
    if (!result || result->kind != XR_TREF_NAMED || !result->name || !source_text_same(ctx, NULL, result->name, "R") || result->nchildren)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"core callback result annotation is invalid");
    XrGenericParam **parameters = source_alloc(ctx,1,sizeof(*parameters));
    XrGenericParam *parameter = source_alloc(ctx,1,sizeof(*parameter));
    XrXirConstraint *constraint = source_alloc(ctx,1,sizeof(*constraint));
    uint32_t *kinds = source_alloc(ctx,1,sizeof(*kinds));
    if (!parameters || !parameter || !constraint || !kinds) return false;
    parameter->name = (char *)result->name; parameters[0] = parameter; kinds[0] = XR_XIR_BINDER_RESULT_VARIABLE;
    SourceFunction *body = &ctx->bodies[index];
    body->type_parameters = parameters; body->type_parameter_count = 1; body->generic_owner = body->declaration;
    ctx->generics[index] = (XrXirGeneric){constraint,1,NULL,0,kinds}; ctx->has_generics = true;
    return true;
}
static bool source_test_role(SourceContext *ctx, AstNode *node, XrXirFunctionIdentity *identity) {
    FunctionDeclNode *decl = &node->as.function_decl;
    if (decl->attr_count < 0 || (decl->attr_count && !decl->attributes))
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "function attributes are malformed");
    for (int a = 0; a < decl->attr_count; ++a) {
        if (!source_work(ctx, node)) return false;
        const XrAttribute *attribute = decl->attributes[a];
        if (!attribute || identity->test_role)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "conflicting test attributes");
        switch (attribute->kind) {
        case ATTR_TEST: case ATTR_TEST_TIMEOUT: identity->test_role = XR_XIR_TEST_ROLE_TEST; break;
        case ATTR_TEST_SKIP: identity->test_role = XR_XIR_TEST_ROLE_SKIP; break;
        case ATTR_BEFORE_ALL: identity->test_role = XR_XIR_TEST_ROLE_BEFORE_ALL; break;
        case ATTR_AFTER_ALL: identity->test_role = XR_XIR_TEST_ROLE_AFTER_ALL; break;
        case ATTR_BEFORE_EACH: identity->test_role = XR_XIR_TEST_ROLE_BEFORE_EACH; break;
        case ATTR_AFTER_EACH: identity->test_role = XR_XIR_TEST_ROLE_AFTER_EACH; break;
        default: return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "function attribute is not implemented in XIR");
        }
        if (attribute->timeout < 0 || (attribute->kind != ATTR_TEST_TIMEOUT && attribute->timeout))
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "invalid test timeout");
        identity->test_timeout_seconds = (uint32_t)attribute->timeout;
    }
    if (identity->test_role && (decl->param_count || decl->type_param_count))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "test functions and hooks require no parameters or type parameters");
    return true;
}
static bool declare_function(SourceContext *ctx, AstNode *node, uint32_t index) {
    FunctionDeclNode *decl = &node->as.function_decl;
    if (decl->is_generator || decl->is_extern || decl->type_param_count < 0 || decl->type_param_count > 65536 ||
        decl->throws_count || decl->borrow_origin_count || !decl->body || decl->param_count > 65536 || decl->param_count < 0)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "function contract is not implemented in XIR");
    XrXirFunctionIdentity identity = {.module = ctx->module, .exported = node->is_exported};
    if (!source_test_role(ctx, node, &identity)) return false;
    SourceName *symbol = add_name(ctx, &ctx->names[ctx->module], decl->name, node);
    if (!symbol) return false;
    symbol->kind = SOURCE_FUNCTION; symbol->index = index;
    SourceFunction *body = &ctx->bodies[index]; body->node = body->type_owner = node; body->module = ctx->module;
    ctx->function = index;
    if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_FUNCTION, 0, source_query_range(ctx, node, decl->name))) return false;
    body->declaration = symbol->declaration;
    body->generic_owner = decl->type_param_count ? symbol->declaration : 0;
    body->type_parameters = decl->type_params; body->type_parameter_count = (uint32_t)decl->type_param_count;
    XrXirConstraint *constraints = decl->type_param_count ? source_alloc(ctx, (size_t) decl->type_param_count, sizeof(*constraints)) : NULL;
    if (decl->type_param_count && !constraints) return false;
    ctx->generics[index].constraints = constraints;
    ctx->generics[index].parameter_count = (uint32_t) decl->type_param_count;
    ctx->has_generics |= decl->type_param_count != 0;
    if (decl->type_param_count && !decl->type_params)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"function type parameters are missing");
    for (int i = 0; i < decl->type_param_count; ++i) {
        if (!source_work(ctx,node)) return false;
        if (!decl->type_params[i] || !decl->type_params[i]->name || !*decl->type_params[i]->name)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"function type parameter is malformed");
    }
    for (int i = 0; i < decl->type_param_count; ++i) {
        XrGenericParam *parameter = decl->type_params[i];
        if (!source_parameter_constraints(ctx, node, parameter, &constraints[i])) return false;
        for (int j = 0; j < i; ++j) {
            if (!source_work(ctx, node)) return false;
            if (source_text_same(ctx, NULL, parameter->name, decl->type_params[j]->name))
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "duplicate type parameter");
        }
    }
    if (ctx->core_factory && index == 2 && !source_core_result_scope(ctx,node,index)) return false;
    body->parameters = decl->param_count ? source_alloc(ctx, (size_t) decl->param_count, sizeof(*body->parameters)) : NULL;
    if (decl->param_count && !body->parameters) return false;
    XrXirFunction *function = &ctx->functions[index];
    *function = (XrXirFunction) {decl->name, (uint32_t) source_text_size(ctx, decl->name), body->parameters,
        (uint32_t) decl->param_count, XR_XIR_UNIT, NULL, 0, NULL, 0, NULL, 0};
    if (!source_type(ctx, decl->return_type, &function->result)) return false;
    if (identity.test_role && function->result != XR_XIR_UNIT)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "test functions and hooks cannot return a value");
    for (int i = 0; i < decl->param_count; ++i) {
        XrParamNode *param = decl->params[i];
        bool by_reference = param->passing_mode == XR_PARAM_REF;
        if (!param->type || (param->passing_mode != XR_PARAM_READ && !by_reference) || param->pattern || param->is_rest ||
            (by_reference && param->default_value) ||
            !source_type(ctx, param->type, &body->parameters[i]) || body->parameters[i] == XR_XIR_UNIT)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "parameter contract is not implemented in XIR");
        /* A ref parameter is the caller's mutable cell: reads and writes in the body reach the
         * caller's binding through it, exactly as a captured mutable variable does. */
        if (by_reference && !source_cell_type(ctx, body->parameters[i], &body->parameters[i])) return false;
        for (int j = 0; j < i; ++j) {
            if (!source_work(ctx, node)) return false;
            if (source_text_same(ctx, NULL, param->name, decl->params[j]->name))
                return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "duplicate parameter name");
        }
    }
    ctx->identities[index] = identity;
    return source_query_parameters(ctx, symbol->declaration);
}
static bool source_resolved_identity(SourceContext *ctx, const XrModuleSpec *spec, const XrModuleId *id) {
    return spec->kind == id->kind && spec->authority.kind == id->authority.kind &&
        source_text_same(ctx, NULL, spec->logical_path, id->logical_path) &&
        source_text_same(ctx, NULL, spec->authority.namespace_id, id->authority.namespace_id) &&
        source_text_same(ctx, NULL, spec->authority.physical_root, id->authority.physical_root) &&
        (spec->embedded_source ? !id->source_path : source_text_same(ctx, NULL, spec->source_path, id->source_path));
}
static bool declare_import(SourceContext *ctx, AstNode *node) {
    ImportStmtNode *decl = &node->as.import_stmt;
    XrModuleSpec *spec = &ctx->graph->specs[ctx->module];
    XrModuleId id = {0}; char *error = NULL;
    XrModuleStatus resolved = xr_compile_module_resolver_resolve(ctx->graph->resolver, decl->module_name, spec->source_path, &spec->authority, &id, &error);
    int target = -1;
    if (resolved == XR_MODULE_OK && id.canonical && id.logical_path)
        resolved = xr_compile_module_graph_find(ctx->graph, id.canonical, &target);
    if (target >= 0 && !source_resolved_identity(ctx, &ctx->graph->specs[target], &id)) target = -1;
    xr_compile_resources_free(error); xr_compile_module_id_cleanup(&id);
    if (target < 0) {
        ctx->query_ready = false;
        return resolved != XR_MODULE_OK ?
            source_module_fail(ctx, node, resolved, "import resolution failed") :
            source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "import does not resolve to the parsed graph");
    }
    for (int i = 0; i < (decl->member_count ? decl->member_count : 1); ++i) {
        const char *name = decl->member_count ? (decl->members[i].alias ? decl->members[i].alias : decl->members[i].name) : decl->alias;
        SourceName *symbol = add_name(ctx, &ctx->names[ctx->module], name, node);
        if (!symbol) return false;
        symbol->kind = decl->member_count ? SOURCE_IMPORT : SOURCE_MODULE;
        symbol->module = (uint32_t) target;
        symbol->imported = decl->member_count ? decl->members[i].name : NULL;
        /* Import aliases do not yet have individual parser token locations. */
        if (!source_query_declare(ctx, symbol, decl->member_count ? XR_XIR_SOURCE_IMPORT : XR_XIR_SOURCE_MODULE,
            0, (XrXirSourceRange) {ctx->module, 0, 0, 0, 0})) return false;
    }
    return true;
}
typedef struct SourceClosureCount { SourceContext *ctx; uint32_t count, depth, defaults, members; } SourceClosureCount;
static bool count_closures(AstNode *node, void *pointer) {
    SourceClosureCount *scan = pointer;
    if (!node) return true;
    if (!source_work(scan->ctx,node)) return false;
    /* A module-level declaration is a direct child of the program node, at depth one. */
    bool closure = node->type == AST_FUNCTION_EXPR || node->type == AST_DEFER_STMT ||
        (node->type == AST_FUNCTION_DECL && scan->depth > 1);
    if (scan->depth == 128 || (closure && scan->count == scan->ctx->compile.limits.functions))
        return source_fail(scan->ctx,node,XR_XIR_BUDGET,"closure declaration budget exhausted");
    if (closure) ++scan->count;
    if (node->type == AST_MEMBER_ACCESS) {
        if (scan->members == UINT32_MAX)
            return source_fail(scan->ctx,node,XR_XIR_BUDGET,"member helper capacity exhausted");
        ++scan->members;
    }
    XrParamNode **parameters = node->type == AST_FUNCTION_DECL ? node->as.function_decl.params :
        node->type == AST_METHOD_DECL ? node->as.method_decl.params : NULL;
    int count = node->type == AST_FUNCTION_DECL ? node->as.function_decl.param_count :
        node->type == AST_METHOD_DECL ? node->as.method_decl.param_count : 0;
    for (int p = 0; p < count; ++p) {
        if (!source_work(scan->ctx, node)) return false;
        if (parameters[p]->default_value) {
            if (scan->defaults == scan->ctx->compile.limits.functions)
                return source_fail(scan->ctx, node, XR_XIR_BUDGET, "default argument function budget exhausted");
            ++scan->defaults;
        }
    }
    ++scan->depth;
    bool ok = xr_ast_for_each_child(node,count_closures,scan);
    --scan->depth;
    return ok || source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"unknown source declaration shape");
}
/* Code after return, throw, break or continue never runs and has no control-flow edge, so it is dropped.
 * Closures in it are rejected because their declarations are counted ahead of lowering and must each be lowered. */
static bool source_unreachable_statement(SourceContext *ctx, AstNode *node) {
    SourceClosureCount scan = {ctx, 0, 2, 0, 0};
    if (!count_closures(node, &scan)) return false;
    if (scan.count) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "closures in unreachable statements are not admitted");
    return true;
}
#include "xxir_source_library.inc.c"

static bool collect_declarations(SourceContext *ctx) {
    uint32_t count = (uint32_t) ctx->graph->spec_count, functions = count + (ctx->linkage_kind == XR_XIR_PROGRAM), slots = 0, nominals = 0, interfaces = 0;
    SourceClosureCount closures = {ctx,0,0,0,0};
    for (uint32_t m = 0; m < count; ++m) {
        ctx->module = m;
        AstNode *ast = ctx->graph->specs[m].ast;
        if (ctx->graph->specs[m].representation == XR_MODULE_CHECKED_LIBRARY) {
            const XrXirModule *library=source_library_module(ctx,m);
            if(!library)return false;
            if(library->function_count-1 > ctx->compile.limits.functions ||
                functions > ctx->compile.limits.functions-(library->function_count-1))
                return source_fail(ctx,NULL,XR_XIR_BUDGET,"library function inventory exhausted");
            functions+=library->function_count-1;continue;
        }
        if (!count_closures(ast,&closures)) return false;
        if (!ast || ast->type != AST_PROGRAM) return source_fail(ctx, ast, XR_XIR_BAD_STRUCTURE, "parsed module required");
        for (int i = 0; i < ast->as.program.count; ++i) {
            AstNode *node = ast->as.program.statements[i];
            if (!source_work(ctx, node)) return false;
            if (node->type == AST_FUNCTION_DECL) ++functions;
            if (node->type == AST_INTERFACE_DECL) ++interfaces;
            if (node->type == AST_STRUCT_DECL || node->type == AST_CLASS_DECL || node->type == AST_ENUM_DECL) ++nominals;
            if (node->type == AST_STRUCT_DECL || node->type == AST_CLASS_DECL || node->type == AST_ENUM_DECL) {
                SourceNominalDeclaration declaration;
                if (!source_nominal_declaration(ctx,node,&declaration)) return false;
                if (functions > ctx->compile.limits.functions || declaration.method_count < 0 ||
                    (uint32_t)declaration.method_count > ctx->compile.limits.functions - functions)
                    return source_fail(ctx,node,XR_XIR_BUDGET,"method function budget exhausted");
                functions += (uint32_t)declaration.method_count;
            }
            if (node->type == AST_STRUCT_DECL || node->type == AST_CLASS_DECL) {
                ClassDeclNode *decl = node->type == AST_CLASS_DECL ? &node->as.class_decl : &node->as.struct_decl;
                for (int f = 0; f < decl->field_count; ++f) {
                    if (!source_work(ctx, decl->fields[f])) return false;
                    if (decl->fields[f]->type == AST_FIELD_DECL && decl->fields[f]->as.field_decl.initializer) {
                        if (functions >= ctx->compile.limits.functions)
                            return source_fail(ctx, node, XR_XIR_BUDGET, "field initializer function budget exhausted");
                        ++functions;
                    }
                }
            }
            if (node->type == AST_VAR_DECL || node->type == AST_CONST_DECL) ++slots;
        }
    }
    if (functions > ctx->compile.limits.functions || closures.defaults > ctx->compile.limits.functions - functions)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "default argument function budget exhausted");
    functions += closures.defaults;
    if (functions > ctx->compile.limits.functions || closures.count > ctx->compile.limits.functions - functions)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"function budget exhausted");
    ctx->first_closure = ctx->next_closure = functions - (ctx->linkage_kind == XR_XIR_PROGRAM);
    functions += closures.count;
    ctx->function_count = functions; ctx->slot_count = slots; ctx->module_count = count;
    if (nominals > UINT32_MAX - functions)
        return source_fail(ctx, NULL, XR_XIR_BUDGET, "default constructor capacity exhausted");
    uint32_t capacity = functions + nominals;
    if (closures.members > UINT32_MAX - capacity)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"member helper capacity exhausted");
    capacity += closures.members;
    if (!ctx->core_factory && capacity <= UINT32_MAX - 7) capacity += 7;
    if (capacity > ctx->compile.limits.functions) capacity = ctx->compile.limits.functions;
    ctx->function_capacity = capacity;
    ctx->functions = source_alloc(ctx, capacity, sizeof(*ctx->functions));
    ctx->bodies = source_alloc(ctx, capacity, sizeof(*ctx->bodies));
    ctx->identities = source_alloc(ctx, capacity, sizeof(*ctx->identities));
    ctx->generics = source_alloc(ctx, capacity, sizeof(*ctx->generics));
    ctx->modules = source_alloc(ctx, count, sizeof(*ctx->modules));
    ctx->names = source_alloc(ctx, count, sizeof(*ctx->names));
    ctx->slots = source_alloc(ctx, slots, sizeof(*ctx->slots));
    ctx->nominals.declarations = source_alloc(ctx, nominals, sizeof(*ctx->nominals.declarations));
    ctx->nominal_members = source_alloc(ctx, nominals, sizeof(*ctx->nominal_members));
    ctx->nominal_variants = source_alloc(ctx, nominals, sizeof(*ctx->nominal_variants));
    ctx->nominal_defaults = source_alloc(ctx, nominals, sizeof(*ctx->nominal_defaults));
    ctx->nominal_sources = source_alloc(ctx, nominals, sizeof(*ctx->nominal_sources));
    ctx->nominal_methods = source_alloc(ctx, nominals, sizeof(*ctx->nominal_methods));
    ctx->nominal_defaultable = source_alloc(ctx, nominals, sizeof(*ctx->nominal_defaultable));
    ctx->nominal_constructors = source_alloc(ctx, nominals, sizeof(*ctx->nominal_constructors));
    if (nominals) ctx->types.nominals = &ctx->nominals;
    ctx->interfaces.declarations = interfaces ? source_alloc(ctx,interfaces,sizeof(*ctx->interfaces.declarations)) : NULL;
    ctx->interface_sources = interfaces ? source_alloc(ctx,interfaces,sizeof(*ctx->interface_sources)) : NULL;
    ctx->interface_members = interfaces ? source_alloc(ctx,interfaces,sizeof(*ctx->interface_members)) : NULL;
    ctx->interface_member_declarations = interfaces ? source_alloc(ctx,interfaces,sizeof(*ctx->interface_member_declarations)) : NULL;
    if (interfaces) ctx->types.interfaces = &ctx->interfaces;
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    for (uint32_t m = 0; m < count; ++m) {
        ctx->module = m;
        AstNode *ast = ctx->graph->specs[m].ast;
        if(ctx->graph->specs[m].representation==XR_MODULE_CHECKED_LIBRARY)continue;
        for (int i = 0; i < ast->as.program.count; ++i) {
            AstNode *node = ast->as.program.statements[i];
            if (node->type == AST_INTERFACE_DECL && !source_interface_declare(ctx,node)) return false;
            if (node->type == AST_STRUCT_DECL && !source_struct_declare(ctx, node)) return false;
            if (node->type == AST_CLASS_DECL && !source_class_declare(ctx,node)) return false;
            if (node->type == AST_ENUM_DECL && !source_enum_declare(ctx, node)) return false;
            if (node->type == AST_IMPORT_STMT && !declare_import(ctx, node)) return false;
        }
    }
    if (!source_nominal_constraints(ctx) || !source_interface_constraints(ctx) ||
        !source_interface_signatures(ctx) || !source_struct_fields(ctx) || !source_enum_fields(ctx) ||
        !source_class_field_capabilities(ctx)) return false;
    if (!source_struct_defaultability(ctx)) return false;
    uint32_t function = count, slot = 0;
    for (uint32_t m = 0; m < count; ++m) {
        ctx->module = m;
        XrModuleSpec *spec = &ctx->graph->specs[m];
        uint32_t *deps = source_alloc(ctx, (size_t) spec->dep_count, sizeof(*deps));
        if (!deps) return false;
        for (int i = 0; i < spec->dep_count; ++i) deps[i] = (uint32_t) spec->dep_indices[i];
        ctx->modules[m] = (XrXirSourceModule) {spec->canonical, (uint32_t) source_text_size(ctx, spec->canonical), deps, (uint32_t) spec->dep_count, m};
        ctx->bodies[m].module = m;
        ctx->functions[m] = (XrXirFunction) {"$init", 5, NULL, 0, XR_XIR_UNIT, NULL, 0, NULL, 0, NULL, 0};
        ctx->identities[m].module = m;
        if(spec->representation==XR_MODULE_CHECKED_LIBRARY){
            if(!source_library_install(ctx,m,&function))return false;continue;
        }
        for (int i = 0; i < spec->ast->as.program.count; ++i) {
            AstNode *node = spec->ast->as.program.statements[i];
            if (node->type == AST_FUNCTION_DECL) { if (!declare_function(ctx, node, function++)) return false; }
            else if (node->type == AST_VAR_DECL || node->type == AST_CONST_DECL) {
                if (node->is_exported || (!node->as.var_decl.is_const && (ctx->linkage_kind == XR_XIR_LIBRARY || m != (uint32_t) ctx->graph->entry_index)))
                    return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "library mutable or exported state is not admitted");
                SourceName *symbol = add_name(ctx, &ctx->names[m], node->as.var_decl.name, node);
                if (!symbol) return false;
                symbol->kind = SOURCE_SLOT; symbol->index = slot; symbol->mutable = !node->as.var_decl.is_const;
                if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_BINDING, 0, source_query_range(ctx, node, symbol->name))) return false;
                ctx->slots[slot++] = (XrXirSlot) {m, XR_XIR_UNIT, symbol->mutable};
            }
        }
    }
    if (!source_nominal_methods(ctx, &function) || !source_struct_default_functions(ctx, &function) || !source_struct_constructors(ctx, &function) || !source_argument_functions(ctx, &function) || function != ctx->first_closure)
        return source_fail(ctx, NULL, XR_XIR_BAD_STRUCTURE, "field initializer function inventory mismatch");
    ctx->closure_limit = ctx->function_count - (ctx->linkage_kind == XR_XIR_PROGRAM);
    ctx->entry_function = ctx->linkage_kind == XR_XIR_PROGRAM ? ctx->function_count - 1 : UINT32_MAX;
    for (uint32_t m = 0; m < count; ++m) {
        ctx->module = m;
        for (SourceName *p = ctx->names[m]; p; p = p->next)
            if (p->kind == SOURCE_IMPORT && !imported_declaration(ctx, p, p->imported)) return false;
    }
    return true;
}
static bool source_parameter_promise(SourceContext *ctx, uint32_t function, uint32_t parameter) {
    SourceFunction *body = &ctx->bodies[function];
    AstNode *node = body->node;
    if (parameter >= ctx->functions[function].parameter_count)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "callable promise parameter is missing");
    const XrXirTypeNode *found = xr_xir_callable_signature(&ctx->types, body->parameters[parameter]);
    if (!found || found->flags)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "callable promise target is invalid or duplicated");
    XrXirTypeNode qualified = *found;
    qualified.flags = XR_XIR_CALLABLE_NO_SUSPEND;
    if (!source_intern_type(ctx, qualified, &body->parameters[parameter])) return false;
    const XrXirDefaultBinding *binding = NULL;
    if (!source_default_binding_get(ctx, NULL, function, parameter, &binding)) return false;
    if (binding) ctx->functions[binding->function].result = body->parameters[parameter];
    ctx->function = function; ctx->module = body->module;
    return source_query_parameters(ctx, body->declaration);
}
typedef struct SourceDeclarationSelector {
    XrXirLiteral module, function, owner;
} SourceDeclarationSelector;
typedef struct SourceDeclarationTarget {
    bool requirement;
    uint32_t function, interface, member;
} SourceDeclarationTarget;
static bool source_declaration_target(SourceContext *ctx, const SourceDeclarationSelector *item,
    SourceDeclarationTarget *output) {
    if (!item->module.bytes || !item->module.length || !item->function.bytes ||
        !item->function.length || (item->owner.length && !item->owner.bytes))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"unsupported source declaration promise");
    uint32_t matches = 0;
    for (uint32_t f = 0; f < ctx->function_count; ++f) {
        if (!source_work(ctx,NULL)) return false;
        AstNode *node = ctx->bodies[f].node;
        if (!node || ctx->bodies[f].default_expression) continue;
        uint32_t owner = ctx->identities[f].nominal_owner;
        if (item->owner.length) {
            if (node->type != AST_METHOD_DECL || node->as.method_decl.is_constructor || !owner) continue;
            const XrXirLiteral *name = &ctx->nominals.declarations[owner - 1].name;
            if (name->length != item->owner.length || !source_span_same(ctx, NULL, name->bytes, item->owner.bytes, name->length)) continue;
        } else if (node->type != AST_FUNCTION_DECL || owner) continue;
        const XrXirFunction *function = &ctx->functions[f];
        uint32_t module = ctx->identities[f].module;
        if (ctx->modules[module].name_length != item->module.length ||
            !source_span_same(ctx, NULL, ctx->modules[module].name, item->module.bytes, item->module.length) ||
            item->function.length != function->name_length ||
            !source_span_same(ctx, NULL, function->name, item->function.bytes, function->name_length)) continue;
        *output = (SourceDeclarationTarget){false,f,0,0}; ++matches;
    }
    for (uint32_t d = 0; item->owner.length && d < ctx->interfaces.count; ++d) {
        const XrXirInterfaceDeclaration *declaration = &ctx->interfaces.declarations[d];
        if (!source_work(ctx,ctx->interface_sources[d]->node)) return false;
        if (declaration->module.length != item->module.length || declaration->name.length != item->owner.length ||
            !source_span_same(ctx, NULL, declaration->module.bytes, item->module.bytes, item->module.length) ||
            !source_span_same(ctx, NULL, declaration->name.bytes, item->owner.bytes, item->owner.length)) continue;
        for (uint32_t m = 0; m < declaration->method_count; ++m) {
            const XrXirLiteral *name = &declaration->methods[m].name;
            if (!source_work(ctx,ctx->interface_sources[d]->node)) return false;
            if (name->length != item->function.length || !source_span_same(ctx, NULL, name->bytes, item->function.bytes, name->length)) continue;
            *output = (SourceDeclarationTarget){true,0,d,m}; ++matches;
        }
    }
    return matches == 1 || source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,
        matches ? "ambiguous source declaration promise" : "source declaration promise target is missing");
}
#include "xxir_source_manifest.inc.c"
#include "xxir_source_implements.inc.c"
static bool finish_body(SourceContext *ctx) {
    XrXirFunction *function = &ctx->functions[ctx->function];
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!ctx->returned) {
        if (function->result != XR_XIR_UNIT) return source_fail(ctx, body->node, XR_XIR_BAD_TYPE, "value function requires a return");
        if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0}}, NULL)) return false;
    }
    return source_region_emit(ctx,function,true);
}
typedef struct SourceCapture {
    struct SourceCapture *next;
    SourceName *source;
    uint32_t index;
} SourceCapture;
typedef struct SourceCaptureScan {
    SourceContext *ctx;
    SourceName *bound;
    SourceCapture *captures;
    uint32_t count, depth;
    /* A nested function declaration is its own root: its name is bound inside the
     * body, and `self_used` records whether the body refers to it. */
    AstNode *root;
    SourceName *self;
    bool self_used;
} SourceCaptureScan;
static bool capture_bind(SourceCaptureScan *scan, const char *name, AstNode *node) {
    SourceName *binding = source_alloc(scan->ctx, 1, sizeof(*binding));
    if (!binding) return false;
    binding->name = name; binding->node = node; binding->next = scan->bound; scan->bound = binding;
    return true;
}
static bool capture_name(SourceCaptureScan *scan, const char *name, AstNode *node) {
    SourceContext *ctx = scan->ctx;
    SourceName *bound = find_name(ctx, scan->bound, name);
    if (bound) {
        if (bound == scan->self) scan->self_used = true;
        return true;
    }
    SourceName *source = visible_name(ctx, name);
    if (source && source->kind == SOURCE_PENDING)
        return source_fail(ctx, node, XR_XIR_BAD_VALUE, "nested function is used before its declaration completes");
    if (!source_local_name(source)) return ctx->diagnostic.status == XR_XIR_OK;
    for (SourceCapture *p = scan->captures; p; p = p->next) {
        if (!source_work(ctx, node)) return false;
        if (p->source == source) return true;
    }
    SourceCapture *capture = source_alloc(ctx, 1, sizeof(*capture));
    if (!capture) return false;
    *capture = (SourceCapture) {scan->captures, source,
        source->kind == SOURCE_UNIT_LOCAL ? UINT32_MAX : scan->count++}; scan->captures = capture;
    return true;
}
static bool capture_scan(AstNode *node, void *pointer);
static bool capture_scope(SourceCaptureScan *scan, AstNode *node) {
    SourceName *saved = scan->bound;
    bool ok = capture_scan(node, scan); scan->bound = saved; return ok;
}
static bool capture_pattern(SourceCaptureScan *scan, AstNode *node, uint32_t depth) {
    if (depth>=128) return source_fail(scan->ctx,node,XR_XIR_BUDGET,"pattern capture depth exhausted");
    if (!node || !source_work(scan->ctx,node)) return false;
    if (node->type==AST_PATTERN_MULTI) {
        if (node->as.pattern_multi.count<=0) return source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"empty multi-pattern");
        return capture_pattern(scan,node->as.pattern_multi.patterns[0],depth+1);
    }
    if (source_pattern_binding(node)) return capture_bind(scan,node->as.pattern_literal.value->as.variable.name,node);
    if (node->type==AST_PATTERN_ADT) {
        for (int i=0;i<node->as.pattern_adt.count;++i)
            if (!capture_pattern(scan,node->as.pattern_adt.patterns[i],depth+1)) return false;
    }
    return true;
}
static bool capture_children(SourceCaptureScan *scan, AstNode *node) {
    switch (node->type) {
    case AST_MATCH_EXPR: {
        if (!capture_scan(node->as.match_expr.expr,scan)) return false;
        SourceName *saved=scan->bound;
        for (int i=0;i<node->as.match_expr.arm_count;++i) {
            MatchArmNode *arm=&node->as.match_expr.arms[i]->as.match_arm;
            scan->bound=saved;
            if (!capture_pattern(scan,arm->pattern,0) || !capture_scan(arm->guard,scan) || !capture_scan(arm->body,scan)) return false;
        }
        scan->bound=saved; return true;
    }
    case AST_TRY_CATCH: {
        TryCatchNode *attempt = &node->as.try_catch;
        if (!capture_scope(scan,attempt->try_body)) return false;
        for (int i = 0; i < attempt->catch_count; ++i) {
            XrCatchClause *clause = attempt->catch_clauses[i]; SourceName *saved = scan->bound;
            if (clause->var_name && !capture_bind(scan,clause->var_name,node)) return false;
            if (!capture_scope(scan,clause->body)) return false;
            scan->bound = saved;
        }
        return true;
    }
    case AST_THIS_EXPR: return capture_name(scan, "this", node);
    case AST_VARIABLE: return capture_name(scan, node->as.variable.name, node);
    case AST_INC: return capture_name(scan, node->as.inc.name, node);
    case AST_DEC: return capture_name(scan, node->as.dec.name, node);
    case AST_ASSIGNMENT:
        if (!capture_name(scan, node->as.assignment.name, node)) return false;
        break;
    case AST_COMPOUND_ASSIGNMENT:
        if (!node->as.compound_assignment.object && node->as.compound_assignment.name &&
            !capture_name(scan, node->as.compound_assignment.name, node)) return false;
        break;
    case AST_VAR_DECL: case AST_CONST_DECL:
        return capture_scan(node->as.var_decl.initializer, scan) && capture_bind(scan, node->as.var_decl.name, node);
    case AST_BLOCK: {
        SourceName *saved = scan->bound;
        for (int i = 0; i < node->as.block.count; ++i)
            if (!capture_scan(node->as.block.statements[i], scan)) return false;
        scan->bound = saved; return true;
    }
    case AST_FUNCTION_EXPR: case AST_FUNCTION_DECL: {
        SourceName *saved = scan->bound;
        bool declaration = node->type == AST_FUNCTION_DECL;
        if (declaration) {
            if (!capture_bind(scan, node->as.function_decl.name, node)) return false;
            if (node == scan->root) scan->self = scan->bound;
        }
        for (int i = 0; i < node->as.function_expr.param_count; ++i)
            if (!capture_bind(scan, node->as.function_expr.params[i]->name, node)) return false;
        bool ok = capture_scan(node->as.function_expr.body, scan); scan->bound = saved;
        /* A nested declaration stays visible to the rest of its enclosing block. */
        if (ok && declaration && node != scan->root) ok = capture_bind(scan, node->as.function_decl.name, node);
        return ok;
    }
    case AST_IF_STMT:
        return capture_scan(node->as.if_stmt.condition, scan) && capture_scope(scan, node->as.if_stmt.then_branch) &&
            capture_scope(scan, node->as.if_stmt.else_branch);
    case AST_WHILE_STMT:
        return capture_scan(node->as.while_stmt.condition, scan) && capture_scope(scan, node->as.while_stmt.body);
    case AST_FOR_IN_STMT: {
        ForInStmtNode *loop = &node->as.for_in_stmt;
        if (!capture_scan(loop->collection, scan)) return false;
        SourceName *saved = scan->bound;
        bool ok = (source_text_same(scan->ctx, node, loop->item_name, "_") || capture_bind(scan,loop->item_name,node)) &&
            (!loop->is_keyvalue || source_text_same(scan->ctx, node, loop->value_name, "_") || capture_bind(scan,loop->value_name,node)) &&
            capture_scope(scan,loop->body);
        scan->bound = saved; return ok;
    }
    case AST_FOR_STMT: {
        SourceName *saved = scan->bound;
        ForStmtNode *loop = &node->as.for_stmt;
        bool ok = capture_scan(loop->initializer, scan) && capture_scan(loop->condition, scan) &&
            capture_scope(scan, loop->body) && capture_scope(scan, loop->increment);
        scan->bound = saved; return ok;
    }
    default: break;
    }
    return xr_ast_for_each_child(node, capture_scan, scan) ||
        source_fail(scan->ctx, node, XR_XIR_BAD_STRUCTURE, "unknown closure syntax");
}
static bool capture_scan(AstNode *node, void *pointer) {
    SourceCaptureScan *scan = pointer;
    if (!node) return true;
    if (!source_work(scan->ctx, node)) return false;
    if (scan->depth == 128) return source_fail(scan->ctx, node, XR_XIR_BUDGET, "capture scan depth exhausted");
    ++scan->depth;
    bool ok = capture_children(scan, node); --scan->depth; return ok;
}
static bool source_capture_parameters(SourceContext *ctx, AstNode *node, const SourceCaptureScan *scan) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    for (SourceCapture *p = scan->captures; p; p = p->next) {
        SourceName *symbol = add_name(ctx, &ctx->locals, p->source->name, node);
        if (!symbol) return false;
        symbol->kind = p->source->kind; symbol->index = p->index; symbol->type = p->source->type;
        symbol->mutable = p->source->mutable;
        symbol->declaration = p->source->declaration;
        if (symbol->kind == SOURCE_UNIT_LOCAL) continue;
        if (p->source->construction && ctx->identities[ctx->function].cleanup_owner) {
            symbol->construction = true; body->constructor_captured = true; body->constructor_shared = true;
            uint32_t count = xr_xir_type_node(&ctx->types, symbol->type)->nominal.field_count;
            body->constructor_places = count ? source_alloc(ctx, count, sizeof(*body->constructor_places)) : NULL;
            if (count && !body->constructor_places) return false;
            for (uint32_t f = 0; f < count; ++f) {
                body->constructor_places[f] = p->index + f;
                if (!source_constructor_storage_type(ctx, symbol->type, f, &body->parameters[p->index + f])) return false;
            }
            continue;
        }
        XrXirType type = symbol->type;
        if (symbol->mutable && !source_cell_type(ctx, type, &type)) return false;
        body->parameters[p->index] = type;
    }
    return true;
}
static bool closure_parameters(SourceContext *ctx, AstNode *node, const SourceCaptureScan *scan,
    XrXirCallableParameter *parameters, const XrXirTypeNode *context) {
    if (!source_capture_parameters(ctx, node, scan)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    FunctionDeclNode *decl = &node->as.function_expr;
    for (int i = 0; i < decl->param_count; ++i) {
        XrParamNode *param = decl->params[i];
        XrXirType type;
        if (!source_work(ctx, node)) return false;
        if (param->passing_mode != XR_PARAM_READ || param->default_value || param->pattern || param->is_rest ||
            (!param->type && !context))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "closure parameter requires an annotation or complete callable context");
        if (param->type) {
            if (!source_type(ctx, param->type, &type)) return false;
        } else type = context->parameters[i].type;
        if (type == XR_XIR_UNIT)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "closure parameter cannot be unit");
        SourceName *symbol = add_name(ctx, &ctx->locals, param->name, node);
        if (!symbol) return false;
        symbol->kind = SOURCE_LOCAL; symbol->index = scan->count + (uint32_t) i; symbol->type = type;
        XrXirSourceRange range = {ctx->module, param->line, param->column, param->line, 0};
        if (param->column > 0 && source_text_size(ctx, param->name) <= (size_t) (INT_MAX - param->column))
            range.end_column = param->column + (int) source_text_size(ctx, param->name);
        if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_PARAMETER, body->declaration, range)) return false;
        source_query_binding_type(ctx, symbol);
        body->parameters[symbol->index] = type;
        parameters[i] = (XrXirCallableParameter) {type, XR_PARAM_READ};
    }
    return true;
}
#include "xxir_source_cleanup.inc.c"
/* Inside its own body a nested declaration is the closure value rebuilt from the
 * body's captures, so recursion needs no extra capture and no cell. */
static bool source_closure_self(SourceContext *ctx, AstNode *node, const SourceCaptureScan *scan,
    uint32_t index, XrXirType type) {
    SourceFunction *body = &ctx->bodies[index];
    for (const SourceCapture *p = scan->captures; p; p = p->next) {
        if (!source_work(ctx, node)) return false;
        if (p->source->construction)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "recursive nested function cannot capture a constructor value");
    }
    SourceValue *captures = scan->count ? source_alloc(ctx, scan->count, sizeof(*captures)) : NULL;
    uint32_t count = ctx->generics[index].parameter_count;
    XrXirType *types = count ? source_alloc(ctx, count, sizeof(*types)) : NULL;
    SourceName *symbol = source_alloc(ctx, 1, sizeof(*symbol));
    if ((scan->count && !captures) || (count && !types) || !symbol) return false;
    for (uint32_t i = 0; i < scan->count; ++i) captures[i] = (SourceValue) {i, body->parameters[i]};
    for (uint32_t i = 0; i < count; ++i) types[i] = (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + i);
    XrXirInstruction op = {XR_XIR_FUNCTION_REF, type, {0}, {0}, index, {0}};
    SourceValue self;
    if (!source_type_arguments(ctx, node, types, count, &op) || !source_recipe_group(ctx, op, captures, scan->count, &self))
        return false;
    *symbol = (SourceName) {ctx->locals, node->as.function_decl.name, NULL, node, SOURCE_LOCAL, self.id,
        ctx->module, type, false, false, body->declaration};
    ctx->locals = symbol;
    return true;
}
static bool source_closure(SourceContext *ctx, AstNode *node, SourceExpectedType expected, SourceValue *value) {
    const XrXirTypeNode *context = expected.present ? xr_xir_callable_signature(&ctx->types, expected.type) : NULL;
    if (expected.present && !context)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"expression cannot satisfy its declared type");
    /* The node pool can grow while parameter annotations and the body are built.
     * Callable parameter storage is source-arena owned and does not relocate. */
    XrXirTypeNode context_copy = {0};
    if (context) { context_copy = *context; context = &context_copy; }
    bool no_suspend = context && (context->flags & XR_XIR_CALLABLE_NO_SUSPEND);
    FunctionDeclNode *decl = &node->as.function_expr;
    if (decl->is_generator || decl->is_extern || decl->attr_count || decl->type_param_count || decl->throws_count ||
        decl->borrow_origin_count || !decl->body || decl->param_count < 0 || decl->param_count > 65536)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "closure contract is not implemented in XIR");
    if (context) {
        if (context->parameter_count != (uint32_t) decl->param_count)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "closure arity differs from its callable context");
        for (uint32_t p = 0; p < context->parameter_count; ++p) {
            if (!source_work(ctx, node)) return false;
            if (context->parameters[p].mode != XR_PARAM_READ)
                return source_fail(ctx, node, XR_XIR_BAD_TYPE, "closure context requires an unsupported parameter mode");
        }
    }
    SourceCaptureScan scan = {ctx, NULL, NULL, 0, 0, node, NULL, false};
    if (!capture_scan(node, &scan)) return false;
    if (scan.count > 65536 - (uint32_t) decl->param_count || ctx->next_closure >= ctx->closure_limit)
        return source_fail(ctx, node, XR_XIR_BUDGET, "closure parameter or declaration budget exhausted");
    uint32_t outer = ctx->function, index = ctx->next_closure++;
    SourceFunction *body = &ctx->bodies[index];
    body->node = node; body->type_owner = ctx->bodies[outer].type_owner; body->module = ctx->module;
    body->generic_owner = ctx->bodies[outer].generic_owner;
    body->type_parameters = ctx->bodies[outer].type_parameters;
    body->type_parameter_count = ctx->bodies[outer].type_parameter_count;
    body->infer_result = node->type == AST_FUNCTION_EXPR && !decl->return_type && (!context || expected.infer_result);
    ctx->generics[index].parameter_count = ctx->generics[outer].parameter_count;
    ctx->generics[index].constraints = ctx->generics[outer].constraints;
    ctx->generics[index].parameter_kinds = ctx->generics[outer].parameter_kinds;
    char *name = source_alloc(ctx, 32, sizeof(*name));
    uint32_t parameter_count = scan.count + (uint32_t)decl->param_count;
    body->parameters = parameter_count ? source_alloc(ctx,parameter_count,sizeof(*body->parameters)) : NULL;
    XrXirCallableParameter *parameters = decl->param_count ? source_alloc(ctx, (uint32_t) decl->param_count, sizeof(*parameters)) : NULL;
    SourceValue *captures = source_alloc(ctx, scan.count, sizeof(*captures));
    if (!name || (parameter_count && !body->parameters) || (decl->param_count && !parameters) || !captures) return false;
    if (source_format(ctx, name, 32, "$closure%u", index) != XR_DIAG_OK)
        return source_fail(ctx, node, XR_XIR_BUDGET, "closure name formatting exhausted");
    SourceName declaration = {0}; declaration.name = name; declaration.node = node;
    if (!source_query_declare(ctx, &declaration, XR_XIR_SOURCE_FUNCTION, ctx->bodies[outer].declaration,
        source_query_range(ctx, node, NULL))) return false;
    body->declaration = declaration.declaration;
    ctx->functions[index] = (XrXirFunction) {name, (uint32_t) source_text_size(ctx, name), body->parameters,
        scan.count + (uint32_t) decl->param_count, XR_XIR_UNIT, NULL, 0, NULL, 0, NULL, 0};
    ctx->identities[index].module = ctx->module;
    ctx->identities[index].nominal_owner = ctx->identities[outer].nominal_owner;
    ctx->identities[index].method_kind = ctx->identities[index].nominal_owner ? XR_XIR_MEMBER_HELPER : XR_XIR_NON_MEMBER;
    ctx->identities[index].promises = no_suspend ? XR_XIR_FUNCTION_NO_SUSPEND : 0;
    SourceName *locals = ctx->locals, *scope = ctx->scope;
    SourceLoop *loop = ctx->loop; bool returned = ctx->returned;
    SourceFact *facts = ctx->facts; uint32_t alternatives = ctx->flow_alternatives;
    ctx->function = index; ctx->locals = ctx->scope = NULL; ctx->loop = NULL; ctx->returned = false;
    ctx->facts = NULL; ctx->flow_alternatives = 0;
    XrXirType type = XR_XIR_UNIT;
    bool ok = closure_parameters(ctx, node, &scan, parameters, context);
    if (ok) {
        if (decl->return_type) ok = source_type(ctx, decl->return_type, &ctx->functions[index].result);
        else if (context && !expected.infer_result) ctx->functions[index].result = context->result;
    }
    /* A declaration states its result, so its signature is known before the body
     * and the body can name the function it belongs to. */
    bool nested_function = node->type == AST_FUNCTION_DECL;
    if (ok && nested_function) {
        ok = source_signature(ctx, parameters, (uint32_t) decl->param_count, ctx->functions[index].result, &type);
        if (ok && scan.self_used) ok = source_closure_self(ctx, node, &scan, index, type);
    }
    ok = ok &&
        statement(ctx, decl->body, false) && finish_body(ctx) &&
        (nested_function || source_signature(ctx, parameters, (uint32_t) decl->param_count, ctx->functions[index].result, &type));
    if (ok && no_suspend) {
        XrXirTypeNode signature = *xr_xir_callable_signature(&ctx->types, type);
        signature.flags = XR_XIR_CALLABLE_NO_SUSPEND;
        ok = source_intern_type(ctx, signature, &type);
    }
    if (ok) {
        XrXirSourceType *query_parameters = decl->param_count ? source_alloc(ctx, (size_t) decl->param_count, sizeof(*query_parameters)) : NULL;
        ok = !decl->param_count || query_parameters;
        for (int i = 0; ok && i < decl->param_count; ++i) {
            ok = source_work(ctx, node);
            query_parameters[i] = source_query_type(ctx, parameters[i].type);
        }
        XrXirSourceDeclaration *query_declaration = (XrXirSourceDeclaration *) &ctx->query.declarations[body->declaration - 1];
        query_declaration->type = source_query_type(ctx, ctx->functions[index].result);
        query_declaration->parameters = query_parameters; query_declaration->parameter_count = (uint32_t) decl->param_count;
        query_declaration->generic_parent = body->generic_owner;
        query_declaration->generic_parent_count = body->type_parameter_count;
        query_declaration->generic_parameter_count = body->type_parameter_count;
        query_declaration->generic_constraints = ctx->generics[index].constraints;
        query_declaration->type_parameter_kinds = ctx->generics[index].parameter_kinds;
    }
    ctx->function = outer; ctx->locals = locals; ctx->scope = scope; ctx->loop = loop; ctx->returned = returned;
    ctx->facts = facts; ctx->flow_alternatives = alternatives;
    if (!ok) return false;
    for (SourceCapture *p = scan.captures; p; p = p->next) {
        if (p->source->kind == SOURCE_UNIT_LOCAL) continue;
        XrXirType capture_type = p->source->type;
        if (p->source->mutable && !source_cell_type(ctx, capture_type, &capture_type)) return false;
        if (p->source->construction) {
            if (xr_xir_type_is_class(&ctx->types,p->source->type)) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"class this cannot be captured during construction");
            if (!source_constructor_value(ctx, node, &captures[p->index])) return false;
        } else captures[p->index] = (SourceValue) {p->source->index, capture_type};
    }
    XrXirInstruction op = {XR_XIR_FUNCTION_REF, type, {0}, {0}, index, {0}};
    uint32_t count = ctx->generics[index].parameter_count;
    XrXirType *types = count ? source_alloc(ctx, count, sizeof(*types)) : NULL;
    if (count && !types) return false;
    for (uint32_t i = 0; i < count; ++i) types[i] = (XrXirType) (XR_XIR_TYPE_PARAMETER_BASE + i);
    return source_type_arguments(ctx, node, types, count, &op) && source_recipe_group(ctx, op, captures, scan.count, value);
}
#include "xxir_source_array_recipes.inc.c"
#include "xxir_source_core_panics.inc.c"
static bool build_bodies(SourceContext *ctx) {
    uint32_t count = (uint32_t) ctx->graph->spec_count;
    for (int t = 0; t < ctx->graph->topo_count; ++t) {
        ctx->module = (uint32_t) ctx->graph->topo_order[t]; ctx->function = ctx->module;
        ctx->locals = ctx->scope = NULL; ctx->returned = false; ctx->facts = NULL; ctx->flow_alternatives = 0;
        AstNode *ast = ctx->graph->specs[ctx->module].ast;
        if(ctx->graph->specs[ctx->module].representation==XR_MODULE_CHECKED_LIBRARY)continue;
        for (int i = 0; i < ast->as.program.count; ++i) if (!statement(ctx, ast->as.program.statements[i], true)) return false;
        if (!finish_body(ctx)) return false;
    }
    for (uint32_t f = count; f < ctx->first_closure; ++f) {
        if(ctx->bodies[f].checked_library)continue;
        ctx->function = f; ctx->module = ctx->bodies[f].module; ctx->returned = false;
        ctx->locals = ctx->scope = NULL; ctx->facts = NULL; ctx->flow_alternatives = 0;
        if (ctx->bodies[f].node->type == AST_STRUCT_DECL || ctx->bodies[f].node->type == AST_CLASS_DECL) {
            if (!source_struct_constructor_body(ctx) || !finish_body(ctx)) return false;
            continue;
        }
        if (ctx->bodies[f].default_expression || ctx->bodies[f].node->type == AST_FIELD_DECL) {
            SourceValue value;
            if (!source_plan_expression(ctx, ctx->bodies[f].default_expression ? ctx->bodies[f].default_expression :
                ctx->bodies[f].node->as.field_decl.initializer, (SourceExpectedType){ctx->functions[f].result != XR_XIR_UNIT,ctx->functions[f].result, false}, &value) ||
                !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {value.id, 0}, {0}, 0, {0}}, NULL)) return false;
            ctx->returned = true;
            if (!finish_body(ctx)) return false;
            continue;
        }
        if (ctx->bodies[f].node->type == AST_METHOD_DECL) {
            if (!(ctx->bodies[f].node->as.method_decl.is_constructor ? source_constructor_body(ctx) : source_method_body(ctx)) || !finish_body(ctx)) return false;
            continue;
        }
        FunctionDeclNode *decl = &ctx->bodies[f].node->as.function_decl;
        for (int i = 0; i < decl->param_count; ++i) {
            SourceName *symbol = add_name(ctx, &ctx->locals, decl->params[i]->name, ctx->bodies[f].node);
            if (!symbol) return false;
            symbol->kind = SOURCE_LOCAL; symbol->index = (uint32_t) i; symbol->type = ctx->bodies[f].parameters[i];
            XrParamNode *param = decl->params[i];
            if (param->passing_mode == XR_PARAM_REF) {
                symbol->type = xr_xir_cell_element(&ctx->types, symbol->type);
                symbol->mutable = true;
            }
            XrXirSourceRange range = {ctx->module, param->line, param->column, param->line, 0};
            if (param->column > 0 && source_text_size(ctx, param->name) <= (size_t) (INT_MAX - param->column))
                range.end_column = param->column + (int) source_text_size(ctx, param->name);
            if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_PARAMETER, ctx->bodies[f].declaration, range)) return false;
            source_query_binding_type(ctx, symbol);
        }
        if (ctx->core_factory && f == 1) {
            if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_ASSERT_CONDITION,
                    XR_XIR_UNIT,{0,1},{0},0,{0}}, NULL) || !finish_body(ctx)) return false;
        } else if (ctx->core_factory && f == 2) {
            if (!source_core_panics_body(ctx) || !finish_body(ctx)) return false;
        } else if (ctx->core_factory && f == 3) {
            SourceValue condition;
            if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_EQUAL,XR_XIR_BOOL,{0,1},{0},0,{0}},&condition) ||
                !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ASSERT_CONDITION,XR_XIR_UNIT,{condition.id,2},{0},0,{0}},NULL) ||
                !finish_body(ctx)) return false;
        } else if (!statement(ctx, decl->body, false) || !finish_body(ctx)) return false;
    }
    if (ctx->next_closure != ctx->closure_limit)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"closure declaration was not checked");
    if (ctx->linkage_kind == XR_XIR_LIBRARY) return true;
    ctx->function = ctx->entry_function; ctx->module = (uint32_t) ctx->graph->entry_index;
    ctx->bodies[ctx->function].module = ctx->module;
    ctx->identities[ctx->function].module = ctx->module;
    ctx->functions[ctx->function] = (XrXirFunction) {"$entry", 6, NULL, 0, XR_XIR_I64,
        NULL, 0, NULL, 0, NULL, 0};
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, 0, {0}}, NULL) ||
        !source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_RETURN, XR_XIR_UNIT, {0, 0}, {0, 0}, 0, {0}}, NULL)) return false;
    ctx->returned = true;
    return finish_body(ctx);
}
static bool source_query_modules(SourceContext *ctx) {
    uint32_t count = (uint32_t) ctx->graph->spec_count;
    XrXirSourceQueryModule *modules = source_alloc(ctx, count, sizeof(*modules));
    if (count && !modules) return false;
    ctx->query.modules = modules; ctx->query.module_count = count;
    ctx->query_module_capacity = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!source_work(ctx, NULL)) return false;
        const XrModuleSpec *module = &ctx->graph->specs[i];
        modules[i] = (XrXirSourceQueryModule) {module->canonical, module->source_path, module->source_content_fingerprint};
    }
    return true;
}
static void source_query_publish(SourceContext *ctx, XrXirSourceResult *output) {
    if (!output || !ctx->query_ready || ctx->diagnostic.status != XR_XIR_OK) {
        if (output) xr_xir_compile_source_result_free(output);
        return;
    }
    ctx->query.complete = ctx->diagnostic.status == XR_XIR_OK;
    ctx->query.implementations = ctx->implementations_ready && ctx->implementations.count ? &ctx->implementations : NULL;
    ctx->query.diagnostic = ctx->diagnostic;
    ctx->query.types = ctx->types.count || ctx->types.nominals || ctx->types.interfaces ? &ctx->types : NULL;
    XrXirStatus status = xr_xir_compile_source_snapshot_copy(&ctx->compile, &ctx->query, &output->snapshot);
    if (status != XR_XIR_OK) {
        xr_xir_compile_source_result_free(output); memset(&ctx->diagnostic, 0, sizeof(ctx->diagnostic));
        source_fail(ctx, NULL, status, "source query snapshot publication failed");
    }
}
#include "xxir_core_source.inc.c"
XrXirStatus xr_xir_compile_source_check(const XrXirSourceRequest *request,
    XrXirSourceResult *output, XrXirSourceDiagnostic *diagnostic, char **failure_path) {
    SourceContext ctx = {0};
    XrXirSourceResult result = {0};
    XrModuleResolver *resolver = NULL;
    char *error = NULL;
    bool semantic_started = false;
    if (!request || !xir_compile_context_valid(request->context) || !request->session ||
        !request->entry_path || !request->authority || !output || output->checked || output->snapshot ||
        (failure_path && *failure_path)) {
        source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, "source request is incomplete"); goto done;
    }
    ctx.compile = *request->context;
    ctx.remaining_blocks = ctx.compile.limits.blocks;
    ctx.remaining_instructions = ctx.compile.limits.instructions;
    XrOsIoPolicy policy = xr_compile_io_policy(ctx.compile.resources);
    if (xr_compile_session_resources(request->session) != ctx.compile.resources ||
        (request->lockfile && !xr_lockfile_uses_policy(request->lockfile, &policy)) ||
        (request->libraries && xr_xir_compile_library_catalog_context(request->libraries)->resources != ctx.compile.resources)) {
        source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, "source request has a foreign resource owner"); goto done;
    }
    XrXirStatus resource_status = xir_compile_resource_status(xr_compile_session_resource_status(request->session));
    if (resource_status != XR_XIR_OK) {
        source_fail(&ctx, NULL, resource_status, "source session resources are unavailable"); goto done;
    }
    if (request->linkage_kind != XR_XIR_PROGRAM && request->linkage_kind != XR_XIR_LIBRARY) {
        source_fail(&ctx,NULL,XR_XIR_BAD_STRUCTURE,"source linkage kind is invalid"); goto done;
    }
    ctx.linkage_kind = request->linkage_kind;
    XrModuleResolverConfig config = {request->stdlib_path, request->lockfile, request->libraries};
    XrModuleStatus module_status = xr_compile_module_resolver_new(ctx.compile.resources, &config, &resolver);
    if (module_status == XR_MODULE_OK)
        module_status = xr_compile_module_graph_new(ctx.compile.resources, request->session, resolver, &ctx.graph);
    if (module_status != XR_MODULE_OK) {
        source_module_fail(&ctx, NULL, module_status, "module graph allocation failed"); goto done;
    }
    module_status = xr_compile_module_graph_build(ctx.graph, request->entry_path, request->authority, &error);
    if (module_status != XR_MODULE_OK) {
        source_module_fail(&ctx, NULL, module_status, error ? error : "module graph build failed"); goto done;
    }
    if (!source_manifests_load(&ctx)) goto done;
    module_status = xr_compile_module_graph_topological_sort(ctx.graph);
    if (module_status != XR_MODULE_OK) {
        source_module_fail(&ctx, NULL, module_status, "module graph ordering failed"); goto done;
    }
    if (ctx.graph->has_cycle || ctx.graph->entry_index < 0) {
        source_fail(&ctx, NULL, XR_XIR_BAD_STRUCTURE, error ? error : "module graph is not an acyclic source closure"); goto done;
    }
    semantic_started = true;
    source_construct(&ctx, &result);
done:
    source_query_publish(&ctx, &result);
    if (failure_path && semantic_started && ctx.diagnostic.status != XR_XIR_OK &&
        ctx.diagnostic.line > 0 && ctx.graph &&
        ctx.diagnostic.module < (uint32_t)ctx.graph->spec_count) {
        XrModuleSpec *module = &ctx.graph->specs[ctx.diagnostic.module];
        *failure_path = module->source_path;
        module->source_path = NULL;
    }
    xr_compile_resources_free(error);
    for (SourceManifest *manifest = ctx.manifests; manifest; manifest = manifest->next)
        xr_compile_declaration_manifest_free(manifest->declarations);
    while (ctx.memory) { SourceMemory *next = ctx.memory->next; xr_compile_resources_free(ctx.memory); ctx.memory = next; }
    xr_compile_module_graph_free(ctx.graph); xr_compile_module_resolver_free(resolver);
    if (ctx.diagnostic.status == XR_XIR_OK) *output = result;
    else xr_xir_compile_source_result_free(&result);
    if (diagnostic) *diagnostic = ctx.diagnostic;
    return ctx.diagnostic.status;
}
