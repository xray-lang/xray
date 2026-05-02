/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_ops.h - Xm opcode enumeration
 *
 * KEY CONCEPT:
 *   All Xm virtual opcodes in a single enum, separated from the
 *   core type definitions in xm.h for header-size compliance.
 *   Machine-specific opcodes (ARM64) start at XM_MACH_BASE = 256.
 */

#ifndef XM_OPS_H
#define XM_OPS_H

/* ========== Xm Opcodes ========== */

typedef enum {
    // --- Arithmetic / Logic (map directly to machine instructions) ---
    XM_ADD = 0,
    XM_SUB,
    XM_MUL,
    XM_DIV,
    XM_MOD,
    XM_NEG,
    XM_AND,
    XM_OR,
    XM_XOR,
    XM_NOT,
    XM_SHL,
    XM_SHR,

    // Float arithmetic
    XM_FADD,
    XM_FSUB,
    XM_FMUL,
    XM_FDIV,
    XM_FNEG,

    // --- Type conversion ---
    XM_I2F,  // int64 → float64 (SCVTF)
    XM_F2I,  // float64 → int64 (FCVTZS)

    // --- Comparison ---
    XM_EQ,
    XM_NE,
    XM_LT,
    XM_LE,
    XM_GT,
    XM_GE,
    XM_FEQ,
    XM_FNE,
    XM_FLT,
    XM_FLE,

    // --- Tagged value operations (xray-specific) ---
    XM_BOX_I64,    // raw i64 → tagged XrValue
    XM_BOX_F64,    // raw f64 → tagged XrValue
    XM_UNBOX_I64,  // tagged → raw i64 (with type guard)
    XM_UNBOX_F64,  // tagged → raw f64 (with type guard)
    XM_TAG_CHECK,  // check tag == expected (deopt if fail)
    XM_TAG_LOAD,   // load tag field from XrValue

    // --- Memory ---
    XM_LOAD,             // load from memory (typed)
    XM_STORE,            // store to memory (typed)
    XM_LOAD_FIELD,       // object field load (with IC)
    XM_STORE_FIELD,      // object field store: dst=const(packed), args[0]=obj, args[1]=val
                          // packed = XM_SF_PACK(xr_tag, byte_offset)
    XM_ALLOC,            // GC allocation (bump pointer)
    XM_STORE_CORO,       // store to coro struct: [coro + byte_offset] = val
    XM_STORE_CORO_BYTE,  // store byte to jit_ctx: [jit_ctx + byte_offset] = (uint8_t)val
                          // dst=const(byte_offset), args[0]=value (low 8 bits written)
    XM_LOAD_CORO,        // load from coro struct: dst = [jit_ctx + byte_offset]
                          // args[0]=const(byte_offset), result=i64
    XM_LOAD_CORO_BYTE,   // load byte from jit_ctx: dst = (uint8_t)[jit_ctx + byte_offset]
                          // args[0]=const(byte_offset), result=i64 (zero-extended)
    XM_LOAD32S,       // 32-bit sign-extending load: dst = (int64_t)*(int32_t*)[args[0] + args[1]]
                       // args[0]=base ptr, args[1]=const(byte_offset), result=i64
    XM_LOAD8Z,        // 8-bit zero-extending load: dst = (int64_t)*(uint8_t*)[args[0]]
                       // args[0]=addr ptr, result=i64
    XM_LOAD8S,        // 8-bit sign-extending load: dst = (int64_t)(int8_t)*(uint8_t*)[args[0]]
                       // args[0]=addr ptr, result=i64
    XM_STORE8,        // 8-bit store: *(uint8_t*)[args[0]] = (uint8_t)args[1]
                       // args[0]=addr ptr, args[1]=value (low 8 bits written)
    XM_LOAD16Z,       // 16-bit zero-extending load: dst = (int64_t)*(uint16_t*)[args[0]]
                       // args[0]=addr ptr, result=i64
    XM_LOAD16S,       // 16-bit sign-extending load: dst = (int64_t)(int16_t)*(uint16_t*)[args[0]]
                       // args[0]=addr ptr, result=i64
    XM_STORE16,       // 16-bit store: *(uint16_t*)[args[0]] = (uint16_t)args[1]
                       // args[0]=addr ptr, args[1]=value (low 16 bits written)
    XM_LOAD32Z,       // 32-bit zero-extending load: dst = (int64_t)*(uint32_t*)[args[0]]
                       // args[0]=addr ptr, result=i64
    XM_STORE32,       // 32-bit store: *(uint32_t*)[args[0]] = (uint32_t)args[1]
                       // args[0]=addr ptr, args[1]=value (low 32 bits written)
    XM_LOAD_F32,      // 32-bit float load with double promotion:
                       // dst = (double)*(float*)[args[0]], result=f64
    XM_STORE_F32,     // 32-bit float store with truncation:
                       // *(float*)[args[0]] = (float)args[1], args[1]=f64 value
    XM_GUARD_BOUNDS,  // array bounds check: deopt if (unsigned)args[0] >= (unsigned)args[1]
                       // args[0]=index (i64), args[1]=length (i64), dst=const(deopt_id)

    // --- Constants ---
    XM_CONST_I64,  // integer constant
    XM_CONST_F64,  // float constant
    XM_CONST_PTR,  // pointer constant (NULL, string literal, etc.)

    // --- Control flow ---
    XM_JMP,               // unconditional jump
    XM_BR,                // conditional branch (if arg0 goto s1 else s2)
    XM_RET,               // return value
    XM_CALL,              // function call (xray function)
    XM_CALL_C,            // call C runtime function (generic, future use)
    XM_CALL_C_LEAF,       // lightweight C call (no GC): precise register save/restore
    XM_CALL_SELF_DIRECT,  // direct recursive self-call (BL to own entry)
    XM_CALL_DIRECT,       // direct JIT→JIT call (inline fast path, fallback to C bridge)
    XM_CALL_KNOWN,        // cross-function direct BL: callee proto known at compile time
                           // args[0] = const_ptr(callee XrProto*), args[1] = const(nargs)
                           // params passed via coro->jit_call_args (already stored by builder)
                           // Fast: load proto->jit_entry, BLR directly (no closure lookup)
                           // Slow: fallback to xr_jit_call_func C bridge
    XM_CALL_KNOWN_REG,    // register-passing variant of CALL_KNOWN (nargs <= 2)
                           // args[0] = param0 Xm ref, args[1] = param1 Xm ref (or NONE)
                           // callee proto pre-stored in coro->jit_call_proto by builder
                           // Fast: load proto->jit_fast_entry, MOV args to regs, BLR
                           // Slow: fallback to xr_jit_call_func C bridge
    XM_CALL_INTRINSIC,    // AOT intrinsic call (converted from CALL_C by resolve pass)
                           // args[0] = const(XmIntrinsicId), args[1] = extra_arg
                           // AOT codegen dispatches on the intrinsic ID, never on fn_ptr

    // --- Runtime helper calls (mixed-type operations) ---
    // Codegen inlines type conversions for known numeric combos,
    // falls back to C helper or deopt for unknown types.
    XM_RT_ADD,    // mixed-type add:  dst = a + b
    XM_RT_SUB,    // mixed-type sub:  dst = a - b
    XM_RT_MUL,    // mixed-type mul:  dst = a * b
    XM_RT_DIV,    // mixed-type div:  dst = a / b
    XM_RT_MOD,    // mixed-type mod:  dst = a % b
    XM_RT_UNM,    // mixed-type neg:  dst = -a  (unary)
    XM_RT_LT,     // mixed-type lt:   dst = a < b
    XM_RT_LE,     // mixed-type le:   dst = a <= b
    XM_RT_EQ,     // mixed-type eq:   dst = a == b
    XM_RT_PRINT,  // print value: args[0]=value, args[1]=const(flags)
                   // flags: bit0=newline, bit1-2=slot_type hint (0=ANY,1=I64,2=F64), bit3=add_space

    // --- Array/Index runtime helpers (AOT only) ---
    XM_RT_ARRAY_NEW,   // dst = xrt_array_new(args[0]=capacity)
    XM_RT_ARRAY_PUSH,  // xrt_array_push(args[0]=arr, args[1]=val)
    XM_RT_ARRAY_LEN,   // dst = xrt_array_len(args[0]=arr)
    XM_RT_INDEX_GET,   // dst = obj[key]:  args[0]=obj, args[1]=key
    XM_RT_INDEX_SET,   // obj[key]=val: dst=val, args[0]=obj, args[1]=key
    XM_RT_MAP_NEW,     // dst = xrt_map_new(args[0]=capacity)
    XM_RT_ISNULL,      // dst = (args[0].tag == XR_TAG_NULL) ? 1 : 0

    // --- Runtime interaction ---
    XM_SAFEPOINT,      // unified safepoint (GC + preemption via reductions)
    XM_BARRIER_FWD,    // forward write barrier (mark child)
    XM_BARRIER_BACK,   // back write barrier (parent → gray)
    XM_DEOPT,          // deoptimization point
    XM_GUARD_TAG,      // tag guard (deopt on type mismatch)
    XM_GUARD_CLASS,    // class guard (deopt on gc_extra mismatch)
    XM_GUARD_KLASS,    // klass pointer guard: deopt if inst->klass != expected
                        // args[0]=instance_ptr, args[1]=const_ptr(expected_klass)
    XM_GUARD_NONNULL,  // non-null guard (deopt on null)
    XM_GUARD_SHAPE,    // shape guard: deopt if obj's shape_id != expected
                        // args[0]=obj_ptr, args[1]=const(expected_shape_id)

    // --- Coroutine suspend/resume ---
    XM_SUSPEND,  // JIT await suspend: save regs, call await_block, return SUSPEND_MARKER
                  // dst = result vreg (filled on resume or inline-resume)
                  // args[0] = child coro vreg, args[1] = const(discard_result)

    // --- Exception handling ---
    XM_TRY_BEGIN,  // enter try block
    XM_TRY_END,    // leave try block
    XM_THROW,      // throw exception
    XM_CATCH,      // catch entry (read exception value)

    // --- Defer (AOT only) ---
    XM_DEFER_PUSH,  // push closure to defer stack: args[0]=closure, args[1]=const(nargs)
                     // actual call args stored in XmFunc.defer_entries[]

    // --- ARC reference counting (AOT mode only) ---
    XM_RETAIN,   // ARC retain: args[0]=ptr — rc++, no result
    XM_RELEASE,  // ARC release: args[0]=ptr — rc--, free when 0

    // --- Closure/Upvalue access ---
    XM_LOAD_UPVAL,   // load upvalue: args[0]=const(upval_index), result=raw value
    XM_STORE_UPVAL,  // store upvalue: args[0]=const(upval_index), args[1]=value

    // --- Conditional select (from if-conversion) ---
    // Two-instruction pattern (cf. QBE Osel0/Osel1):
    //   XM_SELECT_COND: args[0] = cond vreg (void, sets condition)
    //   XM_SELECT:      dst = args[0] if cond != 0, else args[1]
    // Codegen emits: CMP cond, #0; CSEL dst, true, false, NE
    XM_SELECT_COND,  // void: args[0] = condition vreg
    XM_SELECT,       // dst = cond ? args[0] : args[1]

    // --- Miscellaneous ---
    XM_MOV,       // register move (inserted by regalloc)
    XM_NOP,       // no-op (placeholder)
    XM_PHI,       // phi node (only used in early SSA, lowered before isel)
    XM_REDEFINE,  // type narrowing: dst = args[0] (identity, carries narrowed ctype)

    XM_OP_COUNT,  // sentinel

    // --- Machine opcodes (filled by isel, share the same XmIns layout) ---
    // ARM64 opcodes start here; the actual values are defined in xm_arm64.h
    XM_MACH_BASE = 256,
} XmOp;

#endif  // XM_OPS_H
