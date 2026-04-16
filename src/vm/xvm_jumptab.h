/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_jumptab.h - VM jump table (computed goto optimization)
 *
 * KEY CONCEPT:
 *   Computed goto for GCC/Clang provides 10-15% performance boost.
 *   Include in xvm.c main loop, use vmcase(OP_XXX) and vmbreak macros.
 */

#ifndef XVM_JUMPTAB_H
#define XVM_JUMPTAB_H

// vmdispatch: jump to label pointed by disptab[opcode]
// vmcase: define label L_OP_XXX
// vmbreak: fetch next instruction and dispatch (with profiler)
#define vmdispatch(x)     goto *disptab[x]
#define vmcase(l)         L_##l:
#define vmbreak           do { \
                            i = *pc++; \
                            VM_DEBUG_CHECK(); \
                            OpCode _op = GET_OPCODE(i); \
                            VM_PROFILE_COUNT(_op); \
                            vmdispatch(_op); \
                          } while(0)

static const void *const disptab[NUM_OPCODES] = {
    // Basic operations
    &&L_OP_MOVE,
    &&L_OP_LOADI,
    &&L_OP_LOADF,
    &&L_OP_LOADK,
    &&L_OP_LOADNULL,
    &&L_OP_LOADTRUE,
    &&L_OP_LOADFALSE,
    
    // Arithmetic operations
    &&L_OP_ADD,
    &&L_OP_ADDI,
    &&L_OP_ADDK,
    &&L_OP_SUB,
    &&L_OP_SUBI,
    &&L_OP_SUBK,
    &&L_OP_MUL,
    &&L_OP_MULI,
    &&L_OP_MULK,
    &&L_OP_DIV,
    &&L_OP_DIVK,
    &&L_OP_MOD,
    &&L_OP_MODK,
    &&L_OP_UNM,
    &&L_OP_NOT,
    
    // String buffer
    &&L_OP_STRBUF_NEW,
    &&L_OP_STRBUF_APPEND,
    &&L_OP_STRBUF_FINISH,
    
    // Bitwise operations
    &&L_OP_BAND,
    &&L_OP_BOR,
    &&L_OP_BXOR,
    &&L_OP_BNOT,
    &&L_OP_SHL,
    &&L_OP_SHR,
    
    // Comparison (jump-based)
    &&L_OP_EQ,
    &&L_OP_EQK,
    &&L_OP_EQI,
    &&L_OP_LT,
    &&L_OP_LTI,
    &&L_OP_LE,
    &&L_OP_LEI,
    
    // Comparison (expression-based)
    &&L_OP_CMP_EQ,
    &&L_OP_CMP_NE,
    &&L_OP_CMP_EQ_STRICT,
    &&L_OP_CMP_NE_STRICT,
    &&L_OP_CMP_LT,
    &&L_OP_CMP_LE,
    &&L_OP_IS,
    &&L_OP_CHECKTYPE,
    &&L_OP_ISNULL,
    &&L_OP_ISNULL_SET,
    
    // Control flow
    &&L_OP_JMP,
    &&L_OP_TEST,
    &&L_OP_TESTSET,
    &&L_OP_CALL,
    &&L_OP_CALL_KEEP,
    &&L_OP_CALL_STATIC,
    &&L_OP_CALLSELF,
    &&L_OP_TAILCALL,
    &&L_OP_RETURN,
    &&L_OP_RETURN0,
    &&L_OP_RETURN1,
    
    // Container creation
    &&L_OP_NEWARRAY,
    &&L_OP_NEWMAP,
    &&L_OP_NEWSET,
    &&L_OP_NEWSTRINGBUILDER,
    &&L_OP_NEWRANGE,
    &&L_OP_RANGE_UNPACK,
    
    // Array operations
    &&L_OP_ARRAY_GET,
    &&L_OP_ARRAY_GETC,
    &&L_OP_ARRAY_SET,
    &&L_OP_ARRAY_SETC,
    &&L_OP_ARRAY_PUSH,
    &&L_OP_ARRAY_LEN,
    &&L_OP_ARRAY_INIT,
    
    // Map operations
    &&L_OP_MAP_GET,
    &&L_OP_MAP_GETK,
    &&L_OP_MAP_SET,
    &&L_OP_MAP_SETK,
    
    // Generic index operations
    &&L_OP_INDEX_GET,
    &&L_OP_INDEX_SET,
    
    // Slice operations
    &&L_OP_SLICE,
    
    // Closure & Upvalues
    &&L_OP_CLOSURE,
    &&L_OP_UPVAL_GET,
    &&L_OP_CELL_NEW,
    &&L_OP_CELL_GET,
    &&L_OP_CELL_SET,
    
    // OOP - Runtime class building
    &&L_OP_CLASS_CREATE_FROM_DESCRIPTOR,
    &&L_OP_CLINIT_CALL,
    
    // OOP - Class operations
    &&L_OP_INHERIT,
    &&L_OP_GETPROP,
    &&L_OP_SETPROP,
    &&L_OP_GETSUPER,
    &&L_OP_INVOKE,
    &&L_OP_INVOKE_TAIL,
    &&L_OP_SUPERINVOKE,
    
    // OOP - Optimized instructions
    &&L_OP_INVOKE_DIRECT,
    &&L_OP_INVOKE_BUILTIN,
    
    // OOP - Runtime support
    &&L_OP_ABSTRACT_ERROR,
    &&L_OP_SET_STORAGE_CTX,
    
    // shared conversion
    &&L_OP_TO_SHARED,
    &&L_OP_MAP_SETKS,
    
    // Instance field access (O(1))
    &&L_OP_GETFIELD,
    &&L_OP_SETFIELD,
    &&L_OP_GETFIELD_IC,
    
    // Json dynamic object
    &&L_OP_NEWJSON,
    &&L_OP_JSON_GET,
    &&L_OP_JSON_SET,
    &&L_OP_JSON_GETK,
    &&L_OP_JSON_SETK,
    &&L_OP_JSON_INIT,
    &&L_OP_JSON_INIT_I,
    &&L_OP_JSON_INIT_N,
    
    // Builtin globals (read-only)
    &&L_OP_GETBUILTIN,
    
    // Built-in functions
    &&L_OP_PRINT,
    &&L_OP_TYPEOF,
    &&L_OP_TYPENAME,
    &&L_OP_DUMP,
    &&L_OP_TOINT,
    &&L_OP_TOFLOAT,
    &&L_OP_TOSTRING,
    &&L_OP_TOBOOL,
    &&L_OP_COPY,
    &&L_OP_CHR,
    
    // Enum
    &&L_OP_ENUM_ACCESS,
    &&L_OP_ENUM_CONVERT,
    &&L_OP_ENUM_NAME,
    
    // Exception handling
    &&L_OP_TRY,
    &&L_OP_CATCH,
    &&L_OP_FINALLY,
    &&L_OP_END_TRY,
    &&L_OP_THROW,
    
    // Register spill
    &&L_OP_SPILL,
    &&L_OP_RELOAD,
    
    // Box/Unbox (typed storage boundary)
    &&L_OP_BOX_I64,
    &&L_OP_BOX_F64,
    &&L_OP_UNBOX_I64,
    &&L_OP_UNBOX_F64,
    
    // Array access without bounds check
    &&L_OP_ARRAY_GET_NOCHECK,
    
    // Map counter optimization
    &&L_OP_MAP_INCREMENT,
    
    // String operation optimization
    &&L_OP_SUBSTRING,
    &&L_OP_STR_REPEAT,
    
    // Module system
    &&L_OP_IMPORT,
    &&L_OP_EXPORT,
    &&L_OP_EXPORT_ALL,
    
    // Assertion instructions
    &&L_OP_ASSERT,
    &&L_OP_ASSERT_EQ,
    &&L_OP_ASSERT_NE,
    
    // Regex literal
    &&L_OP_REGEX_COMPILE,
    
    // Coroutine instructions
    &&L_OP_GO,
    &&L_OP_GO_INVOKE,
    &&L_OP_SPAWN_CONT,
    &&L_OP_AWAIT,
    &&L_OP_AWAIT_TIMEOUT,
    &&L_OP_AWAIT_ALL,
    &&L_OP_AWAIT_ANY,
    &&L_OP_YIELD,
    &&L_OP_CANCELLED,
    &&L_OP_LOCK_THREAD,
    &&L_OP_UNLOCK_THREAD,
    &&L_OP_SET_LOCAL,
    &&L_OP_GET_LOCAL,
    &&L_OP_SET_PRIORITY,
    &&L_OP_CORO_CTRL,
    
    // Channel instructions
    &&L_OP_CHAN_NEW,
    &&L_OP_CHAN_NEW_NAMED,
    &&L_OP_CHAN_SEND,
    &&L_OP_CHAN_RECV,
    &&L_OP_CHAN_TRY_SEND,
    &&L_OP_CHAN_TRY_RECV,
    &&L_OP_CHAN_SEND_TIMEOUT,
    &&L_OP_CHAN_RECV_TIMEOUT,
    &&L_OP_CHAN_CLOSE,
    &&L_OP_CHAN_IS_CLOSED,
    
    // Select multiplexing
    &&L_OP_SELECT_START,
    &&L_OP_SELECT_CASE,
    &&L_OP_SELECT_END,
    
    // Defer
    &&L_OP_DEFER,
    
    // Bytes array
    &&L_OP_BYTES_NEW,
    
    // Scope structured concurrency
    &&L_OP_SCOPE_ENTER,
    &&L_OP_SCOPE_EXIT,
    
    // Coroutine-friendly instructions
    &&L_OP_SLEEP,
    &&L_OP_TIME_AFTER,
    &&L_OP_SELECT_BLOCK,
    
    // Shared variables
    &&L_OP_GETSHARED,
    &&L_OP_SETSHARED,
    
    // Typed compact storage (raw in, raw out)
    &&L_OP_TARRAY_GET,
    &&L_OP_TARRAY_GETC,
    &&L_OP_TARRAY_SET,
    &&L_OP_TARRAY_PUSH,
    &&L_OP_TFIELD_GET,
    &&L_OP_TFIELD_SET,

    // Instance reified type args
    &&L_OP_INST_TYPE_ARGS,

    // Tail recursion loop
    &&L_OP_LOOP_BACK,

    // Struct native storage
    &&L_OP_NEW_STRUCT,
    &&L_OP_STRUCT_GET,
    &&L_OP_STRUCT_SET,
    &&L_OP_STRUCT_COPY,

    // NOP
    &&L_OP_NOP
};

#endif // XVM_JUMPTAB_H
