/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ops.h - XIR opcode enumeration
 *
 * KEY CONCEPT:
 *   All XIR virtual opcodes in a single enum, separated from the
 *   core type definitions in xir.h for header-size compliance.
 *   Machine-specific opcodes (ARM64) start at XIR_MACH_BASE = 256.
 *
 * SPLIT FROM:
 *   Originally part of xir.h; extracted during Phase 7.
 */

#ifndef XIR_OPS_H
#define XIR_OPS_H

/* ========== XIR Opcodes ========== */

typedef enum {
    // --- Arithmetic / Logic (map directly to machine instructions) ---
    XIR_ADD = 0,
    XIR_SUB,
    XIR_MUL,
    XIR_DIV,
    XIR_MOD,
    XIR_NEG,
    XIR_AND,
    XIR_OR,
    XIR_XOR,
    XIR_NOT,
    XIR_SHL,
    XIR_SHR,

    // Float arithmetic
    XIR_FADD,
    XIR_FSUB,
    XIR_FMUL,
    XIR_FDIV,
    XIR_FNEG,

    // --- Type conversion ---
    XIR_I2F,         // int64 → float64 (SCVTF)
    XIR_F2I,         // float64 → int64 (FCVTZS)

    // --- Comparison ---
    XIR_EQ,
    XIR_NE,
    XIR_LT,
    XIR_LE,
    XIR_GT,
    XIR_GE,
    XIR_FEQ,
    XIR_FNE,
    XIR_FLT,
    XIR_FLE,

    // --- Tagged value operations (xray-specific) ---
    XIR_BOX_I64,     // raw i64 → tagged XrValue
    XIR_BOX_F64,     // raw f64 → tagged XrValue
    XIR_UNBOX_I64,   // tagged → raw i64 (with type guard)
    XIR_UNBOX_F64,   // tagged → raw f64 (with type guard)
    XIR_TAG_CHECK,   // check tag == expected (deopt if fail)
    XIR_TAG_LOAD,    // load tag field from XrValue

    // --- Memory ---
    XIR_LOAD,        // load from memory (typed)
    XIR_STORE,       // store to memory (typed)
    XIR_LOAD_FIELD,  // object field load (with IC)
    XIR_STORE_FIELD, // object field store: dst=const(packed), args[0]=obj, args[1]=val
                     // packed = XIR_SF_PACK(xr_tag, byte_offset)
    XIR_ALLOC,       // GC allocation (bump pointer)
    XIR_STORE_CORO,  // store to coro struct: [coro + byte_offset] = val
    XIR_STORE_CORO_BYTE, // store byte to jit_ctx: [jit_ctx + byte_offset] = (uint8_t)val
                         // dst=const(byte_offset), args[0]=value (low 8 bits written)
    XIR_LOAD_CORO,   // load from coro struct: dst = [jit_ctx + byte_offset]
                     // args[0]=const(byte_offset), result=i64
    XIR_LOAD_CORO_BYTE, // load byte from jit_ctx: dst = (uint8_t)[jit_ctx + byte_offset]
                        // args[0]=const(byte_offset), result=i64 (zero-extended)
    XIR_LOAD32S,     // 32-bit sign-extending load: dst = (int64_t)*(int32_t*)[args[0] + args[1]]
                     // args[0]=base ptr, args[1]=const(byte_offset), result=i64
    XIR_LOAD8Z,      // 8-bit zero-extending load: dst = (int64_t)*(uint8_t*)[args[0]]
                     // args[0]=addr ptr, result=i64
    XIR_LOAD8S,      // 8-bit sign-extending load: dst = (int64_t)(int8_t)*(uint8_t*)[args[0]]
                     // args[0]=addr ptr, result=i64
    XIR_STORE8,      // 8-bit store: *(uint8_t*)[args[0]] = (uint8_t)args[1]
                     // args[0]=addr ptr, args[1]=value (low 8 bits written)
    XIR_LOAD16Z,     // 16-bit zero-extending load: dst = (int64_t)*(uint16_t*)[args[0]]
                     // args[0]=addr ptr, result=i64
    XIR_LOAD16S,     // 16-bit sign-extending load: dst = (int64_t)(int16_t)*(uint16_t*)[args[0]]
                     // args[0]=addr ptr, result=i64
    XIR_STORE16,     // 16-bit store: *(uint16_t*)[args[0]] = (uint16_t)args[1]
                     // args[0]=addr ptr, args[1]=value (low 16 bits written)
    XIR_LOAD32Z,     // 32-bit zero-extending load: dst = (int64_t)*(uint32_t*)[args[0]]
                     // args[0]=addr ptr, result=i64
    XIR_STORE32,     // 32-bit store: *(uint32_t*)[args[0]] = (uint32_t)args[1]
                     // args[0]=addr ptr, args[1]=value (low 32 bits written)
    XIR_LOAD_F32,    // 32-bit float load with double promotion:
                     // dst = (double)*(float*)[args[0]], result=f64
    XIR_STORE_F32,   // 32-bit float store with truncation:
                     // *(float*)[args[0]] = (float)args[1], args[1]=f64 value
    XIR_GUARD_BOUNDS, // array bounds check: deopt if (unsigned)args[0] >= (unsigned)args[1]
                      // args[0]=index (i64), args[1]=length (i64), dst=const(deopt_id)

    // --- Constants ---
    XIR_CONST_I64,   // integer constant
    XIR_CONST_F64,   // float constant
    XIR_CONST_PTR,   // pointer constant (NULL, string literal, etc.)

    // --- Control flow ---
    XIR_JMP,         // unconditional jump
    XIR_BR,          // conditional branch (if arg0 goto s1 else s2)
    XIR_RET,         // return value
    XIR_CALL,        // function call (xray function)
    XIR_CALL_C,      // call C runtime function (generic, future use)
    XIR_CALL_C_LEAF, // lightweight C call (no GC): precise register save/restore
    XIR_CALL_SELF_DIRECT, // direct recursive self-call (BL to own entry)
    XIR_CALL_DIRECT,      // direct JIT→JIT call (inline fast path, fallback to C bridge)
    XIR_CALL_KNOWN,       // cross-function direct BL: callee proto known at compile time
                          // args[0] = const_ptr(callee XrProto*), args[1] = const(nargs)
                          // params passed via coro->jit_call_args (already stored by builder)
                          // Fast: load proto->jit_entry, BLR directly (no closure lookup)
                          // Slow: fallback to xr_jit_call_func C bridge
    XIR_CALL_KNOWN_REG,   // register-passing variant of CALL_KNOWN (nargs <= 2)
                          // args[0] = param0 XIR ref, args[1] = param1 XIR ref (or NONE)
                          // callee proto pre-stored in coro->jit_call_proto by builder
                          // Fast: load proto->jit_fast_entry, MOV args to regs, BLR
                          // Slow: fallback to xr_jit_call_func C bridge

    // --- Runtime helper calls (mixed-type operations) ---
    // Codegen inlines type conversions for known numeric combos,
    // falls back to C helper or deopt for unknown types.
    XIR_RT_ADD,      // mixed-type add:  dst = a + b
    XIR_RT_SUB,      // mixed-type sub:  dst = a - b
    XIR_RT_MUL,      // mixed-type mul:  dst = a * b
    XIR_RT_DIV,      // mixed-type div:  dst = a / b
    XIR_RT_MOD,      // mixed-type mod:  dst = a % b
    XIR_RT_UNM,      // mixed-type neg:  dst = -a  (unary)
    XIR_RT_LT,       // mixed-type lt:   dst = a < b
    XIR_RT_LE,       // mixed-type le:   dst = a <= b
    XIR_RT_EQ,       // mixed-type eq:   dst = a == b
    XIR_RT_PRINT,    // print value: args[0]=value, args[1]=const(flags)
                     // flags: bit0=newline, bit1-2=slot_type hint (0=ANY,1=I64,2=F64), bit3=add_space

    // --- Array/Index runtime helpers (AOT only) ---
    XIR_RT_ARRAY_NEW,   // dst = xrt_array_new(args[0]=capacity)
    XIR_RT_ARRAY_PUSH,  // xrt_array_push(args[0]=arr, args[1]=val)
    XIR_RT_ARRAY_LEN,   // dst = xrt_array_len(args[0]=arr)
    XIR_RT_INDEX_GET,   // dst = obj[key]:  args[0]=obj, args[1]=key
    XIR_RT_INDEX_SET,   // obj[key]=val: dst=val, args[0]=obj, args[1]=key
    XIR_RT_MAP_NEW,     // dst = xrt_map_new(args[0]=capacity)
    XIR_RT_ISNULL,      // dst = (args[0].tag == XRT_TAG_NULL) ? 1 : 0

    // --- Runtime interaction ---
    XIR_SAFEPOINT,      // unified safepoint (GC + preemption via reductions)
    XIR_BARRIER_FWD,    // forward write barrier (mark child)
    XIR_BARRIER_BACK,   // back write barrier (parent → gray)
    XIR_DEOPT,          // deoptimization point
    XIR_GUARD_TAG,      // tag guard (deopt on type mismatch)
    XIR_GUARD_CLASS,    // class guard (deopt on gc_extra mismatch)
    XIR_GUARD_KLASS,    // klass pointer guard: deopt if inst->klass != expected
                        // args[0]=instance_ptr, args[1]=const_ptr(expected_klass)
    XIR_GUARD_NONNULL,  // non-null guard (deopt on null)
    XIR_GUARD_SHAPE,    // shape guard: deopt if obj's shape_id != expected
                        // args[0]=obj_ptr, args[1]=const(expected_shape_id)

    // --- Coroutine suspend/resume ---
    XIR_SUSPEND,     // JIT await suspend: save regs, call await_block, return SUSPEND_MARKER
                     // dst = result vreg (filled on resume or inline-resume)
                     // args[0] = child coro vreg, args[1] = const(discard_result)

    // --- Exception handling ---
    XIR_TRY_BEGIN,   // enter try block
    XIR_TRY_END,     // leave try block
    XIR_THROW,       // throw exception
    XIR_CATCH,       // catch entry (read exception value)

    // --- Defer (AOT only) ---
    XIR_DEFER_PUSH,  // push closure to defer stack: args[0]=closure, args[1]=const(nargs)
                     // actual call args stored in XirFunc.defer_entries[]

    // --- ARC reference counting (AOT mode only) ---
    XIR_RETAIN,      // ARC retain: args[0]=ptr — rc++, no result
    XIR_RELEASE,     // ARC release: args[0]=ptr — rc--, free when 0

    // --- Closure/Upvalue access ---
    XIR_LOAD_UPVAL,  // load upvalue: args[0]=const(upval_index), result=raw value
    XIR_STORE_UPVAL, // store upvalue: args[0]=const(upval_index), args[1]=value

    // --- Conditional select (from if-conversion) ---
    // Two-instruction pattern (cf. QBE Osel0/Osel1):
    //   XIR_SELECT_COND: args[0] = cond vreg (void, sets condition)
    //   XIR_SELECT:      dst = args[0] if cond != 0, else args[1]
    // Codegen emits: CMP cond, #0; CSEL dst, true, false, NE
    XIR_SELECT_COND, // void: args[0] = condition vreg
    XIR_SELECT,      // dst = cond ? args[0] : args[1]

    // --- Miscellaneous ---
    XIR_MOV,         // register move (inserted by regalloc)
    XIR_NOP,         // no-op (placeholder)
    XIR_PHI,         // phi node (only used in early SSA, lowered before isel)
    XIR_REDEFINE,    // type narrowing: dst = args[0] (identity, carries narrowed ctype)

    XIR_OP_COUNT,    // sentinel

    // --- Machine opcodes (filled by isel, share the same XirIns layout) ---
    // ARM64 opcodes start here; the actual values are defined in xir_arm64.h
    XIR_MACH_BASE = 256,
} XirOp;


#endif // XIR_OPS_H
