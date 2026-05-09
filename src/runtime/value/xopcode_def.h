/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xopcode_def.h - Single source of truth for the VM opcode table.
 *
 * KEY CONCEPT:
 *   X-macro listing every VM opcode exactly once. Downstream consumers
 *   expand it without ever maintaining their own list:
 *
 *     - xchunk.h           -> typedef enum { OP_*, NUM_OPCODES }
 *     - xopcode_info.c     -> name / format / per-field semantic kinds
 *     - xvm_jumptab.h      -> &&L_OP_* computed-goto label table
 *     - xemit (future)     -> strongly typed emitter API
 *
 *   Adding an opcode means adding exactly one XR_OPCODE_TABLE entry
 *   here. The expansion order is the canonical enum order, so the
 *   numeric value of every opcode stays stable between consumers.
 *
 * ENTRY SHAPE:
 *
 *   _(NAME, FMT_TAG, KOP_TAG, "human-readable description")
 *
 *   NAME      bare opcode suffix; macros prepend OP_ / L_OP_ as needed
 *   FMT_TAG   InstrFormat enumerator from xopcode_info.h (printer hint)
 *   KOP_TAG   field-kind triple expanded by KOP_* macros below; encodes
 *             the runtime semantic role of each A/B/C byte slot
 *             (REG_OUT / REG_IN / K_IDX / LIT / ...). This triple is
 *             the contract emitter and VM agree on, and exists so the
 *             emitter can validate operands at the call site.
 *   "..."    description string used by the disassembler / debugger.
 */

#ifndef XOPCODE_DEF_H
#define XOPCODE_DEF_H

/* ========== KOP_* — field-kind triple shorthands ==========
 * Each macro expands to three XrOpFieldKind values for slots A, B, C.
 * For ABx / AsBx / GLOBAL / PROTO formats the B slot represents the
 * full Bx field; the C slot is XR_OPF_NONE. */

#define KOP_NONE XR_OPF_NONE, XR_OPF_NONE, XR_OPF_NONE
#define KOP_A_LOAD XR_OPF_REG_OUT, XR_OPF_NONE, XR_OPF_NONE
#define KOP_A_USE XR_OPF_REG_IN, XR_OPF_NONE, XR_OPF_NONE
#define KOP_A_INOUT XR_OPF_REG_INOUT, XR_OPF_NONE, XR_OPF_NONE
#define KOP_A_LIT XR_OPF_LIT, XR_OPF_NONE, XR_OPF_NONE
#define KOP_AB_UNARY XR_OPF_REG_OUT, XR_OPF_REG_IN, XR_OPF_NONE
#define KOP_AB_INPLACE XR_OPF_REG_INOUT, XR_OPF_REG_IN, XR_OPF_NONE
#define KOP_AB_INOUT_IN XR_OPF_REG_INOUT, XR_OPF_REG_IN, XR_OPF_NONE
#define KOP_AB_NEW_LIT XR_OPF_REG_OUT, XR_OPF_LIT, XR_OPF_NONE
#define KOP_AB_BASE_LIT XR_OPF_REG_BASE, XR_OPF_LIT, XR_OPF_NONE
#define KOP_AB_RECV XR_OPF_REG_BASE, XR_OPF_REG_IN, XR_OPF_NONE
#define KOP_ABC_BIN XR_OPF_REG_OUT, XR_OPF_REG_IN, XR_OPF_REG_IN
#define KOP_ABC_INPLACE XR_OPF_REG_INOUT, XR_OPF_REG_IN, XR_OPF_REG_IN
#define KOP_ABC_BIN_K XR_OPF_REG_OUT, XR_OPF_REG_IN, XR_OPF_K_IDX
#define KOP_ABC_BIN_S XR_OPF_REG_OUT, XR_OPF_REG_IN, XR_OPF_LIT_S
#define KOP_ABC_BIN_LIT XR_OPF_REG_OUT, XR_OPF_REG_IN, XR_OPF_LIT
#define KOP_ABC_INPLACE_K XR_OPF_REG_INOUT, XR_OPF_K_IDX, XR_OPF_REG_IN
#define KOP_ABC_INPLACE_LIT XR_OPF_REG_INOUT, XR_OPF_LIT, XR_OPF_REG_IN
#define KOP_AB_TEST XR_OPF_REG_IN, XR_OPF_REG_IN, XR_OPF_LIT_FLAG
#define KOP_AB_TEST_K XR_OPF_REG_IN, XR_OPF_K_IDX, XR_OPF_LIT_FLAG
#define KOP_AB_TEST_S XR_OPF_REG_IN, XR_OPF_LIT_S, XR_OPF_LIT_FLAG
#define KOP_A_TEST XR_OPF_REG_IN, XR_OPF_NONE, XR_OPF_LIT_FLAG
#define KOP_ABx_K XR_OPF_REG_OUT, XR_OPF_K_IDX, XR_OPF_NONE
#define KOP_AsBx_LITS XR_OPF_REG_OUT, XR_OPF_LIT_S, XR_OPF_NONE
#define KOP_PROTO XR_OPF_REG_OUT, XR_OPF_PROTO_IDX, XR_OPF_NONE
#define KOP_GLOBAL_GET XR_OPF_REG_OUT, XR_OPF_GLOBAL_IDX, XR_OPF_NONE
#define KOP_GLOBAL_SET XR_OPF_REG_IN, XR_OPF_GLOBAL_IDX, XR_OPF_NONE
#define KOP_ABx_LAYOUT XR_OPF_REG_OUT, XR_OPF_LIT, XR_OPF_NONE
#define KOP_ABx_LIT XR_OPF_REG_OUT, XR_OPF_LIT, XR_OPF_NONE
#define KOP_CALL XR_OPF_REG_BASE, XR_OPF_LIT, XR_OPF_LIT
#define KOP_CALL_KEEP XR_OPF_REG_BASE, XR_OPF_LIT, XR_OPF_REG_OUT
#define KOP_RETURN XR_OPF_REG_BASE, XR_OPF_LIT, XR_OPF_NONE
#define KOP_INVOKE_K XR_OPF_REG_BASE, XR_OPF_K_IDX, XR_OPF_LIT
#define KOP_INVOKE_SYM XR_OPF_REG_BASE, XR_OPF_SYMBOL_IDX, XR_OPF_LIT
#define KOP_ABC_BIN_SYM XR_OPF_REG_OUT, XR_OPF_REG_IN, XR_OPF_SYMBOL_IDX
#define KOP_ABC_INPLACE_SYM XR_OPF_REG_INOUT, XR_OPF_SYMBOL_IDX, XR_OPF_REG_IN
#define KOP_AB_K XR_OPF_REG_IN, XR_OPF_K_IDX, XR_OPF_NONE
#define KOP_AB_FLAG XR_OPF_REG_IN, XR_OPF_LIT_FLAG, XR_OPF_NONE
#define KOP_PRINT XR_OPF_REG_IN, XR_OPF_LIT_FLAG, XR_OPF_LIT
#define KOP_INVOKE_DIRECT XR_OPF_REG_BASE, XR_OPF_REG_IN, XR_OPF_LIT
#define KOP_INVOKE_BUILTIN XR_OPF_REG_BASE, XR_OPF_BUILTIN_IDX, XR_OPF_LIT
#define KOP_DUMP XR_OPF_REG_IN, XR_OPF_LIT, XR_OPF_NONE
#define KOP_AB_UNARY_HINT XR_OPF_REG_OUT, XR_OPF_REG_IN, XR_OPF_LIT
#define KOP_NEW_CONTAINER XR_OPF_REG_OUT, XR_OPF_LIT, XR_OPF_LIT
#define KOP_SPECIAL XR_OPF_SPECIAL, XR_OPF_SPECIAL, XR_OPF_SPECIAL

#define XR_OPCODE_TABLE(_)                                                                         \
    _(MOVE, FMT_AB, KOP_AB_UNARY, "R[A] = R[B]")                                                   \
    _(LOADI, FMT_AsBx, KOP_AsBx_LITS, "R[A] = sBx")                                                \
    _(LOADF, FMT_AsBx, KOP_AsBx_LITS, "R[A] = (float)sBx")                                         \
    _(LOADK, FMT_ABx, KOP_ABx_K, "R[A] = K[Bx]")                                                   \
    _(LOADNULL, FMT_A, KOP_A_LOAD, "R[A] = null")                                                  \
    _(LOADTRUE, FMT_A, KOP_A_LOAD, "R[A] = true")                                                  \
    _(LOADFALSE, FMT_A, KOP_A_LOAD, "R[A] = false")                                                \
    _(ADD, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] + R[C]")                                             \
    _(ADDI, FMT_AB_sC, KOP_ABC_BIN_S, "R[A] = R[B] + sC")                                          \
    _(ADDK, FMT_ABC, KOP_ABC_BIN_K, "R[A] = R[B] + K[C]")                                          \
    _(SUB, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] - R[C]")                                             \
    _(SUBI, FMT_AB_sC, KOP_ABC_BIN_S, "R[A] = R[B] - sC")                                          \
    _(SUBK, FMT_ABC, KOP_ABC_BIN_K, "R[A] = R[B] - K[C]")                                          \
    _(MUL, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] * R[C]")                                             \
    _(MULI, FMT_AB_sC, KOP_ABC_BIN_S, "R[A] = R[B] * sC")                                          \
    _(MULK, FMT_ABC, KOP_ABC_BIN_K, "R[A] = R[B] * K[C]")                                          \
    _(DIV, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] / R[C]")                                             \
    _(DIVK, FMT_ABC, KOP_ABC_BIN_K, "R[A] = R[B] / K[C]")                                          \
    _(MOD, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] % R[C]")                                             \
    _(MODK, FMT_ABC, KOP_ABC_BIN_K, "R[A] = R[B] % K[C]")                                          \
    _(UNM, FMT_AB, KOP_AB_UNARY, "R[A] = -R[B]")                                                   \
    _(NOT, FMT_AB, KOP_AB_UNARY, "R[A] = !R[B]")                                                   \
    _(STRBUF_NEW, FMT_A, KOP_A_LOAD, "R[A] = new_strbuf()")                                        \
    _(STRBUF_APPEND, FMT_AB, KOP_AB_INPLACE, "strbuf(R[A]).append(R[B])")                          \
    _(STRBUF_FINISH, FMT_A, KOP_A_INOUT, "R[A] = strbuf(R[A]).to_string()")                        \
    _(BAND, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] & R[C]")                                            \
    _(BOR, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] | R[C]")                                             \
    _(BXOR, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] ^ R[C]")                                            \
    _(BNOT, FMT_AB, KOP_AB_UNARY, "R[A] = ~R[B]")                                                  \
    _(SHL, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] << R[C]")                                            \
    _(SHR, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] >> R[C]")                                            \
    _(EQ, FMT_AB_IMM, KOP_AB_TEST, "if (R[A] == R[B]) != k then PC++")                             \
    _(EQK, FMT_AB_IMM, KOP_AB_TEST_K, "if (R[A] == K[B]) != k then PC++")                          \
    _(EQI, FMT_AsB_C, KOP_AB_TEST_S, "if (R[A] == sB) != k then PC++")                             \
    _(LT, FMT_AB_IMM, KOP_AB_TEST, "if (R[A] < R[B]) != k then PC++")                              \
    _(LTI, FMT_AsB_C, KOP_AB_TEST_S, "if (R[A] < sB) != k then PC++")                              \
    _(LE, FMT_AB_IMM, KOP_AB_TEST, "if (R[A] <= R[B]) != k then PC++")                             \
    _(LEI, FMT_AsB_C, KOP_AB_TEST_S, "if (R[A] <= sB) != k then PC++")                             \
    _(CMP_EQ, FMT_ABC, KOP_ABC_BIN, "R[A] = (R[B] == R[C])")                                       \
    _(CMP_NE, FMT_ABC, KOP_ABC_BIN, "R[A] = (R[B] != R[C])")                                       \
    _(CMP_EQ_STRICT, FMT_ABC, KOP_ABC_BIN, "R[A] = (R[B] === R[C])")                               \
    _(CMP_NE_STRICT, FMT_ABC, KOP_ABC_BIN, "R[A] = (R[B] !== R[C])")                               \
    _(CMP_LT, FMT_ABC, KOP_ABC_BIN, "R[A] = (R[B] < R[C])")                                        \
    _(CMP_LE, FMT_ABC, KOP_ABC_BIN, "R[A] = (R[B] <= R[C])")                                       \
    _(IS, FMT_ABC, KOP_ABC_BIN, "R[A] = (R[B] is R[C])")                                           \
    _(CHECKTYPE, FMT_AB, KOP_AB_K, "assert R[A] is Type[K(B)]")                                    \
    _(ISNULL, FMT_AB_IMM, KOP_AB_FLAG, "if (R[A]==null) != k then PC++")                           \
    _(ISNULL_SET, FMT_AB, KOP_AB_UNARY, "R[A] = (R[B] == null)")                                   \
    _(JMP, FMT_sJ, KOP_NONE, "PC += sJ")                                                           \
    _(TEST, FMT_AB_IMM, KOP_A_TEST, "if R[A] != k then PC++")                                      \
    _(TESTSET, FMT_ABC, KOP_ABC_BIN_LIT, "if R[B] != k then PC++ else R[A]=R[B]")                  \
    _(CALL, FMT_ABC, KOP_CALL, "R[A] = R[A](R[A+1]...R[A+B-1])")                                   \
    _(CALL_KEEP, FMT_ABC, KOP_CALL_KEEP, "R[C] = R[A](R[A+1]...R[A+B]); R[A] kept")                \
    _(CALL_STATIC, FMT_ABC, KOP_CALL, "R[A](R[A+1]...R[A+B-1]) - known closure")                   \
    _(CALLSELF, FMT_ABC, KOP_CALL, "recursive call opt")                                           \
    _(TAILCALL, FMT_ABC, KOP_CALL, "tail call opt")                                                \
    _(RETURN, FMT_ABC, KOP_RETURN, "return R[A]...R[A+B-2]")                                       \
    _(RETURN0, FMT_NONE, KOP_NONE, "return (fast)")                                                \
    _(RETURN1, FMT_A, KOP_A_USE, "return R[A] (fast)")                                             \
    _(NEWARRAY, FMT_ABC, KOP_NEW_CONTAINER, "R[A] = [], B=capacity, C=storage")                    \
    _(NEWMAP, FMT_ABC, KOP_NEW_CONTAINER, "R[A] = #{}, B=capacity, C=storage")                     \
    _(NEWSET, FMT_AB, KOP_AB_NEW_LIT, "R[A] = #[], B=storage")                                     \
    _(NEWSTRINGBUILDER, FMT_AB, KOP_AB_NEW_LIT, "R[A] = new StringBuilder(), B=storage")           \
    _(NEWRANGE, FMT_ABC, KOP_ABC_BIN, "R[A] = Range(R[B], R[C])")                                  \
    _(RANGE_UNPACK, FMT_AB, KOP_AB_RECV, "R[A]=start, R[A+1]=end, R[A+2]=step from Range R[B]")    \
    _(ARRAY_GET, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B]:Array[R[C]]")                                  \
    _(ARRAY_GETC, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = R[B]:Array[C]")                                \
    _(ARRAY_SET, FMT_ABC, KOP_ABC_INPLACE, "R[A]:Array[R[B]] = R[C]")                              \
    _(ARRAY_SETC, FMT_ABC, KOP_ABC_INPLACE_LIT, "R[A]:Array[B] = R[C]")                            \
    _(ARRAY_PUSH, FMT_AB, KOP_AB_INPLACE, "R[A]:Array.push(R[B])")                                 \
    _(ARRAY_LEN, FMT_AB, KOP_AB_UNARY, "R[A] = len(R[B]:Array)")                                   \
    _(ARRAY_INIT, FMT_AB_IMM, KOP_AB_BASE_LIT, "R[A][1..B] = R[A+1..A+B]")                         \
    _(MAP_GET, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B]:Map[R[C]]")                                      \
    _(MAP_GETK, FMT_ABC, KOP_ABC_BIN_K, "R[A] = R[B]:Map[K[C]]")                                   \
    _(MAP_SET, FMT_ABC, KOP_ABC_INPLACE, "R[A]:Map[R[B]] = R[C]")                                  \
    _(MAP_SETK, FMT_ABC, KOP_ABC_INPLACE_K, "R[A]:Map[K[B]] = R[C]")                               \
    _(INDEX_GET, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B][R[C]] (runtime dispatch)")                     \
    _(INDEX_SET, FMT_ABC, KOP_ABC_INPLACE, "R[A][R[B]] = R[C] (runtime dispatch)")                 \
    _(SLICE, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B][R[C]:R[C+1]] (slice)")                             \
    _(CLOSURE, FMT_PROTO, KOP_PROTO, "R[A] = closure(XrProto[Bx])")                                \
    _(UPVAL_GET, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = cl->upvals[B]")                                 \
    _(CELL_NEW, FMT_A, KOP_A_INOUT, "R[A] = new_cell(R[A])")                                       \
    _(CELL_GET, FMT_ABC, KOP_AB_UNARY, "R[A] = cell_deref(R[B])")                                  \
    _(CELL_SET, FMT_ABC, KOP_ABC_INPLACE, "cell_store(R[A], R[B])")                                \
    _(CLASS_CREATE_FROM_DESCRIPTOR, FMT_ABx, KOP_ABx_K, "R[A] = Class.from_descriptor(K[Bx])")     \
    _(CLINIT_CALL, FMT_A, KOP_A_LIT, "call static init <clinit>")                                  \
    _(GETPROP, FMT_ABC, KOP_ABC_BIN_SYM, "R[A] = R[B].symbol[C]")                                  \
    _(SETPROP, FMT_ABC, KOP_ABC_INPLACE_SYM, "R[A].symbol[B] = R[C]")                              \
    _(GETSUPER, FMT_ABC, KOP_ABC_BIN_SYM, "R[A] = R[B].super.symbol[C]")                           \
    _(INVOKE, FMT_ABC, KOP_INVOKE_SYM, "R[A] = R[B]:method(...)")                                  \
    _(INVOKE_TAIL, FMT_ABC, KOP_INVOKE_SYM, "tail: R[A+1]:method(...) reuse frame")                \
    _(SUPERINVOKE, FMT_ABC, KOP_INVOKE_K, "super.K[B](...)")                                       \
    _(INVOKE_DIRECT, FMT_ABC, KOP_INVOKE_DIRECT, "R[A] = R[B]:methods[C](...)")                    \
    _(INVOKE_BUILTIN, FMT_ABC, KOP_INVOKE_BUILTIN, "R[A] = R[A+1]:builtin[B](nargs=C)")            \
    _(ABSTRACT_ERROR, FMT_NONE, KOP_NONE, "runtime: called abstract method")                       \
    _(SET_STORAGE_CTX, FMT_A, KOP_A_LIT, "set storage mode context A=mode")                        \
    _(TO_SHARED, FMT_AB, KOP_AB_UNARY, "R[A] = to_shared(R[B])")                                   \
    _(MAP_SETKS, FMT_AB, KOP_AB_BASE_LIT, "R[A].fields[0..B-1] = R[A+1]..R[A+B]")                  \
    _(GETFIELD, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = R[B]:Instance.fields[C]")                        \
    _(SETFIELD, FMT_ABC, KOP_ABC_INPLACE_LIT, "R[A]:Instance.fields[B] = R[C]")                    \
    _(GETFIELD_IC, FMT_ABC, KOP_SPECIAL, "R[A] = R[B].K[C] (IC)")                                  \
    _(NEWJSON, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = new Json(K[B]:Shape, storage_mode=C)")            \
    _(JSON_GET, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = R[B].fields[C]")                                 \
    _(JSON_SET, FMT_ABC, KOP_ABC_INPLACE_LIT, "R[A].fields[B] = R[C]")                             \
    _(JSON_GETK, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = R[B].get(symbol=C)")                            \
    _(JSON_SETK, FMT_ABC, KOP_ABC_INPLACE_LIT, "R[A].set(symbol=B, R[C])")                         \
    _(JSON_INIT, FMT_ABC, KOP_ABC_INPLACE_LIT, "R[A].fields[B] = R[C]")                            \
    _(JSON_INIT_I, FMT_ABC, KOP_SPECIAL, "R[A].fields[B] = C")                                     \
    _(JSON_INIT_N, FMT_ABC, KOP_SPECIAL, "R[A].fields[B] = null")                                  \
    _(JSON_DECODE, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = decode(R[B]:string, K[C]:Shape)")             \
    _(GETBUILTIN, FMT_GLOBAL, KOP_GLOBAL_GET, "R[A] = builtins[Bx]")                               \
    _(PRINT, FMT_A, KOP_PRINT, "print(R[A], add_space=B, packed=C)")                               \
    _(TYPEOF, FMT_AB, KOP_AB_UNARY_HINT, "R[A] = typeof(R[B]) -> int")                             \
    _(TYPENAME, FMT_AB, KOP_AB_UNARY_HINT, "R[A] = typename(R[B]) -> string")                      \
    _(DUMP, FMT_AB, KOP_DUMP, "dump(R[A], indent=B)")                                              \
    _(TOINT, FMT_AB, KOP_AB_UNARY_HINT, "R[A] = int(R[B])")                                        \
    _(TOFLOAT, FMT_AB, KOP_AB_UNARY_HINT, "R[A] = float(R[B])")                                    \
    _(TOSTRING, FMT_AB, KOP_AB_UNARY_HINT, "R[A] = string(R[B])")                                  \
    _(TOBOOL, FMT_AB, KOP_AB_UNARY, "R[A] = bool(R[B])")                                           \
    _(COPY, FMT_AB, KOP_AB_UNARY, "R[A] = copy(R[B])")                                             \
    _(CHR, FMT_AB, KOP_AB_UNARY, "R[A] = chr(R[B])")                                               \
    _(ENUM_ACCESS, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B].variant[R[C]]")                              \
    _(ENUM_CONVERT, FMT_ABC, KOP_ABC_BIN, "R[A] = enum_convert(R[B], R[C])")                       \
    _(ENUM_NAME, FMT_AB, KOP_AB_UNARY, "R[A] = enum_name(R[B])")                                   \
    _(TRY, FMT_SPECIAL, KOP_SPECIAL, "try block start")                                            \
    _(CATCH, FMT_A, KOP_A_LOAD, "catch block")                                                     \
    _(FINALLY, FMT_NONE, KOP_NONE, "finally block")                                                \
    _(END_TRY, FMT_NONE, KOP_NONE, "try block end")                                                \
    _(THROW, FMT_A, KOP_A_USE, "throw R[A]")                                                       \
    _(SPILL, FMT_AB, KOP_AB_UNARY, "S[A] = R[B] (spill register to slot)")                         \
    _(RELOAD, FMT_AB, KOP_AB_UNARY, "R[A] = S[B] (reload from slot)")                              \
    _(BOX_I64, FMT_AB, KOP_AB_UNARY, "R[A] = box(R[B] as i64)")                                    \
    _(BOX_F64, FMT_AB, KOP_AB_UNARY, "R[A] = box(R[B] as f64)")                                    \
    _(UNBOX_I64, FMT_AB, KOP_AB_UNARY, "R[A] = unbox(R[B]) as i64")                                \
    _(UNBOX_F64, FMT_AB, KOP_AB_UNARY, "R[A] = unbox(R[B]) as f64")                                \
    _(NARROW_I8, FMT_AB, KOP_AB_UNARY, "R[A] = (int8_t)R[B]")                                      \
    _(NARROW_U8, FMT_AB, KOP_AB_UNARY, "R[A] = (uint8_t)R[B]")                                     \
    _(NARROW_I16, FMT_AB, KOP_AB_UNARY, "R[A] = (int16_t)R[B]")                                    \
    _(NARROW_U16, FMT_AB, KOP_AB_UNARY, "R[A] = (uint16_t)R[B]")                                   \
    _(NARROW_I32, FMT_AB, KOP_AB_UNARY, "R[A] = (int32_t)R[B]")                                    \
    _(NARROW_U32, FMT_AB, KOP_AB_UNARY, "R[A] = (uint32_t)R[B]")                                   \
    _(NARROW_F32, FMT_AB, KOP_AB_UNARY, "R[A] = (float)R[B]")                                      \
    _(WIDEN_I8, FMT_AB, KOP_AB_UNARY, "R[A] = sign_ext_i8(R[B])")                                  \
    _(WIDEN_U8, FMT_AB, KOP_AB_UNARY, "R[A] = zero_ext_u8(R[B])")                                  \
    _(WIDEN_I16, FMT_AB, KOP_AB_UNARY, "R[A] = sign_ext_i16(R[B])")                                \
    _(WIDEN_U16, FMT_AB, KOP_AB_UNARY, "R[A] = zero_ext_u16(R[B])")                                \
    _(WIDEN_I32, FMT_AB, KOP_AB_UNARY, "R[A] = sign_ext_i32(R[B])")                                \
    _(WIDEN_U32, FMT_AB, KOP_AB_UNARY, "R[A] = zero_ext_u32(R[B])")                                \
    _(WIDEN_F32, FMT_AB, KOP_AB_UNARY, "R[A] = (double)(float)R[B]")                               \
    _(ARRAY_GET_NOCHECK, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B]:Array[R[C]] (no check)")               \
    _(MAP_INCREMENT, FMT_AB, KOP_AB_INPLACE, "R[A]:Map[R[B]]++")                                   \
    _(SUBSTRING, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B].substring(R[C], R[C+1])")                      \
    _(STR_REPEAT, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B] * R[C] (string repeat)")                      \
    _(IMPORT, FMT_ABx, KOP_ABx_K, "R[A] = import(K[Bx])")                                          \
    _(EXPORT, FMT_ABC, KOP_SPECIAL, "export(K[A], R[B], C=const?)")                                \
    _(EXPORT_ALL, FMT_A, KOP_A_USE, "export * from R[A]")                                          \
    _(ASSERT, FMT_ABC, KOP_SPECIAL, "if !R[A] throw AssertError(K[B]); C=1: negate")               \
    _(ASSERT_EQ, FMT_ABC, KOP_SPECIAL, "if R[A] != R[B] throw AssertError(K[C])")                  \
    _(ASSERT_NE, FMT_ABC, KOP_SPECIAL, "if R[A] == R[B] throw AssertError(K[C])")                  \
    _(REGEX_COMPILE, FMT_ABC, KOP_ABC_BIN_K, "R[A] = regex.compile(K[B], K[C])")                   \
    _(GO, FMT_ABC, KOP_ABC_BIN_LIT,                                                                \
      "R[A]=task = go R[B](R[B+1]..R[B+C&0x7F]), C bit7=fire-and-forget")                          \
    _(AWAIT, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = await R[B], C=discard")                             \
    _(AWAIT_TIMEOUT, FMT_ABC, KOP_ABC_BIN, "R[A] = await(timeout: R[C]) R[B]")                     \
    _(AWAIT_ALL, FMT_AB, KOP_AB_UNARY, "R[A] = await R[B]:Array")                                  \
    _(AWAIT_ANY, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = await any R[B]:Array, C=mode")                  \
    _(YIELD, FMT_A, KOP_A_LIT, "yield (A=poll_threshold for select default)")                      \
    _(CANCELLED, FMT_A, KOP_A_LOAD, "R[A] = cancelled()")                                          \
    _(LOCK_THREAD, FMT_NONE, KOP_NONE, "Coro.lockThread()")                                        \
    _(UNLOCK_THREAD, FMT_NONE, KOP_NONE, "Coro.unlockThread()")                                    \
    _(SET_LOCAL, FMT_AB, KOP_AB_INOUT_IN, "Coro.setLocal(R[A], R[B])")                             \
    _(GET_LOCAL, FMT_AB, KOP_AB_UNARY, "R[A] = Coro.getLocal(R[B])")                               \
    _(SET_PRIORITY, FMT_AB, KOP_AB_INOUT_IN, "Coro.setPriority(R[A], R[B])")                       \
    _(CORO_CTRL, FMT_ABC, KOP_SPECIAL, "coro monitoring, C=sub_op")                                \
    _(CHAN_NEW, FMT_ABx, KOP_ABx_LIT, "R[A] = Channel(Bx) — buffer size")                          \
    _(CHAN_NEW_NAMED, FMT_ABC, KOP_ABC_BIN, "R[A] = Channel(R[B], R[C]) - named")                  \
    _(CHAN_SEND, FMT_ABC, KOP_SPECIAL, "R[B].send(R[C])")                                          \
    _(CHAN_RECV, FMT_AB, KOP_AB_RECV, "R[A], R[A+1] = R[B].recv()")                                \
    _(CHAN_TRY_SEND, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B].trySend(R[C])")                            \
    _(CHAN_TRY_RECV, FMT_AB, KOP_AB_RECV, "R[A], R[A+1] = R[B].tryRecv()")                         \
    _(CHAN_SEND_TIMEOUT, FMT_ABC, KOP_SPECIAL, "R[A] = R[B].send(R[C], timeout: R[C+1])")          \
    _(CHAN_RECV_TIMEOUT, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B].recv(timeout: R[C])")                  \
    _(CHAN_CLOSE, FMT_A, KOP_A_USE, "R[A].close()")                                                \
    _(CHAN_IS_CLOSED, FMT_AB, KOP_AB_UNARY, "R[A] = R[B].isClosed()")                              \
    _(SELECT_START, FMT_NONE, KOP_NONE, "select start")                                            \
    _(SELECT_CASE, FMT_ABC, KOP_SPECIAL, "case A=type, B=channel, C=value")                        \
    _(SELECT_END, FMT_NONE, KOP_NONE, "select end")                                                \
    _(DEFER, FMT_AB, KOP_AB_BASE_LIT, "defer R[A](args R[A+1..A+B-1])")                            \
    _(BYTES_NEW, FMT_AB, KOP_AB_NEW_LIT, "R[A] = Bytes(B args)")                                   \
    _(SCOPE_ENTER, FMT_A, KOP_A_LIT, "enter scope, A=mode(0=wait,1=linked,2=supervisor)")          \
    _(SCOPE_EXIT, FMT_AB, KOP_AB_NEW_LIT, "exit scope, A=mode, B=result_reg")                      \
    _(SLEEP, FMT_A, KOP_A_USE, "time.sleep(R[A]) ms")                                              \
    _(TIME_AFTER, FMT_AB, KOP_AB_UNARY, "R[A] = time.after(R[B]) ms")                              \
    _(SELECT_BLOCK, FMT_ABC, KOP_ABC_BIN_LIT,                                                      \
      "select block wait: A=ch_base, B=ch_count, C=case_count")                                    \
    _(GETSHARED, FMT_GLOBAL, KOP_GLOBAL_GET, "R[A] = shared[Bx]")                                  \
    _(SETSHARED, FMT_GLOBAL, KOP_GLOBAL_SET, "shared[Bx] = R[A]")                                  \
    _(TARRAY_GET, FMT_ABC, KOP_ABC_BIN, "R[A] = R[B]:typed_array[R[C]]")                           \
    _(TARRAY_GETC, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = R[B]:typed_array[C]")                         \
    _(TARRAY_SET, FMT_ABC, KOP_ABC_INPLACE, "R[A]:typed_array[R[B]] = R[C]")                       \
    _(TARRAY_PUSH, FMT_AB, KOP_AB_INPLACE, "R[A]:typed_array.push(R[B])")                          \
    _(TFIELD_GET, FMT_ABC, KOP_ABC_BIN_LIT, "R[A].i = R[B]:compact_fields[C]")                     \
    _(TFIELD_SET, FMT_ABC, KOP_ABC_INPLACE_LIT, "R[A]:compact_fields[B] = R[C].i")                 \
    _(LOOP_BACK, FMT_ABC, KOP_SPECIAL, "tail recursion: R[0..B-1]=R[A+1..A+B]; PC=entry")          \
    _(NEW_STRUCT, FMT_ABC, KOP_ABC_BIN_LIT,                                                        \
      "R[A] = alloc struct in struct_area (B=class reg, C=slot offset)")                           \
    _(STRUCT_GET, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = struct(R[B]).field[C]")                        \
    _(STRUCT_SET, FMT_ABC, KOP_ABC_INPLACE_LIT, "struct(R[A]).field[B] = R[C]")                    \
    _(STRUCT_COPY, FMT_ABC, KOP_ABC_BIN_LIT, "R[A] = memcpy struct R[B] into struct_area slot C")  \
    _(NOP, FMT_SPECIAL, KOP_SPECIAL, "no-op / spawn metadata")

#endif  // XOPCODE_DEF_H
