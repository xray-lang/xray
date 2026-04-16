/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdebug.c - Bytecode debugging tools implementation
 *
 * KEY CONCEPT:
 *   Table-driven bytecode disassembler for debugging and analysis.
 */

#include "xdebug.h"
#include "../base/xchecks.h"
#include "../runtime/object/xstring.h"
#include "../runtime/value/xvalue_print.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xslot_type.h"
#include <stdio.h>

/* ========== Instruction Format Definitions ========== */

// Instruction format enum for table-driven disassembly
typedef enum {
    FMT_NONE,
    FMT_A,
    FMT_AB,
    FMT_ABC,
    FMT_ABx,
    FMT_AsBx,
    FMT_sJ,
    FMT_AB_sC,
    FMT_AsB_C,
    FMT_AB_IMM,
    FMT_ABx_INT,   // R[A] <integer Bx> — Bx is a raw integer, not a const index
    FMT_PROTO,
    FMT_GLOBAL,
    FMT_SPECIAL
} InstrFormat;

// Instruction metadata
typedef struct {
    const char *name;
    InstrFormat format;
    const char *desc;
} OpCodeInfo;

/* ========== Opcode Metadata Table ========== */

// Instruction metadata table (core of table-driven disassembly)
static const OpCodeInfo opcode_table[NUM_OPCODES] = {
    // Basic operations
    [OP_MOVE]       = {"MOVE",       FMT_AB,    "R[A] = R[B]"},
    [OP_LOADI]      = {"LOADI",      FMT_AsBx,  "R[A] = sBx"},
    [OP_LOADF]      = {"LOADF",      FMT_AsBx,  "R[A] = (float)sBx"},
    [OP_LOADK]      = {"LOADK",      FMT_ABx,   "R[A] = K[Bx]"},
    [OP_LOADNULL]   = {"LOADNULL",   FMT_A,     "R[A] = null"},
    [OP_LOADTRUE]   = {"LOADTRUE",   FMT_A,     "R[A] = true"},
    [OP_LOADFALSE]  = {"LOADFALSE",  FMT_A,     "R[A] = false"},
    
    // Arithmetic
    [OP_ADD]        = {"ADD",        FMT_ABC,   "R[A] = R[B] + R[C]"},
    [OP_ADDI]       = {"ADDI",       FMT_AB_sC, "R[A] = R[B] + sC"},
    [OP_ADDK]       = {"ADDK",       FMT_ABC,   "R[A] = R[B] + K[C]"},
    [OP_SUB]        = {"SUB",        FMT_ABC,   "R[A] = R[B] - R[C]"},
    [OP_SUBI]       = {"SUBI",       FMT_AB_sC, "R[A] = R[B] - sC"},
    [OP_SUBK]       = {"SUBK",       FMT_ABC,   "R[A] = R[B] - K[C]"},
    [OP_MUL]        = {"MUL",        FMT_ABC,   "R[A] = R[B] * R[C]"},
    [OP_MULI]       = {"MULI",       FMT_AB_sC, "R[A] = R[B] * sC"},
    [OP_MULK]       = {"MULK",       FMT_ABC,   "R[A] = R[B] * K[C]"},
    [OP_DIV]        = {"DIV",        FMT_ABC,   "R[A] = R[B] / R[C]"},
    [OP_DIVK]       = {"DIVK",       FMT_ABC,   "R[A] = R[B] / K[C]"},
    [OP_MOD]        = {"MOD",        FMT_ABC,   "R[A] = R[B] % R[C]"},
    [OP_MODK]       = {"MODK",       FMT_ABC,   "R[A] = R[B] % K[C]"},
    [OP_UNM]        = {"UNM",        FMT_AB,    "R[A] = -R[B]"},
    [OP_NOT]        = {"NOT",        FMT_AB,    "R[A] = !R[B]"},
    
    // String buffer
    [OP_STRBUF_NEW]    = {"STRBUF_NEW",    FMT_A,     "R[A] = new_strbuf()"},
    [OP_STRBUF_APPEND] = {"STRBUF_APPEND", FMT_AB,    "strbuf(R[A]).append(R[B])"},
    [OP_STRBUF_FINISH] = {"STRBUF_FINISH", FMT_A,     "R[A] = strbuf(R[A]).to_string()"},
    
    // Bitwise operations
    [OP_BAND]       = {"BAND",       FMT_ABC,   "R[A] = R[B] & R[C]"},
    [OP_BOR]        = {"BOR",        FMT_ABC,   "R[A] = R[B] | R[C]"},
    [OP_BXOR]       = {"BXOR",       FMT_ABC,   "R[A] = R[B] ^ R[C]"},
    [OP_BNOT]       = {"BNOT",       FMT_AB,    "R[A] = ~R[B]"},
    [OP_SHL]        = {"SHL",        FMT_ABC,   "R[A] = R[B] << R[C]"},
    [OP_SHR]        = {"SHR",        FMT_ABC,   "R[A] = R[B] >> R[C]"},
    
    // Comparison - C is condition flag k, not register
    [OP_EQ]         = {"EQ",         FMT_AB_IMM,"if (R[A] == R[B]) != k then PC++"},
    [OP_EQK]        = {"EQK",        FMT_AB_IMM,"if (R[A] == K[B]) != k then PC++"},
    [OP_EQI]        = {"EQI",        FMT_AsB_C, "if (R[A] == sB) != k then PC++"},
    [OP_LT]         = {"LT",         FMT_AB_IMM,"if (R[A] < R[B]) != k then PC++"},
    [OP_LTI]        = {"LTI",        FMT_AsB_C, "if (R[A] < sB) != k then PC++"},
    [OP_LE]         = {"LE",         FMT_AB_IMM,"if (R[A] <= R[B]) != k then PC++"},
    [OP_LEI]        = {"LEI",        FMT_AsB_C, "if (R[A] <= sB) != k then PC++"},
    
    // Comparison (expression)
    [OP_CMP_EQ]     = {"CMP_EQ",     FMT_ABC,   "R[A] = (R[B] == R[C])"},
    [OP_CMP_NE]     = {"CMP_NE",     FMT_ABC,   "R[A] = (R[B] != R[C])"},
    [OP_CMP_LT]     = {"CMP_LT",     FMT_ABC,   "R[A] = (R[B] < R[C])"},
    [OP_CMP_LE]     = {"CMP_LE",     FMT_ABC,   "R[A] = (R[B] <= R[C])"},
    [OP_CMP_EQ_STRICT] = {"CMP_EQ_STRICT", FMT_ABC, "R[A] = (R[B] === R[C])"},
    [OP_CMP_NE_STRICT] = {"CMP_NE_STRICT", FMT_ABC, "R[A] = (R[B] !== R[C])"},
    [OP_CHECKTYPE]  = {"CHECKTYPE",  FMT_AB,    "assert R[A] is Type[K(B)]"},
    [OP_ISNULL]     = {"ISNULL",     FMT_AB_IMM,"if (R[A]==null) != k then PC++"},
    [OP_ISNULL_SET] = {"ISNULL_SET", FMT_AB,    "R[A] = (R[B] == null)"},
    
    // Control flow
    [OP_JMP]        = {"JMP",        FMT_sJ,    "PC += sJ"},
    [OP_TEST]       = {"TEST",       FMT_AB_IMM,"if R[A] != k then PC++"},
    [OP_TESTSET]    = {"TESTSET",    FMT_ABC,   "if R[B] != k then PC++ else R[A]=R[B]"},
    [OP_CALL]       = {"CALL",       FMT_ABC,   "R[A] = R[A](R[A+1]...R[A+B-1])"},
    [OP_CALL_KEEP]  = {"CALL_KEEP",  FMT_ABC,   "R[C] = R[A](R[A+1]...R[A+B]); R[A] kept"},
    [OP_CALLSELF]   = {"CALLSELF",   FMT_ABC,   "recursive call opt"},
    [OP_TAILCALL]   = {"TAILCALL",   FMT_ABC,   "tail call opt"},
    [OP_RETURN]     = {"RETURN",     FMT_ABC,   "return R[A]...R[A+B-2]"},
    [OP_RETURN0]    = {"RETURN0",    FMT_NONE,  "return (fast)"},
    [OP_RETURN1]    = {"RETURN1",    FMT_A,     "return R[A] (fast)"},
    
    // Container creation (C=storage mode: 0=normal, 1=shared)
    [OP_NEWARRAY]   = {"NEWARRAY",   FMT_ABC,   "R[A] = [], B=capacity, C=storage"},
    [OP_NEWMAP]     = {"NEWMAP",     FMT_ABC,   "R[A] = #{}, B=capacity, C=storage"},
    [OP_NEWSET]     = {"NEWSET",     FMT_AB,    "R[A] = #[], B=storage"},
    [OP_NEWSTRINGBUILDER] = {"NEWSTRINGBUILDER", FMT_AB, "R[A] = new StringBuilder(), B=storage"},
    
    // Array operations
    [OP_ARRAY_GET]  = {"ARRAY_GET",  FMT_ABC,   "R[A] = R[B]:Array[R[C]]"},
    [OP_ARRAY_GETC] = {"ARRAY_GETC", FMT_ABC,   "R[A] = R[B]:Array[C]"},
    [OP_ARRAY_SET]  = {"ARRAY_SET",  FMT_ABC,   "R[A]:Array[R[B]] = R[C]"},
    [OP_ARRAY_SETC] = {"ARRAY_SETC", FMT_ABC,   "R[A]:Array[B] = R[C]"},
    [OP_ARRAY_PUSH] = {"ARRAY_PUSH", FMT_AB,    "R[A]:Array.push(R[B])"},
    [OP_ARRAY_LEN]  = {"ARRAY_LEN",  FMT_AB,    "R[A] = len(R[B]:Array)"},
    [OP_ARRAY_INIT] = {"ARRAY_INIT", FMT_AB_IMM,"R[A][1..B] = R[A+1..A+B]"},
    
    // Map operations
    [OP_MAP_GET]    = {"MAP_GET",    FMT_ABC,   "R[A] = R[B]:Map[R[C]]"},
    [OP_MAP_GETK]   = {"MAP_GETK",   FMT_ABC,   "R[A] = R[B]:Map[K[C]]"},
    [OP_MAP_SET]    = {"MAP_SET",    FMT_ABC,   "R[A]:Map[R[B]] = R[C]"},
    [OP_MAP_SETK]   = {"MAP_SETK",   FMT_ABC,   "R[A]:Map[K[B]] = R[C]"},
    
    // Generic index operations
    [OP_INDEX_GET]  = {"INDEX_GET",  FMT_ABC,   "R[A] = R[B][R[C]] (runtime dispatch)"},
    [OP_INDEX_SET]  = {"INDEX_SET",  FMT_ABC,   "R[A][R[B]] = R[C] (runtime dispatch)"},
    
    // Slice operations
    [OP_SLICE]      = {"SLICE",      FMT_ABC,   "R[A] = R[B][R[C]:R[C+1]] (slice)"},
    
    // Closure & Upvalues
    [OP_CLOSURE]    = {"CLOSURE",    FMT_PROTO, "R[A] = closure(XrProto[Bx])"},
    [OP_UPVAL_GET]  = {"UPVAL_GET",  FMT_ABC,   "R[A] = cl->upvals[B]"},
    [OP_CELL_NEW]   = {"CELL_NEW",   FMT_A,     "R[A] = new_cell(R[A])"},
    [OP_CELL_GET]   = {"CELL_GET",   FMT_ABC,   "R[A] = cell_deref(R[B])"},
    [OP_CELL_SET]   = {"CELL_SET",   FMT_ABC,   "cell_store(R[A], R[B])"},
    
    // OOP - Class building
    [OP_CLASS_CREATE_FROM_DESCRIPTOR] = {"CLASS_FROM_DESC", FMT_ABx, "R[A] = Class.from_descriptor(K[Bx])"},
    [OP_CLINIT_CALL]            = {"CLINIT_CALL",          FMT_A,    "call static init <clinit>"},
    
    // OOP - Class operations
    [OP_INHERIT]    = {"INHERIT",    FMT_ABC,   "R[A].super = R[B]"},
    [OP_GETPROP]    = {"GETPROP",    FMT_ABC,   "R[A] = R[B].K[C]"},
    [OP_SETPROP]    = {"SETPROP",    FMT_ABC,   "R[A].K[B] = R[C]"},
    [OP_GETSUPER]   = {"GETSUPER",   FMT_ABC,   "R[A] = R[B].super.K[C]"},
    [OP_INVOKE]     = {"INVOKE",     FMT_ABC,   "R[A] = R[B]:method(...)"},
    [OP_INVOKE_TAIL]= {"INVOKE_TAIL",FMT_ABC,   "tail: R[A+1]:method(...) reuse frame"},
    [OP_SUPERINVOKE]= {"SUPERINVOKE",FMT_ABC,   "super.method(...)"},
    
    // OOP - Optimized instructions
    [OP_INVOKE_DIRECT]   = {"INVOKE_DIRECT",   FMT_ABC, "R[A] = R[B]:methods[C](...)"},
    [OP_INVOKE_BUILTIN]  = {"INVOKE_BUILTIN",  FMT_ABC, "R[A] = R[A+1]:builtin[B](nargs=C)"},
    
    // OOP - Runtime support
    [OP_ABSTRACT_ERROR]        = {"ABSTRACT_ERROR",        FMT_NONE,  "runtime: called abstract method"},
    [OP_SET_STORAGE_CTX]       = {"SET_STORAGE_CTX",       FMT_A,     "set storage mode context A=mode"},
    
    // shared conversion
    [OP_TO_SHARED]             = {"TO_SHARED",             FMT_AB,    "R[A] = to_shared(R[B])"},
    
    // Field operations
    [OP_MAP_SETKS]      = {"SETFIELDS",      FMT_AB,   "R[A].fields[0..B-1] = R[A+1]..R[A+B]"},
    
    // Instance field access (O(1))
    [OP_GETFIELD]       = {"GETFIELD",       FMT_ABC,  "R[A] = R[B]:Instance.fields[C]"},
    [OP_SETFIELD]       = {"SETFIELD",       FMT_ABC,  "R[A]:Instance.fields[B] = R[C]"},
    [OP_GETFIELD_IC]    = {"GETFIELD_IC",    FMT_ABC,  "R[A] = R[B].K[C] (IC)"},
    
    // Json dynamic object
    [OP_NEWJSON]        = {"NEWJSON",        FMT_ABx,  "R[A] = new Json(K[Bx]:Shape)"},
    [OP_JSON_GET]       = {"JSON_GET",       FMT_ABC,  "R[A] = R[B].fields[C]"},
    [OP_JSON_SET]       = {"JSON_SET",       FMT_ABC,  "R[A].fields[B] = R[C]"},
    [OP_JSON_GETK]      = {"JSON_GETK",      FMT_ABC,  "R[A] = R[B].get(symbol=C)"},
    [OP_JSON_SETK]      = {"JSON_SETK",      FMT_ABC,  "R[A].set(symbol=B, R[C])"},
    [OP_JSON_INIT]      = {"JSON_INIT",      FMT_ABC,  "R[A].fields[B] = R[C]"},
    [OP_JSON_INIT_I]    = {"JSON_INIT_I",    FMT_ABC,  "R[A].fields[B] = C"},
    [OP_JSON_INIT_N]    = {"JSON_INIT_N",    FMT_ABC,  "R[A].fields[B] = null"},
    
    // Builtin globals (read-only)
    [OP_GETBUILTIN]  = {"GETBUILTIN",  FMT_GLOBAL, "R[A] = builtins[Bx]"},
    
    // Builtin functions
    [OP_PRINT]      = {"PRINT",      FMT_A,     "print(R[A])"},
    [OP_TYPEOF]     = {"TYPEOF",     FMT_AB,    "R[A] = typeof(R[B]) -> int"},
    [OP_TYPENAME]   = {"TYPENAME",   FMT_AB,    "R[A] = typename(R[B]) -> string"},
    [OP_DUMP]       = {"DUMP",       FMT_AB,    "dump(R[A], indent=B)"},
    [OP_TOINT]      = {"TOINT",      FMT_AB,    "R[A] = int(R[B])"},
    [OP_TOFLOAT]    = {"TOFLOAT",    FMT_AB,    "R[A] = float(R[B])"},
    [OP_TOSTRING]   = {"TOSTRING",   FMT_AB,    "R[A] = string(R[B])"},
    [OP_TOBOOL]     = {"TOBOOL",     FMT_AB,    "R[A] = bool(R[B])"},
    [OP_COPY]       = {"COPY",       FMT_AB,    "R[A] = copy(R[B])"},
    [OP_CHR]        = {"CHR",        FMT_AB,    "R[A] = chr(R[B])"},
    
    // Enum operations
    [OP_ENUM_ACCESS] = {"ENUM_ACCESS", FMT_ABC, "R[A] = R[B].variant[R[C]]"},
    [OP_ENUM_CONVERT]= {"ENUM_CONVERT",FMT_ABC, "R[A] = enum_convert(R[B], R[C])"},
    [OP_ENUM_NAME]  = {"ENUM_NAME",  FMT_AB,    "R[A] = enum_name(R[B])"},
    
    // Exception handling
    [OP_TRY]        = {"TRY",        FMT_SPECIAL, "try block start"},
    [OP_CATCH]      = {"CATCH",      FMT_A,     "catch block"},
    [OP_FINALLY]    = {"FINALLY",    FMT_NONE,  "finally block"},
    [OP_END_TRY]    = {"END_TRY",    FMT_NONE,  "try block end"},
    [OP_THROW]      = {"THROW",      FMT_A,     "throw R[A]"},
    
    // Box/Unbox (typed storage boundary)
    [OP_BOX_I64]   = {"BOX_I64",   FMT_AB,    "R[A] = box(R[B] as i64)"},
    [OP_BOX_F64]   = {"BOX_F64",   FMT_AB,    "R[A] = box(R[B] as f64)"},
    [OP_UNBOX_I64] = {"UNBOX_I64", FMT_AB,    "R[A] = unbox(R[B]) as i64"},
    [OP_UNBOX_F64] = {"UNBOX_F64", FMT_AB,    "R[A] = unbox(R[B]) as f64"},
    
    // Array access without bounds check
    [OP_ARRAY_GET_NOCHECK] = {"ARRAY_GET_NC", FMT_ABC, "R[A] = R[B]:Array[R[C]] (no check)"},
    
    // Map counter optimization
    [OP_MAP_INCREMENT] = {"MAP_INCREMENT", FMT_AB, "R[A]:Map[R[B]]++"},
    
    // String operation optimization
    [OP_SUBSTRING] = {"SUBSTRING", FMT_ABC, "R[A] = R[B].substring(R[C], R[C+1])"},
    [OP_STR_REPEAT] = {"STR_REPEAT", FMT_ABC, "R[A] = R[B] * R[C] (string repeat)"},
    
    // Module system
    [OP_IMPORT] = {"IMPORT", FMT_ABx, "R[A] = import(K[Bx])"},
    [OP_EXPORT] = {"EXPORT", FMT_ABC, "export(K[A], R[B], C=const?)"},
    [OP_EXPORT_ALL] = {"EXPORT_ALL", FMT_A, "export * from R[A]"},
    
    // Assert instructions
    [OP_ASSERT] = {"ASSERT", FMT_ABC, "if !R[A] throw AssertError(K[B]); C=1: negate"},
    [OP_ASSERT_EQ] = {"ASSERT_EQ", FMT_ABC, "if R[A] != R[B] throw AssertError(K[C])"},
    [OP_ASSERT_NE] = {"ASSERT_NE", FMT_ABC, "if R[A] == R[B] throw AssertError(K[C])"},
    
    // Regex literal
    [OP_REGEX_COMPILE] = {"REGEX_COMPILE", FMT_ABC, "R[A] = regex.compile(K[B], K[C])"},
    
    // Coroutine instructions
    [OP_GO]         = {"GO",         FMT_ABC,   "R[A] = go R[B](R[B+1]..R[B+C])"},
    [OP_GO_INVOKE]  = {"GO_INVOKE",  FMT_ABC,   "R[A] = go R[B].method(args)"},
    [OP_SPAWN_CONT] = {"SPAWN_CONT", FMT_NONE,  "spawn continuation"},
    [OP_AWAIT]      = {"AWAIT",      FMT_ABC,   "R[A] = await R[B], C=discard"},
    [OP_AWAIT_TIMEOUT] = {"AWAIT_TIMEOUT", FMT_ABC, "R[A] = await(timeout: R[C]) R[B]"},
    [OP_AWAIT_ALL]  = {"AWAIT_ALL",  FMT_AB,    "R[A] = await R[B]:Array"},
    [OP_AWAIT_ANY]  = {"AWAIT_ANY",  FMT_ABC,   "R[A] = await.any R[B]:Array, C=mode"},
    [OP_YIELD]      = {"YIELD",      FMT_NONE,  "yield"},
    [OP_CANCELLED]  = {"CANCELLED",  FMT_A,     "R[A] = cancelled()"},
    [OP_LOCK_THREAD]  = {"LOCK_THREAD",  FMT_NONE, "Coro.lockThread()"},
    [OP_UNLOCK_THREAD]= {"UNLOCK_THREAD",FMT_NONE, "Coro.unlockThread()"},
    [OP_SET_LOCAL]  = {"SET_LOCAL",  FMT_AB,    "Coro.setLocal(R[A], R[B])"},
    [OP_GET_LOCAL]  = {"GET_LOCAL",  FMT_AB,    "R[A] = Coro.getLocal(R[B])"},
    [OP_SET_PRIORITY]= {"SET_PRIORITY",FMT_AB,   "Coro.setPriority(R[A], R[B])"},
    [OP_CORO_CTRL]  = {"CORO_CTRL",  FMT_ABC,   "coro monitoring, C=sub_op"},
    
    // Channel instructions
    [OP_CHAN_NEW]   = {"CHAN_NEW",   FMT_AB,    "R[A] = Channel(B)"},
    [OP_CHAN_SEND]  = {"CHAN_SEND",  FMT_ABC,   "R[B].send(R[C])"},
    [OP_CHAN_RECV]  = {"CHAN_RECV",  FMT_AB,    "R[A], R[A+1] = R[B].recv()"},
    [OP_CHAN_TRY_SEND] = {"CHAN_TRY_SEND", FMT_ABC, "R[A] = R[B].trySend(R[C])"},
    [OP_CHAN_TRY_RECV] = {"CHAN_TRY_RECV", FMT_AB, "R[A], R[A+1] = R[B].tryRecv()"},
    [OP_CHAN_SEND_TIMEOUT] = {"CHAN_SEND_TIMEOUT", FMT_ABC, "R[A] = R[B].send(R[C], timeout: R[C+1])"},
    [OP_CHAN_RECV_TIMEOUT] = {"CHAN_RECV_TIMEOUT", FMT_ABC, "R[A] = R[B].recv(timeout: R[C])"},
    [OP_CHAN_CLOSE] = {"CHAN_CLOSE", FMT_A,     "R[A].close()"},
    [OP_CHAN_IS_CLOSED] = {"CHAN_IS_CLOSED", FMT_AB, "R[A] = R[B].isClosed()"},
    
    // Select multiplexing
    [OP_SELECT_START] = {"SELECT_START", FMT_NONE, "select start"},
    [OP_SELECT_CASE]  = {"SELECT_CASE",  FMT_ABC,  "case A=type, B=channel, C=value"},
    [OP_SELECT_END]   = {"SELECT_END",   FMT_NONE, "select end"},
    
    // Defer execution
    [OP_DEFER]      = {"DEFER",      FMT_A,     "defer R[A]"},
    
    // Bytes array
    [OP_BYTES_NEW]  = {"BYTES_NEW",  FMT_AB,    "R[A] = Bytes(B args)"},
    
    // Scope structured concurrency
    [OP_SCOPE_ENTER] = {"SCOPE_ENTER", FMT_A,   "enter scope, A=mode(0=wait,1=linked,2=supervisor)"},
    [OP_SCOPE_EXIT]  = {"SCOPE_EXIT",  FMT_AB,   "exit scope, A=mode, B=result_reg"},
    
    // Coroutine-friendly instructions
    [OP_SLEEP]       = {"SLEEP",       FMT_A,     "time.sleep(R[A]) ms"},
    [OP_TIME_AFTER]  = {"TIME_AFTER",  FMT_AB,    "R[A] = time.after(R[B]) ms"},
    [OP_SELECT_BLOCK] = {"SELECT_BLOCK", FMT_AB,   "select block wait"},
    
    // shared variables
    [OP_GETSHARED]  = {"GETSHARED",  FMT_GLOBAL, "R[A] = shared[Bx]"},
    [OP_SETSHARED]  = {"SETSHARED",  FMT_GLOBAL, "shared[Bx] = R[A]"},
    
    // Typed compact storage (raw in, raw out, zero BOX/UNBOX)
    [OP_TARRAY_GET]  = {"TARRAY_GET",  FMT_ABC,  "R[A].i = R[B]:TypedArray[R[C]]"},
    [OP_TARRAY_GETC] = {"TARRAY_GETC", FMT_ABC,  "R[A].i = R[B]:TypedArray[C]"},
    [OP_TARRAY_SET]  = {"TARRAY_SET",  FMT_ABC,  "R[A]:TypedArray[R[B]] = R[C].i"},
    [OP_TARRAY_PUSH] = {"TARRAY_PUSH", FMT_AB,   "R[A]:TypedArray.push(R[B].i)"},
    [OP_TFIELD_GET]  = {"TFIELD_GET",  FMT_ABC,  "R[A].i = R[B]:compact_fields[C]"},
    [OP_TFIELD_SET]  = {"TFIELD_SET",  FMT_ABC,  "R[A]:compact_fields[B] = R[C].i"},

    // Tail recursion loop
    [OP_LOOP_BACK]  = {"LOOP_BACK",  FMT_ABC,   "tail recursion: R[0..B-1]=R[A+1..A+B]; PC=entry"},

    // Struct native storage
    [OP_NEW_STRUCT]  = {"NEW_STRUCT",  FMT_ABx,   "R[A] = alloc struct in struct_area (Bx=layout_id)"},
    [OP_STRUCT_GET]  = {"STRUCT_GET",  FMT_ABC,   "R[A] = struct(R[B]).field[C]"},
    [OP_STRUCT_SET]  = {"STRUCT_SET",  FMT_ABC,   "struct(R[A]).field[B] = R[C]"},
    [OP_STRUCT_COPY] = {"STRUCT_COPY", FMT_AB,    "R[A] = memcpy struct R[B]"},

    // Placeholder
    [OP_NOP]        = {"NOP",        FMT_SPECIAL, "no-op / spawn metadata"},
};

/* ========== Opcode Name Query (for external use) ========== */

// Get opcode name (called by xchunk.c)
const char *xr_debug_opcode_name(OpCode op) {
    XR_DCHECK(sizeof(opcode_table)/sizeof(opcode_table[0]) == NUM_OPCODES,
              "opcode_table size mismatch with NUM_OPCODES");
    if (op >= 0 && op < NUM_OPCODES && opcode_table[op].name != NULL) {
        return opcode_table[op].name;
    }
    return "UNKNOWN";
}

/* ========== Formatting Output Functions ========== */

// Format: no operand
static int disasm_none(const char *name, int offset) {
    printf("%-16s\n", name);
    return offset + 1;
}

// Format: R[A]
static int disasm_a(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    printf("%-16s R[%d]\n", name, a);
    return offset + 1;
}

// Format: R[A] K[Bx] (constant)
static int disasm_abx(const char *name, XrProto *proto, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint16_t bx = GETARG_Bx(inst);
    
    printf("%-16s R[%d] K[%d] ; ", name, a, bx);
    
    // Print constant value
    if (bx < PROTO_CONST_COUNT(proto)) {
        xr_value_print(PROTO_CONSTANT(proto, bx));
    } else {
        printf("???");
    }
    printf("\n");
    
    return offset + 1;
}

// Format: R[A] G[Bx] (global variable)
static int disasm_abx_global(const char *name, XrProto *proto, XrInstruction inst, int offset) {
    (void)proto;
    uint8_t a = GETARG_A(inst);
    uint16_t bx = GETARG_Bx(inst);
    
    printf("%-16s R[%d] G[%d]\n", name, a, bx);
    
    return offset + 1;
}

// Format: R[A] XrProto[Bx] (closure)
static int disasm_proto(const char *name, XrProto *proto, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint16_t bx = GETARG_Bx(inst);
    
    printf("%-16s R[%d] XrProto[%d]", name, a, bx);
    
    // Print function name (if any)
    if (bx < PROTO_PROTO_COUNT(proto)) {
        XrProto *child = PROTO_PROTO(proto, bx);
        if (child != NULL && child->name != NULL) {
            printf(" ; \"%s\"", child->name->data);
        }
    }
    printf("\n");
    
    return offset + 1;
}

// Format: R[A] R[B] R[C]
static int disasm_abc(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint8_t b = GETARG_B(inst);
    uint8_t c = GETARG_C(inst);
    
    printf("%-16s R[%d] R[%d] R[%d]\n", name, a, b, c);
    return offset + 1;
}

// Format: R[A] R[B]
static int disasm_ab(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint8_t b = GETARG_B(inst);
    
    printf("%-16s R[%d] R[%d]\n", name, a, b);
    return offset + 1;
}

// Format: R[A] <Bx as raw integer> (Bx is a direct integer, not a const index)
static int disasm_abx_int(const char *name, XrInstruction inst, int offset) {
    uint8_t  a  = GETARG_A(inst);
    uint16_t bx = GETARG_Bx(inst);
    printf("%-16s R[%d] %d\n", name, a, (int)bx);
    return offset + 1;
}

// Format: R[A] B (B as immediate)
static int disasm_ab_imm(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint8_t b = GETARG_B(inst);
    uint8_t c = GETARG_C(inst);
    
    // Check if comparison instruction (need to show k flag)
    OpCode op = GET_OPCODE(inst);
    if (op == OP_EQ || op == OP_EQK || op == OP_LT || op == OP_LE || 
        op == OP_TEST) {
        printf("%-16s R[%d] R[%d] k=%d\n", name, a, b, c);
    } else {
    printf("%-16s R[%d] %d\n", name, a, b);
    }
    return offset + 1;
}

// Format: R[A] sBx (signed immediate)
static int disasm_asbx(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    int sbx = GETARG_sBx(inst);
    
    printf("%-16s R[%d] %d\n", name, a, sbx);
    return offset + 1;
}

// Format: R[A] R[B] sC (B is register, C is signed immediate)
static int disasm_ab_sc(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    uint8_t b = GETARG_B(inst);
    int sc = GETARG_sC(inst);
    
    printf("%-16s R[%d] R[%d] %d\n", name, a, b, sc);
    return offset + 1;
}

// Format: R[A] sB C (B is signed immediate, C is condition flag)
static int disasm_asb_c(const char *name, XrInstruction inst, int offset) {
    uint8_t a = GETARG_A(inst);
    int sb = GETARG_sB(inst);
    uint8_t k = GETARG_C(inst);
    
    printf("%-16s R[%d] %d k=%d\n", name, a, sb, k);
    return offset + 1;
}

// Format: sJ (jump)
static int disasm_sj(const char *name, XrInstruction inst, int offset) {
    int sj = GETARG_sJ(inst);
    int target = offset + 1 + sj;
    
    printf("%-16s %d -> %d\n", name, sj, target);
    return offset + 1;
}

// Special format handling (OP_TRY, etc.)
static int disasm_special(const char *name, XrProto *proto, XrInstruction inst, int offset) {
    OpCode op = GET_OPCODE(inst);
    
    if (op == OP_TRY) {
        // OP_TRY is followed by an instruction containing finally offset
        uint16_t catch_offset = GETARG_Bx(inst);
        printf("%-16s catch=%d ", name, catch_offset);
        
        // Read next instruction (finally offset)
        if (offset + 1 < PROTO_CODE_COUNT(proto)) {
            XrInstruction next_inst = PROTO_CODE(proto, offset + 1);
            uint16_t finally_offset = GETARG_Bx(next_inst);
            printf("finally=%d\n", finally_offset);
        }
        return offset + 2;
    }
    
    // NOP with spawn metadata
    if (op == OP_NOP) {
        uint8_t a = GETARG_A(inst);
        uint16_t bx = GETARG_Bx(inst);
        if (a == 1) {
            // Coroutine name
            printf("%-16s ; name=K[%d]", name, bx);
            if (bx < PROTO_CONST_COUNT(proto)) {
                printf(" \"");
                xr_value_print(PROTO_CONSTANT(proto, bx));
                printf("\"");
            }
            printf("\n");
        } else if (a == 2) {
            // Coroutine priority
            const char *prio = bx == 0 ? "LOW" : bx == 1 ? "NORMAL" : bx == 2 ? "HIGH" : "?";
            printf("%-16s ; priority=%s(%d)\n", name, prio, bx);
        } else if (a == 3) {
            // Link mode
            const char *mode = bx == 1 ? "LINKED" : bx == 2 ? "MONITORED" : "?";
            printf("%-16s ; link_mode=%s(%d)\n", name, mode, bx);
        } else {
            printf("%-16s\n", name);
        }
        return offset + 1;
    }
    
    // Other special instructions
    printf("%-16s\n", name);
    return offset + 1;
}

/* ========== Value Printing ========== */

// Print constants table
void xr_print_constants(XrProto *proto) {
    XR_DCHECK(proto != NULL, "xr_print_constants: NULL proto");
    int count = PROTO_CONST_COUNT(proto);
    if (count == 0) {
        return;
    }
    
    printf("Constants:\n");
    for (int i = 0; i < count; i++) {
        printf("  K[%d] = ", i);
        xr_value_print(PROTO_CONSTANT(proto, i));
        printf("\n");
    }
}

/* ========== Disassembly API ========== */

// Disassemble single instruction (table-driven)
int xr_disassemble_instruction(XrProto *proto, int offset) {
    XR_DCHECK(proto != NULL, "xr_disassemble_instruction: NULL proto");
    // Print offset
    printf("%04d ", offset);
    
    // Print line number
    int lineinfo_count = DYNARRAY_COUNT(&proto->lineinfo);
    if (offset > 0 && offset < lineinfo_count &&
        PROTO_LINE(proto, offset) == PROTO_LINE(proto, offset - 1)) {
        printf("   | ");
    } else if (offset < lineinfo_count) {
        printf("%4d ", PROTO_LINE(proto, offset));
    } else {
        printf("   ? ");
    }
    
    // Get instruction
    XrInstruction inst = PROTO_CODE(proto, offset);
    OpCode op = GET_OPCODE(inst);
    
    // Table-driven: lookup metadata
    if (op >= NUM_OPCODES || opcode_table[op].name == NULL) {
        printf("UNKNOWN [opcode=%d]\n", op);
        return offset + 1;
    }
    
    const OpCodeInfo *info = &opcode_table[op];
    
    // Dispatch by format
    switch (info->format) {
        case FMT_NONE:
            return disasm_none(info->name, offset);
            
        case FMT_A:
            return disasm_a(info->name, inst, offset);
            
        case FMT_AB:
            return disasm_ab(info->name, inst, offset);
            
        case FMT_ABC:
            return disasm_abc(info->name, inst, offset);
            
        case FMT_ABx:
            return disasm_abx(info->name, proto, inst, offset);
            
        case FMT_AsBx:
            return disasm_asbx(info->name, inst, offset);
            
        case FMT_sJ:
            return disasm_sj(info->name, inst, offset);
            
        case FMT_AB_sC:
            return disasm_ab_sc(info->name, inst, offset);
            
        case FMT_AsB_C:
            return disasm_asb_c(info->name, inst, offset);
            
        case FMT_AB_IMM:
            return disasm_ab_imm(info->name, inst, offset);
            
        case FMT_ABx_INT:
            return disasm_abx_int(info->name, inst, offset);
            
        case FMT_PROTO:
            return disasm_proto(info->name, proto, inst, offset);
            
        case FMT_GLOBAL:
            return disasm_abx_global(info->name, proto, inst, offset);
            
        case FMT_SPECIAL:
            return disasm_special(info->name, proto, inst, offset);
            
        default:
            printf("INVALID_FORMAT [op=%d, fmt=%d]\n", op, info->format);
            return offset + 1;
    }
}

// Disassemble entire function prototype
void xr_disassemble_proto(XrProto *proto, const char *name) {
    XR_DCHECK(proto != NULL, "xr_disassemble_proto: NULL proto");
    printf("== ");
    if (name != NULL) {
        printf("%s ", name);
    } else if (proto->name != NULL) {
        printf("%s ", proto->name->data);
    } else {
        printf("<script> ");
    }
    printf("==\n");
    
    // Print function info
    printf("Parameters: %d, Stack: %d, Code: %d",
           proto->numparams, proto->maxstacksize, PROTO_CODE_COUNT(proto));
    if (proto->return_type != NULL) {
        printf(", Returns: %s", proto->return_type);
    }
    printf("\n");
    
    // Print param_types coverage
    if (proto->param_types && proto->param_types_count > 0) {
        int typed = 0;
        for (int i = 0; i < proto->param_types_count; i++) {
            if (proto->param_types[i]) typed++;
        }
        printf("ParamTypes: %d/%d typed", typed, proto->param_types_count);
        if (typed > 0) {
            printf(" [");
            for (int i = 0; i < proto->param_types_count; i++) {
                if (proto->param_types[i]) {
                    uint8_t tag = xr_type_to_slot_type(proto->param_types[i]);
                    const char *name = (tag == XR_SLOT_I64) ? "i64" :
                                       (tag == XR_SLOT_F64) ? "f64" :
                                       (tag == XR_SLOT_PTR) ? "ptr" :
                                       (tag == XR_SLOT_BOOL) ? "bool" : "unknown";
                    printf("R%d=%s ", i, name);
                }
            }
            printf("]");
        }
        printf("\n");
    }
    
    // Print constants table
    if (PROTO_CONST_COUNT(proto) > 0) {
        xr_print_constants(proto);
        printf("\n");
    }
    
    // Disassemble all instructions
    int code_count = PROTO_CODE_COUNT(proto);
    for (int offset = 0; offset < code_count;) {
        offset = xr_disassemble_instruction(proto, offset);
    }
    
    // Disassemble nested functions
    int proto_count = PROTO_PROTO_COUNT(proto);
    if (proto_count > 0) {
        printf("\nNested functions:\n");
        for (int i = 0; i < proto_count; i++) {
            XrProto *child = PROTO_PROTO(proto, i);
            if (child != NULL) {
                printf("\n");
                xr_disassemble_proto(child, NULL);
            } else {
                printf("\n[Null proto at index %d]\n", i);
            }
        }
    }
}

