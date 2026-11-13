/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_lexical.h - Owned lexical bindings and their actual program boundaries
 */
#ifndef XXIR_LEXICAL_H
#define XXIR_LEXICAL_H
#include "xxir.h"
#include "../shared/xr_param_mode.h"

typedef struct XirLexicalBindingKey {
    /* function is the receiving physical function. The other declaration
     * coordinates retain their source identity through specialization. */
    uint32_t module, declaration, node, parent_declaration, function, storage;
} XirLexicalBindingKey;

/* Scope and binding indices belong to one physical function. Re-entering the
 * same loop scope creates another runtime instance of that same declaration. */
typedef struct XirLexicalScope {
    uint32_t parent, node;
} XirLexicalScope;

typedef enum XirLexicalBindingRole {
    XIR_LEXICAL_LOCAL,
    XIR_LEXICAL_PARAMETER_READ,
    XIR_LEXICAL_PARAMETER_REF,
    XIR_LEXICAL_PARAMETER_MOVE,
    XIR_LEXICAL_CAPTURE
} XirLexicalBindingRole;

typedef struct XirLexicalBinding {
    XirLexicalBindingKey key;
    uint32_t scope, order, value;
    XrXirType logical, physical;
    XrParamMode mode;
    XirLexicalBindingRole role;
    bool rebindable, direct_borrowed;
} XirLexicalBinding;

typedef enum XirLexicalPositionKind {
    XIR_LEXICAL_BLOCK_ENTRY,
    XIR_LEXICAL_BEFORE,
    XIR_LEXICAL_AFTER,
    XIR_LEXICAL_NORMAL_EDGE,
    XIR_LEXICAL_ERROR_EDGE,
    XIR_LEXICAL_PANIC_EDGE
} XirLexicalPositionKind;

typedef enum XirLexicalEventKind {
    XIR_LEXICAL_ENTER,
    XIR_LEXICAL_LEAVE,
    XIR_LEXICAL_RETURN,
    XIR_LEXICAL_BREAK,
    XIR_LEXICAL_CONTINUE,
    XIR_LEXICAL_THROW,
    XIR_LEXICAL_PANIC,
    XIR_LEXICAL_BIND,
    XIR_LEXICAL_REPLACE,
    XIR_LEXICAL_TAKE,
    XIR_LEXICAL_TEMP_BEGIN,
    XIR_LEXICAL_TEMP_END
} XirLexicalEventKind;

/* Position names the trigger. A transfer keeps its actual result before any
 * retirement, then retires crossed scopes after their existing user cleanups.
 * instruction is a zero-based instruction ordinal without parameters. value
 * is an SSA value ID including parameters. BLOCK_ENTRY names block.first.
 * ordinal selects an ordinary CFG edge/outcome; same-point events retain
 * their array order. A zero scope/binding/value is valid; UINT32_MAX means
 * no binding/value or a target outside this function. statement uses the
 * Source full-expression identity plus one; zero means no temporary owner.
 * parent_statement names that expression's enclosing temporary, or zero.
 * Edge ordinals and frontiers retain the ordinary CFG/cleanup coordinates. */
typedef struct XirLexicalEvent {
    uint32_t block, instruction, ordinal;
    uint32_t scope, target, binding, value;
    uint32_t frontier, target_frontier, statement, parent_statement;
    XirLexicalPositionKind position;
    XirLexicalEventKind kind;
} XirLexicalEvent;

typedef struct XirLexicalFunction {
    const XirLexicalScope *scopes;
    uint32_t scope_count;
    const XirLexicalBinding *bindings;
    uint32_t binding_count;
    const XirLexicalEvent *events;
    uint32_t event_count;
} XirLexicalFunction;

typedef struct XirLexicalProgram {
    const XirLexicalFunction *functions;
    uint32_t function_count;
} XirLexicalProgram;

/* Borrow only the receiving Artifact's owned numeric directory. Absence keeps
 * the extent unresolved. A successful query never infers or allocates. */
XR_FUNC XrXirStatus xr_xir_compile_lexical_function(const XrXirArtifact *,
    uint32_t function, XirLexicalFunction *output);
#endif // XXIR_LEXICAL_H
