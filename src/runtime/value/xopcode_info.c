/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xopcode_info.c - Instruction metadata table
 *
 * NOTE: Keep entries in sync with OpCode enum (see xchunk.h). Missing
 *   entries default-initialize to {NULL, FMT_NONE, NULL}; xr_opcode_info
 *   treats those as UNKNOWN.
 */

#include "xopcode_info.h"
#include "../../base/xchecks.h"

static const XrOpCodeInfo opcode_table[NUM_OPCODES] = {
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

static const XrOpCodeInfo k_unknown_info = {"UNKNOWN", FMT_SPECIAL, NULL};

const XrOpCodeInfo *xr_opcode_info(OpCode op) {
    XR_DCHECK(sizeof(opcode_table) / sizeof(opcode_table[0]) == NUM_OPCODES,
              "opcode_table size mismatch with NUM_OPCODES");
    if (op >= 0 && op < NUM_OPCODES && opcode_table[op].name != NULL) {
        return &opcode_table[op];
    }
    return &k_unknown_info;
}

const char *xr_opcode_name(OpCode op) {
    return xr_opcode_info(op)->name;
}
