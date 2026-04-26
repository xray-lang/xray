/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_blueprint.c - Blueprint generation (loop liveness for OSR)
 *
 * KEY CONCEPT:
 *   Loop live maps tell OSR stubs which bytecode registers to load
 *   from the interpreter's register array when entering JIT mid-loop.
 */

#include "xir_blueprint.h"
#include "xir.h"
#include "../runtime/value/xchunk.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include <string.h>

/* ========== Internal: Opcode Read/Write Slot Analysis ========== */

// Check if a bytecode instruction reads from register slot `reg` as a source.
// Used for loop liveness: if a slot is read in a loop body, it's live at header.
static bool opcode_reads_slot(XrInstruction inst, int reg) {
    OpCode op = GET_OPCODE(inst);
    int a = GETARG_A(inst);
    int b = GETARG_B(inst);
    int c = GETARG_C(inst);

    switch (op) {
        // Instructions that read B and/or C as register sources
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        case OP_BAND:
        case OP_BOR:
        case OP_BXOR:
        case OP_SHL:
        case OP_SHR:
        case OP_CMP_EQ:
        case OP_CMP_NE:
        case OP_CMP_LT:
        case OP_CMP_LE:
        case OP_CMP_EQ_STRICT:
        case OP_CMP_NE_STRICT:
            return reg == b || reg == c;

        // Instructions that read B as source
        case OP_MOVE:
        case OP_UNM:
        case OP_BNOT:
        case OP_NOT:
        case OP_TOINT:
        case OP_TOFLOAT:
        case OP_TOBOOL:
        case OP_TOSTRING:
        case OP_TYPEOF:
        case OP_COPY:
        case OP_BOX_I64:
        case OP_BOX_F64:
        case OP_UNBOX_I64:
        case OP_UNBOX_F64:
        case OP_ARRAY_LEN:
        case OP_TYPENAME:
        case OP_CHR:
            return reg == b;

        // Instructions that read B + constant K
        case OP_ADDK:
        case OP_SUBK:
        case OP_MULK:
        case OP_DIVK:
        case OP_MODK:
        case OP_ADDI:
        case OP_SUBI:
        case OP_MULI:
            return reg == b;

        // Conditional comparisons: OP_LT/LE/EQ read both R[A] and R[B]
        case OP_LT:
        case OP_LE:
        case OP_EQ:
            return reg == a || reg == b;

        // Immediate/constant comparisons: read only R[A]
        case OP_EQI:
        case OP_EQK:
        case OP_LTI:
        case OP_LEI:
        case OP_TEST:
        case OP_TESTSET:
            return reg == a;

        // CALL: reads func in A, args in A+1..A+B
        case OP_CALL:
        case OP_CALL_KEEP:
        case OP_CALL_STATIC:
        case OP_CALLSELF:
        case OP_TAILCALL:
            return reg >= a && reg <= a + b;

        // Property access: reads object from B
        case OP_GETPROP:
        case OP_GETFIELD:
        case OP_GETFIELD_IC:
        case OP_GETSUPER:
            return reg == b;

        // Property set: reads object from A, value from B
        case OP_SETPROP:
        case OP_SETFIELD:
            return reg == a || reg == b;

        // Index access: reads container from B, index from C
        case OP_INDEX_GET:
        case OP_ARRAY_GET:
        case OP_ARRAY_GETC:
        case OP_ARRAY_GET_NOCHECK:
        case OP_MAP_GET:
            return reg == b || reg == c;

        // Index set: reads container from A, index from B, value from C
        case OP_INDEX_SET:
        case OP_ARRAY_SET:
        case OP_MAP_SET:
            return reg == a || reg == b || reg == c;

        // Array push: reads array from A, value from B
        case OP_ARRAY_PUSH:
            return reg == a || reg == b;

        // RETURN: reads A
        case OP_RETURN:
        case OP_RETURN1:
            return reg == a;

        // Store operations
        case OP_SETSHARED:
            return reg == GETARG_C(inst);

        // Invoke: reads object in A, args follow
        case OP_INVOKE:
        case OP_INVOKE_BUILTIN:
        case OP_INVOKE_DIRECT:
        case OP_INVOKE_TAIL:
        case OP_SUPERINVOKE:
            return reg >= a && reg <= a + b;

        // Struct operations
        case OP_STRUCT_GET:
            return reg == b;
        case OP_STRUCT_SET:
            return reg == a || reg == c;

        // Channel operations
        case OP_CHAN_SEND:
            return reg == a || reg == b;
        case OP_CHAN_RECV:
        case OP_CHAN_TRY_RECV:
            return reg == b;

        // Go spawn: reads func + args
        case OP_GO:
        case OP_GO_INVOKE:
            return reg >= a && reg <= a + b;

        // Typed array/field
        case OP_TARRAY_GET:
        case OP_TARRAY_GETC:
            return reg == b || reg == c;
        case OP_TARRAY_SET:
            return reg == a || reg == b || reg == c;
        case OP_TFIELD_GET:
            return reg == b;
        case OP_TFIELD_SET:
            return reg == a || reg == c;

        // IS/ISNULL reads B
        case OP_IS:
        case OP_ISNULL_SET:
            return reg == b;

        // Print reads A
        case OP_DUMP:
            return reg == a;

        default:
            // Conservative: assume no reads for unknown opcodes.
            // WARNING: if you add a new opcode that reads registers, you MUST
            // add a case here. Missing cases cause Blueprint loop live maps to
            // omit live slots, leading to uninitialized registers in OSR stubs.
            // Run scripts/check_jit_safety.sh to detect missing cases.
            return false;
    }
}

/* ========== Loop Live Maps ========== */

// Find the back-edge PC for a given loop header.
// The back-edge is a JMP instruction that jumps backward to the header.
static int find_back_edge(XrProto *proto, int header_pc) {
    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    int best = -1;
    for (uint32_t pc = (uint32_t) header_pc; pc < code_count; pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        OpCode op = GET_OPCODE(inst);
        if (op == OP_JMP) {
            int sj = GETARG_sJ(inst);
            int target = (int) pc + sj + 1;
            if (target == header_pc) {
                best = (int) pc;
                // Don't break: there might be a later back-edge (nested break)
            }
        }
    }
    return best;
}

// Generate loop live maps for OSR entry points.
// For each loop header, determines which bytecode slots are live.
static void generate_loop_live_maps(XrBlueprint *bp, XrProto *proto) {
    XR_DCHECK(bp != NULL, "generate_loop_live_maps: NULL blueprint");
    if (!proto->loop_headers || proto->loop_header_count == 0)
        return;

    int nloops = proto->loop_header_count;
    bp->loops = (XrBpLoopInfo *) xr_calloc(nloops, sizeof(XrBpLoopInfo));
    bp->nloops = 0;

    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);

    // Track which slots are written before each loop header.
    // Liveness = defined (written) + used (read in function body).
    uint8_t slot_written[256];
    memset(slot_written, 0, 256);

    // Parameters are implicitly written (initialized by caller frame).
    // Without this, OSR stubs won't load parameter registers, causing
    // the JIT loop to use garbage values for unmodified parameters
    // (e.g. loop bound 'end' in: for (i = start; i < end; i++)).
    for (int s = 0; s < proto->numparams && s < 256; s++) {
        slot_written[s] = 1;
    }

    // Forward scan to track which slots are written before each loop header.
    for (uint32_t pc = 0; pc < code_count; pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        OpCode op = GET_OPCODE(inst);
        int ra = GETARG_A(inst);

        // Track slot writes: most instructions write result to R[A].
        // Exclude instructions that only READ R[A] or use A as a target operand.
        if (ra >= 0 && ra < 256) {
            switch (op) {
                case OP_JMP:
                case OP_TEST:
                case OP_SETPROP:
                case OP_SETFIELD:
                case OP_INDEX_SET:
                case OP_ARRAY_SET:
                case OP_MAP_SET:
                case OP_ARRAY_PUSH:
                case OP_RETURN:
                case OP_RETURN1:
                case OP_CHAN_SEND:
                    break;  // these do NOT write to R[A]
                default:
                    slot_written[ra] = 1;
                    break;
            }
        }

        // Check if this PC is a loop header
        for (int li = 0; li < nloops; li++) {
            if ((int) pc == (int) proto->loop_headers[li] && bp->nloops < nloops) {
                // Snapshot current slot_tag state at loop header
                XrBpLoopInfo *info = &bp->loops[bp->nloops];
                info->header_pc = (uint16_t) pc;
                info->nlive = 0;

                // Find back-edge to determine loop body range
                int back_edge = find_back_edge(proto, (int) pc);
                if (back_edge < 0)
                    back_edge = (int) code_count - 1;

                // A slot is live at the loop header if:
                // 1. It has a known type (assigned before the header), AND
                // 2. It is read anywhere in the function.
                //
                // We scan the entire function [0, code_count) because
                // nested loops may use variables defined before the inner
                // loop header. After the outer loop back-edge, execution
                // returns to code before the inner header, so those reads
                // must be included in the inner loop's live set.
                int maxstack = proto->maxstacksize;
                if (maxstack > 256)
                    maxstack = 256;

                for (int s = 0; s < maxstack && info->nlive < XR_BP_MAX_LOOP_LIVE; s++) {
                    if (!slot_written[s])
                        continue;  // pure DEF-USE: only slots with a real write are live

                    // Default to I64 for all slots.
                    // OSR trigger copies raw .i values; the vreg rep
                    // (from XIR regalloc) determines GPR vs FPR load.
                    uint8_t effective_tag = XR_TAG_I64;

                    // Check if slot s is read anywhere in the function
                    bool read_in_func = false;
                    for (int body_pc = 0; body_pc < (int) code_count; body_pc++) {
                        XrInstruction body_inst = PROTO_CODE(proto, body_pc);
                        if (opcode_reads_slot(body_inst, s)) {
                            read_in_func = true;
                            break;
                        }
                    }

                    // Also consider: slot is live if it's a parameter
                    // (always live across the entire function)
                    if (!read_in_func && s < proto->numparams) {
                        read_in_func = true;
                    }

                    if (read_in_func) {
                        XrBpLiveSlot *ls = &info->live[info->nlive];
                        ls->slot = (uint8_t) s;
                        ls->tag = effective_tag;
                        info->nlive++;
                    }
                }

                bp->nloops++;
                break;  // this PC matched, move on
            }
        }
    }

    // Trim if no loops were actually populated
    if (bp->nloops == 0) {
        xr_free(bp->loops);
        bp->loops = NULL;
    }
}

/* ========== Public API ========== */

XrBlueprint *xr_blueprint_generate(XrProto *proto) {
    XR_DCHECK(proto != NULL, "xr_blueprint_generate: NULL proto");

    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    if (code_count == 0)
        return NULL;

    XrBlueprint *bp = (XrBlueprint *) xr_calloc(1, sizeof(XrBlueprint));

    // Generate loop live maps for OSR
    generate_loop_live_maps(bp, proto);

    return bp;
}

void xr_blueprint_free(XrBlueprint *bp) {
    if (!bp)
        return;

    if (bp->loops)
        xr_free(bp->loops);

    xr_free(bp);
}
