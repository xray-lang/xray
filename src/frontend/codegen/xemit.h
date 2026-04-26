/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xemit.h - Instruction emitter with peephole optimization
 *
 * KEY CONCEPT:
 *   Unified instruction emission API with automatic peephole optimization
 *   and emission statistics for debugging.
 */

#ifndef XEMIT_H
#define XEMIT_H

#include "../../runtime/value/xchunk.h"
#include "xregalloc.h"
#include <stdbool.h>
#include "../../base/xdefs.h"

typedef struct XrPeepholeWindow {
    XrInstruction last_inst;
    int last_pc;
    int last_dest;
    OpCode last_op;
    bool can_optimize;
} XrPeepholeWindow;

typedef struct XrCompilerContext XrCompilerContext;

typedef struct XrEmitter {
    XrProto *proto;
    XrCompilerContext *ctx;
    XRegAlloc *regalloc;

    int pc;

    XrPeepholeWindow window;
    bool enable_peephole;

    struct {
        int inst_count;
        int move_count;
        int optimized_count;
        int jump_count;
    } stats;

    bool debug_mode;
} XrEmitter;

XR_FUNC XrEmitter *emitter_new(XrCompilerContext *ctx, XrProto *proto, XRegAlloc *regalloc);
XR_FUNC void emitter_free(XrEmitter *e);
XR_FUNC void emitter_set_peephole(XrEmitter *e, bool enable);
XR_FUNC void emitter_set_debug(XrEmitter *e, bool enable);

// Generic instruction emitters. Prefer the strongly typed xemit_<op>()
// API in xemit_typed.h (included at the bottom of this header) for any
// call site whose opcode is a literal OP_<NAME>. The generic emitters
// below remain available for two purposes only:
//   1. Dynamic dispatch — opcode is selected at runtime (e.g. binary_op
//      wrappers that pick OP_ADD / OP_SUB / OP_MUL based on AST kind).
//   2. KOP_SPECIAL opcodes (OP_TRY, OP_NOP) whose operand encoding is a
//      composite payload that doesn't fit any single typed signature.
// Every other call site goes through xemit_<op>() and gets parameter-by-
// parameter type / role checking from the compiler.
XR_FUNC int emit_abc(XrEmitter *e, OpCode op, int a, int b, int c);
XR_FUNC int emit_abx(XrEmitter *e, OpCode op, int a, int bx);
XR_FUNC int emit_asbx(XrEmitter *e, OpCode op, int a, int sbx);
XR_FUNC int emit_sj(XrEmitter *e, OpCode op, int sj);

// Convenience helpers retained for emitter-internal use (peephole,
// fixups, etc.). Callers outside the emitter should prefer the typed
// API; these wrappers exist mainly because emit_binary_op / emit_unary_op
// take a runtime OpCode.
XR_FUNC int emit_move(XrEmitter *e, int dest, int src);
XR_FUNC int emit_loadk(XrEmitter *e, int dest, int const_idx);
XR_FUNC int emit_loadnull(XrEmitter *e, int reg);
XR_FUNC int emit_loadtrue(XrEmitter *e, int reg);
XR_FUNC int emit_loadfalse(XrEmitter *e, int reg);
XR_FUNC int emit_binary_op(XrEmitter *e, OpCode op, int dest, int lhs, int rhs);
XR_FUNC int emit_unary_op(XrEmitter *e, OpCode op, int dest, int src);
XR_FUNC int emit_call(XrEmitter *e, int func, int nargs, int nresults);
XR_FUNC int emit_return(XrEmitter *e, int base, int nret);

// Per-function symbol helpers: register global symbol → local index in proto->symbols
XR_FUNC int emitter_add_symbol(XrEmitter *e, int global_symbol);

#define NO_JUMP (-1)

XR_FUNC int emit_jump(XrEmitter *e, OpCode op);
XR_FUNC void patch_jump(XrEmitter *e, int jump_pc, int target_pc);
XR_FUNC int jump_list_concat(XrEmitter *e, int list1, int list2);
XR_FUNC void patch_jump_list(XrEmitter *e, int list, int target_pc);
XR_FUNC void emit_loop(XrEmitter *e, int loop_start);
XR_FUNC int emit_test(XrEmitter *e, int cond_reg, bool negate);

XR_FUNC void emitter_print_stats(XrEmitter *e);
XR_FUNC void emitter_flush_peephole(XrEmitter *e);
XR_FUNC void emitter_write_instruction(XrEmitter *e, XrInstruction inst);
XR_FUNC void emitter_update_window(XrEmitter *e, XrInstruction inst, int pc);

XR_FUNC bool try_optimize_sequence(XrEmitter *e, XrInstruction inst);
XR_FUNC bool optimize_redundant_move(XrEmitter *e, XrInstruction inst);

XR_FUNC void emit_patch_instruction_A(XrEmitter *e, int pc, int new_A);
XR_FUNC XrInstruction *emit_get_instruction(XrEmitter *e, int pc);
XR_FUNC int emit_get_current_pc(XrEmitter *e);

XR_FUNC int emit_spill(XrEmitter *e, int slot, int reg);
XR_FUNC int emit_reload(XrEmitter *e, int reg, int slot);

// Strongly typed per-opcode emit API (one inline function per VM opcode).
// Generated from xopcode_def.h; included here so all codegen TUs that
// already include xemit.h pick it up automatically.
#include "xemit_typed.h"

#endif  // XEMIT_H
