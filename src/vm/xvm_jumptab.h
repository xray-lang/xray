/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_jumptab.h - VM jump table (computed goto optimization).
 *
 * KEY CONCEPT:
 *   Computed goto for GCC/Clang gives ~10-15% over a switch on the
 *   hot dispatch loop. Include this from xvm.c right inside run();
 *   call sites use vmcase(OP_XXX) / vmbreak.
 *
 * SOURCE OF TRUTH:
 *   The disptab[] body is generated from XR_OPCODE_TABLE in
 *   xopcode_def.h. Adding an opcode there automatically gets a slot
 *   here, and the corresponding L_OP_<NAME> label inside run() must
 *   exist (compiler emits a "jump to undefined label" error otherwise,
 *   making the missing case impossible to ship).
 */

#ifndef XVM_JUMPTAB_H
#define XVM_JUMPTAB_H

#include "../runtime/value/xopcode_def.h"

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
#define _XR_OPCODE_LABEL(name, fmt, kop, desc) [OP_##name] = &&L_OP_##name,
    XR_OPCODE_TABLE(_XR_OPCODE_LABEL)
#undef _XR_OPCODE_LABEL
};

#endif // XVM_JUMPTAB_H
