/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xemit_optimize.c - Emission-time peephole optimization
 *
 * KEY CONCEPT:
 *   Real-time optimization during instruction emission.
 *   Post-processing optimization is in xpeephole.c.
 */

#include "xemit.h"
#include "../../base/xchecks.h"
#include <stdio.h>

static inline bool is_move(XrInstruction inst) {
    return GET_OPCODE(inst) == OP_MOVE;
}

static inline int get_dest(XrInstruction inst) {
    return GETARG_A(inst);
}

static inline void replace_instruction(XrEmitter *e, int pc, XrInstruction new_inst) {
    XrInstruction *inst_ptr = PROTO_CODE_PTR(e->proto, pc);
    *inst_ptr = new_inst;
}

// Redundant MOVE elimination:
// MOVE R[a], R[b] followed by MOVE R[a], R[c] -> keep only the second
bool optimize_redundant_move(XrEmitter *e, XrInstruction inst) {
    if (!is_move(inst)) {
        return false;
    }
    
    if (!is_move(e->window.last_inst)) {
        return false;
    }
    
    int last_dest = get_dest(e->window.last_inst);
    int curr_dest = get_dest(inst);
    
    if (last_dest == curr_dest) {
        if (e->debug_mode) {
            printf("[Optimize] Redundant MOVE eliminated: R[%d]\n", last_dest);
        }
        
        replace_instruction(e, e->window.last_pc, inst);
        e->window.last_inst = inst;
        
        return true;
    }
    
    return false;
}

