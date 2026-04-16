/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xchunk.h - Bytecode chunk for register-based VM
 *
 * KEY CONCEPT:
 *   32-bit instruction encoding with multiple formats.
 *   Supports up to 256 opcodes, 256 registers.
 *
 * INSTRUCTION FORMATS:
 *    31           23           15            7    0
 *    +------------+------------+------------+----+
 *    |      C     |      B     |      A     | OP | iABC
 *    +------------+------------+------------+----+
 *    |            Bx           |      A     | OP | iABx
 *    +------------+------------+------------+----+
 *    |            sBx          |      A     | OP | iAsBx
 *    +------------+------------+------------+----+
 *    |            Ax                        | OP | iAx
 *    +------------+------------+------------+----+
 *    |            sJ                        | OP | isJ
 *    +------------+------------+------------+----+
 *
 * FIELD SIZES:
 *   OP:  8-bit  (256 opcodes)
 *   A:   8-bit  (256 registers)
 *   B:   8-bit
 *   C:   8-bit
 *   Bx:  16-bit (65536 constants)
 *   sBx: 16-bit signed (-32768 to 32767)
 *   Ax:  24-bit
 *   sJ:  24-bit signed (jump offset)
 */

#ifndef XCHUNK_H
#define XCHUNK_H

#include "xvalue.h"
#include "../../base/xdynarray.h"
#include <stdint.h>
#include "../../base/xdefs.h"

typedef struct XrString XrString;

struct XrICMethodTable;
struct XrICFieldTable;

/* ========== 32-bit Instruction Type ========== */

typedef uint32_t XrInstruction;

/* ========== Instruction Format ========== */

#define SIZE_OP     8
#define SIZE_A      8
#define SIZE_B      8
#define SIZE_C      8
#define SIZE_Bx     16
#define SIZE_Ax     24

#define MAXARG_A    ((1 << SIZE_A) - 1)
#define MAXARG_B    ((1 << SIZE_B) - 1)
#define MAXARG_C    ((1 << SIZE_C) - 1)
#define MAXARG_Bx   ((1 << SIZE_Bx) - 1)
#define MAXARG_sBx  (MAXARG_Bx >> 1)
#define MAXARG_Ax   ((1 << SIZE_Ax) - 1)
#define MAXARG_sJ   ((1 << (SIZE_Ax - 1)) - 1)

// OP_LOADI immediate range (sBx format)
#define LOADI_MAX   MAXARG_sBx
#define LOADI_MIN   (-(MAXARG_sBx) - 1)

/* ========== Opcode Enum ========== */

typedef enum {
    /* === Load/Move === */
    OP_MOVE, // R[A] = R[B]
    OP_LOADI, // R[A] = sBx (integer immediate)
    OP_LOADF, // R[A] = (float)sBx
    OP_LOADK, // R[A] = K[Bx] (constant)
    OP_LOADNULL, // R[A] = ... = R[A+B] = null
    OP_LOADTRUE, // R[A] = true
    OP_LOADFALSE, // R[A] = false
    
    /* === Arithmetic === */
    OP_ADD, // R[A] = R[B] + R[C]
    OP_ADDI, // R[A] = R[B] + sC (immediate)
    OP_ADDK, // R[A] = R[B] + K[C] (constant)
    OP_SUB, // R[A] = R[B] - R[C]
    OP_SUBI, // R[A] = R[B] - sC
    OP_SUBK, // R[A] = R[B] - K[C]
    OP_MUL, // R[A] = R[B] * R[C]
    OP_MULI, // R[A] = R[B] * sC
    OP_MULK, // R[A] = R[B] * K[C]
    OP_DIV, // R[A] = R[B] / R[C]
    OP_DIVK, // R[A] = R[B] / K[C]
    OP_MOD, // R[A] = R[B] % R[C]
    OP_MODK, // R[A] = R[B] % K[C]
    OP_UNM, // R[A] = -R[B]
    OP_NOT, // R[A] = not R[B]
    
    /* === String Buffer === */
    OP_STRBUF_NEW, // R[A] = new_strbuf()
    OP_STRBUF_APPEND, // strbuf(R[A]).append(R[B])
    OP_STRBUF_FINISH, // R[A] = strbuf(R[A]).to_string()
    
    /* === Bitwise === */
    OP_BAND, // R[A] = R[B] & R[C]
    OP_BOR, // R[A] = R[B] | R[C]
    OP_BXOR, // R[A] = R[B] ^ R[C]
    OP_BNOT, // R[A] = ~R[B]
    OP_SHL, // R[A] = R[B] << R[C]
    OP_SHR, // R[A] = R[B] >> R[C]
    
    /* === Comparison === */
    OP_EQ, // if (R[A] == R[B]) != k then PC++
    OP_EQK, // if (R[A] == K[B]) != k then PC++
    OP_EQI, // if (R[A] == sB) != k then PC++
    OP_LT, // if (R[A] < R[B]) != k then PC++
    OP_LTI, // if (R[A] < sB) != k then PC++
    OP_LE, // if (R[A] <= R[B]) != k then PC++
    OP_LEI, // if (R[A] <= sB) != k then PC++
    
    /* === Comparison (expression) === */
    OP_CMP_EQ, // R[A] = (R[B] == R[C]) ? true : false (deep compare)
    OP_CMP_NE, // R[A] = (R[B] != R[C]) ? true : false (deep compare)
    OP_CMP_EQ_STRICT, // R[A] = (R[B] === R[C]) ? true : false (reference compare)
    OP_CMP_NE_STRICT, // R[A] = (R[B] !== R[C]) ? true : false (reference compare)
    OP_CMP_LT, // R[A] = (R[B] < R[C]) ? true : false
    OP_CMP_LE, // R[A] = (R[B] <= R[C]) ? true : false
    OP_IS, // R[A] = (R[B] is Type[C]) ? true : false (runtime type check)
    OP_CHECKTYPE, // assert R[A] is Type[K(B)]; throw TypeError if not (Json→concrete coercion)
    OP_ISNULL, // if (R[A] == null) != k then PC++
    OP_ISNULL_SET, // R[A] = (R[B] == null)
    
    /* === Control Flow === */
    OP_JMP, // PC += sJ
    OP_TEST, // if (R[A]) != k then PC++
    OP_TESTSET, // if (R[B]) != k then PC++ else R[A] = R[B]
    OP_CALL, // R[A]...R[A+C-2] = R[A](R[A+1]...R[A+B-1])
    OP_CALL_KEEP, // R[C] = R[A](R[A+1]...R[A+B]); R[A] preserved
    OP_CALL_STATIC, // R[A](R[A+1]...R[A+B-1]) - known closure, skip callable type check
    OP_CALLSELF, // R[A]...R[A+C-2] = self(R[A+1]...R[A+B-1]) - recursive call
    OP_TAILCALL, // R[A](R[A+1]...R[A+B-1]) - tail call
    OP_RETURN, // return R[A]...R[A+B-2]
    OP_RETURN0, // return (no values) - fast path
    OP_RETURN1, // return R[A] (single value) - fast path
    
    /* === Container Creation === */
    OP_NEWARRAY, // R[A] = [], B=capacity hint
    OP_NEWMAP, // R[A] = #{}, B=capacity hint
    OP_NEWSET, // R[A] = #[]
    OP_NEWSTRINGBUILDER, // R[A] = new StringBuilder()
    OP_NEWRANGE, // R[A] = Range(R[B], R[C]) - lazy integer range
    OP_RANGE_UNPACK, // R[A]=start, R[A+1]=end, R[A+2]=step from Range R[B]
    
    /* === Array Operations === */
    OP_ARRAY_GET, // R[A] = R[B]:Array[R[C]]
    OP_ARRAY_GETC, // R[A] = R[B]:Array[C]
    OP_ARRAY_SET, // R[A]:Array[R[B]] = R[C]
    OP_ARRAY_SETC, // R[A]:Array[B] = R[C]
    OP_ARRAY_PUSH, // R[A]:Array.push(R[B])
    OP_ARRAY_LEN, // R[A] = len(R[B]:Array)
    OP_ARRAY_INIT, // R[A][1..B] = R[A+1..A+B] - batch init
    
    /* === Map Operations === */
    OP_MAP_GET, // R[A] = R[B]:Map[R[C]]
    OP_MAP_GETK, // R[A] = R[B]:Map[K[C]]
    OP_MAP_SET, // R[A]:Map[R[B]] = R[C]
    OP_MAP_SETK, // R[A]:Map[K[B]] = R[C]
    
    /* === Generic Index === */
    OP_INDEX_GET, // R[A] = R[B][R[C]] - runtime dispatch
    OP_INDEX_SET, // R[A][R[B]] = R[C] - runtime dispatch
    
    /* === Slice === */
    OP_SLICE, // R[A] = R[B][R[C]:R[C+1]], -1=omitted
    
    /* === Closure & Upvalues (flat capture model) === */
    OP_CLOSURE, // R[A] = closure(PROTO[Bx]), populate upvals[] from descriptors
    OP_UPVAL_GET, // R[A] = cl->upvals[B] (flat upvalue read)
    OP_CELL_NEW, // R[A] = new_cell(R[A]) — wrap value in heap-allocated cell
    OP_CELL_GET, // R[A] = cell_deref(R[B]) — read cell value
    OP_CELL_SET, // cell_store(R[A], R[B]) — write cell value
    
    /* === OOP - Class Building === */
    OP_CLASS_CREATE_FROM_DESCRIPTOR, // R[A] = Class.from_descriptor(K[Bx])
    OP_CLINIT_CALL, // call R[A] class static constructor <clinit>
    
    /* === OOP - Class Operations === */
    OP_INHERIT, // R[A].super = R[B]
    OP_GETPROP, // R[A] = R[B].K[C]
    OP_SETPROP, // R[A].K[B] = R[C]
    OP_GETSUPER, // R[A] = R[B].super.K[C]
    OP_INVOKE, // R[A] = R[B]:method(...) - VTable dispatch
    OP_INVOKE_TAIL, // tail call: R[A+1]:method(...) reuse frame
    OP_SUPERINVOKE, // super.method(...)
    
    /* === OOP - Optimized Instructions === */
    OP_INVOKE_DIRECT, // R[A] = R[B]:methods[C](...) - direct call (type known)
    OP_INVOKE_BUILTIN, // R[A] = R[A+1]:builtin_method[B](args)
    
    /* === OOP - Runtime Support === */
    OP_ABSTRACT_ERROR, // abstract method call error
    OP_SET_STORAGE_CTX, // set storage context A=mode (0=normal,1=shared)
    
    /* === Shared Conversion === */
    OP_TO_SHARED, // R[A] = to_shared(R[B])
    OP_MAP_SETKS, // R[A].fields[0..B-1] = R[A+1]..R[A+B] - batch set
    
    /* === Instance Field Access (O(1)) === */
    OP_GETFIELD, // R[A] = R[B]:Instance.fields[C]
    OP_SETFIELD, // R[A]:Instance.fields[B] = R[C]
    OP_GETFIELD_IC, // R[A] = R[B].K[C] - inline cache
    
    /* === Json Dynamic Object === */
    OP_NEWJSON, // R[A] = new Json(K[Bx]:Shape)
    OP_JSON_GET, // R[A] = R[B].fields[C]
    OP_JSON_SET, // R[A].fields[B] = R[C]
    OP_JSON_GETK, // R[A] = R[B].get(C) - C is Symbol ID
    OP_JSON_SETK, // R[A].set(B, R[C]) - B is Symbol ID
    OP_JSON_INIT, // R[A].fields[B] = R[C] - init
    OP_JSON_INIT_I, // R[A].fields[B] = SignedC - init with immediate int
    OP_JSON_INIT_N, // R[A].fields[B] = null - init with null
    
    /* === Builtin Globals (read-only, 18 predefined slots) === */
    OP_GETBUILTIN, // R[A] = builtins[Bx]
    
    /* === Builtin Functions === */
    OP_PRINT, // print(R[A]), B=add_space, C=newline
    OP_TYPEOF, // R[A] = typeof(R[B]) → returns int (XrTypeId)
    OP_TYPENAME, // R[A] = typename(R[B]) → returns string
    OP_DUMP, // dump(R[A], indent=B)
    OP_TOINT, // R[A] = int(R[B])
    OP_TOFLOAT, // R[A] = float(R[B])
    OP_TOSTRING, // R[A] = string(R[B])
    OP_TOBOOL, // R[A] = bool(R[B])
    OP_COPY, // R[A] = copy(R[B])
    OP_CHR, // R[A] = chr(R[B])
    
    /* === Enum Operations === */
    OP_ENUM_ACCESS, // R[A] = R[B].variant[R[A+1]]
    OP_ENUM_CONVERT, // R[A] = enum_convert(R[B], R[A+1])
    OP_ENUM_NAME, // R[A] = enum_name(R[B])
    
    /* === Exception Handling === */
    OP_TRY, // set exception handler: try { ... }
    OP_CATCH, // catch exception: catch (e) { ... }
    OP_FINALLY, // finally block: finally { ... }
    OP_END_TRY, // end try-catch block
    OP_THROW, // throw exception: throw R[A]
    
    /* === Register Spill === */
    OP_SPILL, // S[A] = R[B] - spill register to slot
    OP_RELOAD, // R[A] = S[B] - reload from slot
    
    /* === Box/Unbox (typed storage ↔ tagged boundary) === */
    OP_BOX_I64, // R[A] = XR_FROM_INT(R[B].i)
    OP_BOX_F64, // R[A] = XR_FROM_FLOAT(R[B].f)
    OP_UNBOX_I64, // R[A].i = XR_TO_INT(R[B])
    OP_UNBOX_F64, // R[A].f = XR_TO_FLOAT(R[B])
    
    /* === Unchecked Array Access (loop optimization) === */
    OP_ARRAY_GET_NOCHECK, // R[A] = R[B]:Array[R[C]] - proven safe
    
    /* === Map Counter === */
    OP_MAP_INCREMENT, // R[A]:Map[R[B]]++ - set to 1 if not exists
    
    /* === String Optimization === */
    OP_SUBSTRING, // R[A] = R[B].substring(R[C], R[C+1])
    OP_STR_REPEAT, // R[A] = R[B] * R[C] - string repeat
    
    /* === Module System === */
    OP_IMPORT, // R[A] = import(K[Bx])
    OP_EXPORT, // export(K[A], R[B])
    OP_EXPORT_ALL, // export * from R[A]
    
    /* === Assertion (test framework) === */
    OP_ASSERT, // if !R[A] then throw AssertError(K[B]); C=1: assert_false (negate)
    OP_ASSERT_EQ, // if R[A] != R[B] then throw AssertError(K[C])
    OP_ASSERT_NE, // if R[A] == R[B] then throw AssertError(K[C])
    
    /* === Regex Literal === */
    OP_REGEX_COMPILE, // R[A] = regex.compile(K[B], K[C])
    
    /* === Coroutine === */
    OP_GO, // R[A] = go R[B](R[B+1]..R[B+C]), C=arg count
    OP_GO_INVOKE, // R[A] = go R[B].method(args), B=symbol, C=arg_count
    OP_SPAWN_CONT, // scope-internal go: continuation stealing (Phase 5)
    OP_AWAIT, // R[A] = await R[B]
    OP_AWAIT_TIMEOUT, // R[A] = await(timeout: R[C]) R[B]
    OP_AWAIT_ALL, // R[A] = await R[B]:Array - wait all
    OP_AWAIT_ANY, // R[A] = await.any R[B]:Array, C=mode (0=any, 1=anySuccess)
    OP_YIELD, // yield
    OP_CANCELLED, // R[A] = cancelled()
    OP_LOCK_THREAD, // Coro.lockThread() - pin coro to current worker
    OP_UNLOCK_THREAD, // Coro.unlockThread() - unpin coro
    OP_SET_LOCAL, // Coro.setLocal(R[A], R[B])
    OP_GET_LOCAL, // R[A] = Coro.getLocal(R[B])
    OP_SET_PRIORITY, // Coro.setPriority(R[A], R[B])
    OP_CORO_CTRL, // coroutine monitoring: A=result/arg, B=arg2, C=sub_op
    
    /* === Channel === */
    OP_CHAN_NEW, // R[A] = Channel(B), B=buffer size (0=unbuffered)
    OP_CHAN_NEW_NAMED, // R[A] = Channel(R[B], R[C]) - Named Channel, B=size reg, C=name reg
    OP_CHAN_SEND, // R[B].send(R[C]) - blocking send, R[A] = null on success
    OP_CHAN_RECV, // R[A], R[A+1] = R[B].recv() -> (value, ok)
    OP_CHAN_TRY_SEND, // R[A] = R[B].trySend(R[C]) -> bool
    OP_CHAN_TRY_RECV, // R[A], R[A+1] = R[B].tryRecv() -> (value, ok)
    OP_CHAN_SEND_TIMEOUT, // R[A] = R[B].send(R[C], timeout: R[D]) -> bool
    OP_CHAN_RECV_TIMEOUT, // R[A] = R[B].recv(timeout: R[C]) -> null on timeout
    OP_CHAN_CLOSE, // R[A].close()
    OP_CHAN_IS_CLOSED, // R[A] = R[B].isClosed()
    
    /* === Select === */
    OP_SELECT_START, // start select block
    OP_SELECT_CASE, // add case: A=type(0=recv,1=send), B=channel, C=value/target
    OP_SELECT_END, // execute select
    
    /* === Defer === */
    OP_DEFER, // defer R[A]
    
    /* === Bytes === */
    OP_BYTES_NEW, // R[A] = Bytes(R[A+1..A+B]), B=arg count
    
    /* === Structured Concurrency === */
    OP_SCOPE_ENTER, // enter scope
    OP_SCOPE_EXIT, // exit scope, wait for child coroutines
    
    /* === Coroutine-friendly === */
    OP_SLEEP, // time.sleep(R[A]) - yields CPU
    OP_TIME_AFTER, // R[A] = time.after(R[B]) - timer channel
    OP_SELECT_BLOCK, // select block wait: A=channel array base, B=count
    
    /* === Shared Variables === */
    OP_GETSHARED, // R[A] = shared_array[Bx]
    OP_SETSHARED, // shared_array[Bx] = R[A]
    
    /* === Typed Compact Storage (raw in, raw out, zero BOX/UNBOX) === */
    OP_TARRAY_GET, // R[A].i = R[B]:TypedArray[R[C]] (raw output)
    OP_TARRAY_GETC, // R[A].i = R[B]:TypedArray[C] (constant index)
    OP_TARRAY_SET, // R[A]:TypedArray[R[B]] = R[C].i (raw input)
    OP_TARRAY_PUSH, // R[A]:TypedArray.push(R[B].i) (raw input)
    OP_TFIELD_GET, // R[A].i = R[B]:compact_fields[C] (raw output)
    OP_TFIELD_SET, // R[A]:compact_fields[B] = R[C].i (raw input)

    /* === Instance Reified Type Args === */
    OP_INST_TYPE_ARGS, // R[A]:Instance.gc.extra = packed type args from Bx

    /* === Tail Recursion Loop === */
    OP_LOOP_BACK, // close upvals; R[0..B-1] = R[A+1..A+B]; PC = entry

    /* === Struct Native Storage === */
    OP_NEW_STRUCT, // R[A] = alloc struct in struct_area (Bx = layout_id)
    OP_STRUCT_GET, // R[A] = struct(R[B]).field[C] (native read + box)
    OP_STRUCT_SET, // struct(R[A]).field[B] = R[C] (unbox + native write)
    OP_STRUCT_COPY, // R[A] = memcpy of struct R[B] in struct_area

    /* === Placeholder === */
    OP_NOP, // no-op
    
} OpCode;

// Opcode count (instruction encoding uses 8-bit opcode field, max 256)
#define NUM_OPCODES (OP_NOP + 1)
_Static_assert(NUM_OPCODES <= 256, "Opcode count exceeds 8-bit encoding limit (max 256)");

/* ========== Sub-opcode Constants ========== */

// OP_CORO_CTRL sub-opcodes (C field)
#define CORO_CTRL_STATS         0
#define CORO_CTRL_LIST          1
#define CORO_CTRL_INFO          2
#define CORO_CTRL_DUMP          3
#define CORO_CTRL_STALLED       4
#define CORO_CTRL_DEADLOCKS     5
#define CORO_CTRL_TOP           6
#define CORO_CTRL_GROUP_BY      7
// Registry/lifecycle (merged from stdlib/coro)
#define CORO_CTRL_WHEREIS       8
#define CORO_CTRL_MONITOR       9
#define CORO_CTRL_DEMONITOR     10
#define CORO_CTRL_SELF          11
#define CORO_CTRL_KILL          12

// OP_JSON_INIT mode flags (encoded in high bits of C)
#define JSON_INIT_REG     0 // C = register index
#define JSON_INIT_IMM     1 // C = signed immediate int (use GETARG_sC)
#define JSON_INIT_NULL    2 // C ignored, value = null

/* ========== OP_INVOKE_BUILTIN Type Hints ========== */

typedef enum {
    BUILTIN_TYPE_MAP    = 0,
    BUILTIN_TYPE_ARRAY  = 1,
    BUILTIN_TYPE_STRING = 2,
    BUILTIN_TYPE_SET    = 3,
    BUILTIN_TYPE_INT    = 4,
    BUILTIN_TYPE_FLOAT  = 5,
} BuiltinTypeHint;

/* ========== Instruction Encode/Decode Macros ========== */
#define GET_OPCODE(i)   ((OpCode)((i) & 0xFFu))

// Create instruction
// Note: Use unsigned constants (0xFFu, 0xFFFFu) to avoid signed integer overflow
// when left-shifting. This is important for UBSan compliance.
#define CREATE_ABC(op, a, b, c) \
    ((XrInstruction)(((op) & 0xFFu) | \
                   (((unsigned)(a) & 0xFFu) << 8) | \
                   (((unsigned)(b) & 0xFFu) << 16) | \
                   (((unsigned)(c) & 0xFFu) << 24)))

#define CREATE_ABx(op, a, bx) \
    ((XrInstruction)(((op) & 0xFFu) | \
                   (((unsigned)(a) & 0xFFu) << 8) | \
                   (((unsigned)(bx) & 0xFFFFu) << 16)))

#define CREATE_AsBx(op, a, sbx) \
    CREATE_ABx(op, a, (sbx) + MAXARG_sBx)

#define CREATE_Ax(op, ax) \
    ((XrInstruction)(((op) & 0xFFu) | \
                   (((unsigned)(ax) & 0xFFFFFFu) << 8)))

#define CREATE_sJ(op, sj) \
    CREATE_Ax(op, (sj) + MAXARG_sJ)

// Extract arguments
#define GETARG_A(i)    (((i) >> 8)  & 0xFF)
#define GETARG_B(i)    (((i) >> 16) & 0xFF)
#define GETARG_C(i)    (((i) >> 24) & 0xFF)
#define GETARG_sB(i)   ((int8_t)(GETARG_B(i)))  // signed B
#define GETARG_sC(i)   ((int8_t)(GETARG_C(i)))  // signed C
#define GETARG_Bx(i)   (((i) >> 16) & 0xFFFF)
#define GETARG_sBx(i)  ((int)(GETARG_Bx(i)) - MAXARG_sBx)
#define GETARG_Ax(i)   (((i) >> 8)  & 0xFFFFFF)
#define GETARG_sJ(i)   ((int)(GETARG_Ax(i)) - MAXARG_sJ)

// Set arguments (modify instruction)
// Note: Use unsigned constants to avoid signed integer overflow
#define SETARG_A(i, v)  ((i) = ((i) & ~(0xFFu << 8))  | (((unsigned)(v) & 0xFFu) << 8))
#define SETARG_B(i, v)  ((i) = ((i) & ~(0xFFu << 16)) | (((unsigned)(v) & 0xFFu) << 16))
#define SETARG_C(i, v)  ((i) = ((i) & ~(0xFFu << 24)) | (((unsigned)(v) & 0xFFu) << 24))
#define SETARG_Bx(i, v) ((i) = ((i) & ~(0xFFFFu << 16)) | (((unsigned)(v) & 0xFFFFu) << 16))

/* ========== Constant Table ========== */

/*
** Constant array (using new dynamic array implementation)
** Preserve compatible interface, use XrDynArray internally
*/
typedef XrDynArray ValueArray;

// Constant table operations
XR_FUNC void xr_valuearray_init(ValueArray *array);
XR_FUNC void xr_valuearray_free(ValueArray *array);
XR_FUNC int xr_valuearray_add(ValueArray *array, XrValue value);

// Convenience macros
#define VALUEARRAY_GET(arr, index) DYNARRAY_GET(arr, index, XrValue)
#define VALUEARRAY_COUNT(arr) DYNARRAY_COUNT(arr)

/* ========== Function Prototype (XrProto) ========== */
// Upvalue source: where OP_CLOSURE reads the initial value from
#define UPVAL_SRC_UPVAL      1 // from enclosing closure's upvals[] (transitive)
#define UPVAL_SRC_REG        2 // from enclosing frame's register (direct capture)

typedef struct UpvalInfo {
    uint8_t index;        // SRC_REG: register number; SRC_UPVAL: enclosing upval index
    uint8_t storage_mode; // 0=normal, 1=shared
    uint8_t is_const;     // const variable flag
    uint8_t slot_type;    // XrSlotType: storage class for GC traversal
    uint8_t source;       // UPVAL_SRC_REG or UPVAL_SRC_UPVAL
    struct XrType *type_info; // full compile-time type (NULL = unknown/any)
} UpvalInfo;

// Local variable debug info
typedef struct XrLocVar {
    const char *name;   // variable name
    int start_pc;       // scope start instruction index
    int end_pc;         // scope end instruction index (-1 = not ended)
    int reg;            // register number
} XrLocVar;

// entry_type values for XrProto.entry_type
#define XR_ENTRY_NORMAL    0  // plain function: no default params, no generator
#define XR_ENTRY_DEFAULTS  1  // has default parameters (fill missing args with null)
#define XR_ENTRY_GENERATOR 2  // generator function (supports yield)

// Function prototype (compiled function)
typedef struct XrProto {
    XrDynArray code;        // bytecode array
    ValueArray constants;   // constant pool
    XrDynArray protos;      // nested functions
    XrDynArray upvalues;    // upvalue info
    XrDynArray lineinfo;    // line number info
    XrDynArray locvars;     // local variable names
    const char *source_file; // source file path
    
    /*
     * Per-function symbol table: maps local index (0-254) to global SymbolId.
     * Instructions encode local indices in 8-bit B/C fields.
     * VM dereferences: global_sym = proto->symbols[local_idx]
     *
     * WHY THIS DESIGN:
     *   Global symbol IDs can exceed 255 (8-bit limit of iABC fields),
     *   but per-function unique property count is always small (<255).
     *   This eliminates symbol ID overflow by design.
     */
    int32_t *symbols;       // local-to-global symbol mapping
    int symbol_count;       // number of symbols used
    int symbol_capacity;    // allocated capacity
    
    XrString *name;         // function name
    char *return_type;      // return type (NULL = unspecified)
    int maxstacksize;       // max stack (register) size
    int numparams;          // parameter count
    int min_params;         // minimum required params (for default params)
    int num_globals;        // global variable count
    int num_spill_slots;    // spill slot count
    uint16_t struct_area_size; // bytes needed for struct_area in stack frame (0 = none)

    // Struct layout cache for JIT: compile-time struct_layout pointers indexed by
    // slot_offset (from OP_NEW_STRUCT C operand). Enables JIT to resolve field
    // offsets and types at compile time instead of runtime class lookup.
    struct XrStructLayout **struct_layouts; // [struct_layout_count] layout per slot
    int struct_layout_count;

    int shared_offset;      // per-module shared variable offset into isolate->vm.shared
    bool is_vararg;         // is variadic function

    // Entry type: controls VM function setup (skips irrelevant init code)
    // 0=normal, 1=has_defaults (fill missing params), 2=generator (yield support)
    uint8_t entry_type;

    struct XrICMethodTable *ic_methods;  // method call IC table
    struct XrICFieldTable *ic_fields;    // field access IC table
    
    uint8_t test_attr;      // test attribute type
    int test_timeout;       // test timeout (seconds)
    bool is_coro_safe;      // safe to call in coroutine
    
    // Raw constant pool (uint64_t[]) for native-width values (int64/float64)
    // Used by OP_LOADK_RAW to load raw 64-bit values without tagged union
    uint64_t *raw_constants;
    int raw_constant_count;
    int raw_constant_capacity;
    
    /*
     * JIT/AOT metadata: type information preserved from compile-time analysis.
     *
     * WHY THIS DESIGN:
     *   - return_type_info: JIT specializes caller based on return type (XrType*)
     *   - inline_hint: JIT uses compiler's inline decision directly
     *   - loop_headers: JIT identifies OSR entry points and loop bodies
     *   - bb_leaders: bitmap of basic block entry points for JIT CFG construction
     */
    uint8_t *bb_leaders;        // bitmap: bit[pc] = 1 if pc is a basic block leader
    int bb_leaders_size;        // byte count of bb_leaders (ceil(code_count/8))
    uint8_t inline_hint;        // 0=no, 1=candidate, 2=always_inline
    uint8_t is_recursive;       // set by call graph DFS (indirect recursion detection)
    uint64_t tfield_float_bitmap; // bit[i]=1: TFIELD field index i is F64 (set by compiler)
    int16_t *loop_headers;      // PC offsets of loop headers (NULL = none)
    int loop_header_count;

    /*
     * Per-parameter type annotations (authoritative source for param types).
     * param_types[i] = XrType* for parameter i (NULL = untyped/any).
     * Generated by xr_compiler_end() from surviving locals.
     * Used by JIT entry guards, builder param creation, AOT codegen.
     */
    struct XrType **param_types;    // [numparams] parameter types
    uint8_t param_types_count;      // = numparams when allocated

    /*
     * Per-instruction type annotations (flow-sensitive, authoritative for non-params).
     * inst_types[pc] = XrType* for the result of instruction at pc (NULL = untyped).
     * Generated by codegen from compile_type at each emit site.
     * Used by JIT builder for field/call result types, AOT struct inference.
     */
    struct XrType **inst_types;     // [code_count] per-PC result types
    uint32_t inst_types_count;      // = code_count when allocated

    struct XrType *return_type_info; // full return type (NULL = void/any)

    // JIT runtime state (populated at runtime, not compile time)
    void *jit_entry;            // JIT compiled function pointer (NULL = not compiled)
    void *jit_fast_entry;       // Fast entry: skip param loading (register-passing calls)
    void *jit_resume_entry;     // Phase 2: resume entry for JIT suspend/resume (NULL = none)
    void *_Atomic jit_entry_pending;  // Background JIT: compiled entry awaiting installation
    struct XirTypeFeedback *type_feedback; // runtime type profile (lazily allocated)
    _Atomic uint32_t call_count;  // runtime call count (hot function detection)
    _Atomic uint32_t exec_count;  // runtime execution count (Tier 2 trigger)
    uint8_t deopt_count;        // deopt counter (>3 → enter backoff retry)
    bool osr_pending;           // post-deopt: re-attempt OSR at next matching loop back-edge
    uint8_t jit_opt_level;      // current JIT compilation level (0=none, 1=basic, 2=full)
    uint32_t deopt_backoff;     // retry interval: calls to wait before reattempting JIT (doubles on each failure, max 10000)
    uint32_t deopt_reset_at;    // call_count snapshot taken when backoff was last set

    // OSR entry points (populated by JIT compilation, NULL if no loops)
    void *osr_entries;          // XirOsrEntry array (opaque, from xir_codegen.h)
    uint32_t nosr;              // number of OSR entry points

    // Deopt table: per-guard snapshot for precise deoptimization
    // Each entry records bc_pc + live slot→physical_reg/spill mappings
    // Populated by JIT compilation, freed on proto destruction
    void *deopt_table;          // XirRtDeoptEntry array (opaque)
    uint32_t ndeopt;

    // GC stack map: compile-time bitmap for precise GC root scanning.
    // Each safepoint (call/loop back-edge) has a bitmap recording which
    // registers and spill slots hold GC pointers. Populated by JIT codegen.
    void *stack_map;            // XrStackMapTable* (opaque, from xir_codegen.h)

    // Blueprint: compiler-generated JIT metadata (per-instruction types,
    // loop live maps, per-BB slot states). Eliminates inference in XIR builder.
    // Generated by xr_blueprint_generate() in xr_compiler_end().
    struct XrBlueprint *blueprint;

    // Parent proto: set by xr_vm_proto_add_proto when this proto is added
    // as a child. Used by JIT to walk up to module root and build
    // shared_protos mapping for CALL_KNOWN optimization.
    struct XrProto *enclosing;
} XrProto;

// Convenience macros
#define PROTO_CODE(p, idx)         DYNARRAY_GET(&(p)->code, idx, XrInstruction)
#define PROTO_CODE_PTR(p, idx)     DYNARRAY_GET_PTR(&(p)->code, idx, XrInstruction)
#define PROTO_CODE_BASE(p)         ((XrInstruction*)((p)->code.data))
#define PROTO_SET_CODE(p, idx, v)  DYNARRAY_SET(&(p)->code, idx, v, XrInstruction)
#define PROTO_CONSTANT(p, idx)     DYNARRAY_GET(&(p)->constants, idx, XrValue)
// Direct constant access (hot path optimization)
#define PROTO_CONST_BASE(p)        ((XrValue*)((p)->constants.data))
#define PROTO_CONST_FAST(p, idx)   (PROTO_CONST_BASE(p)[idx])
#define PROTO_PROTO(p, idx)        DYNARRAY_GET(&(p)->protos, idx, XrProto*)
#define PROTO_UPVALUE(p, idx)      DYNARRAY_GET(&(p)->upvalues, idx, UpvalInfo)
#define PROTO_LINE(p, idx)         DYNARRAY_GET(&(p)->lineinfo, idx, int)

#define PROTO_SYMBOL(p, idx)       ((p)->symbols[idx])
#define PROTO_SYMBOL_COUNT(p)      ((p)->symbol_count)

#define PROTO_CODE_COUNT(p)        DYNARRAY_COUNT(&(p)->code)
#define PROTO_CONST_COUNT(p)       DYNARRAY_COUNT(&(p)->constants)
#define PROTO_PROTO_COUNT(p)       DYNARRAY_COUNT(&(p)->protos)
#define PROTO_UPVAL_COUNT(p)       DYNARRAY_COUNT(&(p)->upvalues)
#define PROTO_LINE_COUNT(p)        DYNARRAY_COUNT(&(p)->lineinfo)
#define PROTO_LOCVAR(p, idx)       DYNARRAY_GET(&(p)->locvars, idx, XrLocVar)
#define PROTO_LOCVAR_COUNT(p)      DYNARRAY_COUNT(&(p)->locvars)

// XrProto Operations
XR_FUNC XrProto *xr_vm_proto_new(void);
XR_FUNC void xr_vm_proto_free(XrProto *proto);

// Bytecode Operations
XR_FUNC void xr_vm_proto_write(XrProto *proto, XrInstruction inst, int line);
XR_FUNC int xr_vm_proto_add_constant(XrProto *proto, XrValue value);
XR_FUNC int xr_vm_proto_add_proto(XrProto *proto, XrProto *child);
XR_FUNC int xr_vm_proto_add_upvalue(XrProto *proto, uint8_t index, uint8_t storage_mode, uint8_t is_const, uint8_t slot_type, uint8_t source, struct XrType *type_info);
XR_FUNC int xr_proto_add_symbol(XrProto *proto, int32_t global_symbol);
XR_FUNC int xr_proto_add_raw_constant(XrProto *proto, uint64_t value);

/* ========== Debug Helpers ========== */
XR_FUNC const char *xr_opcode_name(OpCode op);

#endif // XCHUNK_H
