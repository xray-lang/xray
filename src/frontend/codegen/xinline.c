/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xinline.c - Function inlining analyzer implementation
*/

#include "xinline.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Optimization statistics
InlineStats g_inline_stats = {0};

/* ========== Helper Functions ========== */

/*
** Detect if function has loops (backward jumps)
*/
bool xr_inline_has_loops(XrProto *proto) {
    XR_DCHECK(proto != NULL, "inline_has_loops: NULL proto");
    for (int pc = 0; pc < PROTO_CODE_COUNT(proto); pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        OpCode op = GET_OPCODE(inst);
        
        if (op == OP_JMP) {
            int offset = GETARG_sJ(inst);
            int target = pc + 1 + offset;
            
            // Backward jump indicates loop
            if (target <= pc) {
                return true;
            }
        }
    }
    return false;
}

/*
** Detect if function has recursive calls
** Direct recursion: OP_CALLSELF instruction
** Indirect recursion: detected via call graph DFS in xr_inline_detect_indirect_recursion
*/
bool xr_inline_has_recursion(XrProto *proto) {
    XR_DCHECK(proto != NULL, "inline_has_recursion: NULL proto");
    // Direct recursion check
    for (int pc = 0; pc < PROTO_CODE_COUNT(proto); pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        OpCode op = GET_OPCODE(inst);
        
        if (op == OP_CALLSELF) {
            return true;
        }
    }
    return false;
}

/* ========== Indirect Recursion Detection ========== */

#define MAX_CALLGRAPH_NODES 128
#define MAX_CALLGRAPH_EDGES 8

typedef struct {
    XrProto *proto;
    int callees[MAX_CALLGRAPH_EDGES];  // indices into node array
    int callee_count;
    int8_t visit_state;  // 0=unvisited, 1=in_stack, 2=done
    bool is_recursive;   // set true if part of a cycle
} CallGraphNode;

static int cg_find_proto(CallGraphNode *nodes, int count, XrProto *proto) {
    for (int i = 0; i < count; i++) {
        if (nodes[i].proto == proto) return i;
    }
    return -1;
}

// Collect all protos from a proto tree into a flat array
static int cg_collect_protos(XrProto *proto, CallGraphNode *nodes, int count) {
    if (count >= MAX_CALLGRAPH_NODES) return count;
    nodes[count].proto = proto;
    nodes[count].callee_count = 0;
    nodes[count].visit_state = 0;
    nodes[count].is_recursive = false;
    count++;
    for (int i = 0; i < PROTO_PROTO_COUNT(proto); i++) {
        count = cg_collect_protos(PROTO_PROTO(proto, i), nodes, count);
    }
    return count;
}

// Build call edges by tracking OP_CLOSURE reg -> OP_CALL reg patterns
static void cg_build_edges(CallGraphNode *nodes, int count) {
    for (int ni = 0; ni < count; ni++) {
        XrProto *proto = nodes[ni].proto;
        int code_count = PROTO_CODE_COUNT(proto);
        
        // Track which proto index was loaded into which register (simple, last-write wins)
        int reg_proto[256];
        memset(reg_proto, -1, sizeof(reg_proto));
        
        for (int pc = 0; pc < code_count; pc++) {
            XrInstruction inst = PROTO_CODE(proto, pc);
            OpCode op = GET_OPCODE(inst);
            
            if (op == OP_CLOSURE) {
                int a = GETARG_A(inst);
                int bx = GETARG_Bx(inst);
                if (a < 256 && bx < PROTO_PROTO_COUNT(proto)) {
                    reg_proto[a] = bx;
                }
            } else if (op == OP_CALL || op == OP_CALL_STATIC || op == OP_TAILCALL) {
                int a = GETARG_A(inst);
                if (a < 256 && reg_proto[a] >= 0) {
                    XrProto *callee = PROTO_PROTO(proto, reg_proto[a]);
                    int callee_idx = cg_find_proto(nodes, count, callee);
                    if (callee_idx >= 0 && nodes[ni].callee_count < MAX_CALLGRAPH_EDGES) {
                        nodes[ni].callees[nodes[ni].callee_count++] = callee_idx;
                    }
                }
            } else {
                // Any instruction that writes to a register invalidates the tracking
                int a = GETARG_A(inst);
                if (a < 256) reg_proto[a] = -1;
            }
        }
    }
}

// DFS to detect cycles; returns true if node is part of a cycle
static bool cg_dfs(CallGraphNode *nodes, int idx) {
    nodes[idx].visit_state = 1;  // in_stack
    for (int i = 0; i < nodes[idx].callee_count; i++) {
        int ci = nodes[idx].callees[i];
        if (nodes[ci].visit_state == 1) {
            // Back edge: cycle detected, mark both nodes
            nodes[ci].is_recursive = true;
            nodes[idx].is_recursive = true;
            return true;
        }
        if (nodes[ci].visit_state == 0) {
            if (cg_dfs(nodes, ci)) {
                nodes[idx].is_recursive = true;
            }
        }
    }
    nodes[idx].visit_state = 2;  // done
    return nodes[idx].is_recursive;
}

/*
** Detect indirect recursion across all protos in a module.
** Sets proto->is_recursive flag for protos involved in call cycles.
*/
void xr_inline_detect_indirect_recursion(XrProto *root) {
    XR_DCHECK(root != NULL, "inline_detect_indirect_recursion: NULL root");
    CallGraphNode nodes[MAX_CALLGRAPH_NODES];
    int count = cg_collect_protos(root, nodes, 0);
    if (count <= 1) return;
    
    cg_build_edges(nodes, count);
    
    // DFS from all unvisited nodes
    for (int i = 0; i < count; i++) {
        if (nodes[i].visit_state == 0) {
            cg_dfs(nodes, i);
        }
    }
    
    // Propagate results back to protos
    for (int i = 0; i < count; i++) {
        if (nodes[i].is_recursive) {
            nodes[i].proto->is_recursive = true;
        }
    }
}

/*
** Detect if function creates closures
*/
bool xr_inline_has_closure(XrProto *proto) {
    XR_DCHECK(proto != NULL, "inline_has_closure: NULL proto");
    for (int pc = 0; pc < PROTO_CODE_COUNT(proto); pc++) {
        XrInstruction inst = PROTO_CODE(proto, pc);
        OpCode op = GET_OPCODE(inst);
        
        if (op == OP_CLOSURE) {
            return true;
        }
    }
    return false;
}

/*
** Calculate function complexity
** Return value: 0-100, smaller means more suitable for inlining
*/
int xr_inline_complexity(XrProto *proto) {
    int complexity = 0;
    
    // Base complexity = instruction count
    complexity += PROTO_CODE_COUNT(proto);
    
    // Parameters and locals increase complexity
    complexity += proto->numparams * 2;
    
    // Nested functions increase complexity
    complexity += PROTO_PROTO_COUNT(proto) * 5;
    
    // Upvalues increase complexity
    complexity += PROTO_UPVAL_COUNT(proto) * 3;
    
    // Branches and jumps increase complexity
    for (int pc = 0; pc < PROTO_CODE_COUNT(proto); pc++) {
        OpCode op = GET_OPCODE(PROTO_CODE(proto, pc));
        if (op == OP_JMP || op == OP_TEST || op == OP_TESTSET) {
            complexity += 2;
        }
    }
    
    return complexity;
}

/* ========== Inline Analysis ========== */

/*
** Analyze if function is suitable for inlining
*/
bool xr_inline_analyze(XrProto *proto, InlineCandidate *candidate) {
    if (!proto || !candidate) {
        return false;
    }
    
    // Initialize candidate info
    memset(candidate, 0, sizeof(InlineCandidate));
    
    // Collect basic info
    candidate->instruction_count = PROTO_CODE_COUNT(proto);
    candidate->param_count = proto->numparams;
    candidate->local_count = proto->maxstacksize;
    
    // Detect features
    candidate->has_loops = xr_inline_has_loops(proto);
    candidate->has_recursion = xr_inline_has_recursion(proto) || proto->is_recursive;
    candidate->has_closure = xr_inline_has_closure(proto);
    
    // Determine if can be inlined
    candidate->can_inline = true;
    
    // Rule 1: instruction count must not be too large
    if (candidate->instruction_count > INLINE_MAX_INSTRUCTIONS) {
        candidate->can_inline = false;
        g_inline_stats.too_large++;
    }
    
    // Rule 2: must not have loops
    if (candidate->has_loops) {
        candidate->can_inline = false;
        g_inline_stats.has_loops++;
    }
    
    // Rule 3: must not be recursive
    if (candidate->has_recursion) {
        candidate->can_inline = false;
        g_inline_stats.has_recursion++;
    }
    
    // Rule 4: parameter count must not be too large
    if (candidate->param_count > INLINE_MAX_PARAMS) {
        candidate->can_inline = false;
    }
    
    // Rule 5: should not have closures (simplified)
    if (candidate->has_closure) {
        candidate->can_inline = false;
    }
    
    // Update statistics
    g_inline_stats.total_functions++;
    if (candidate->can_inline) {
        g_inline_stats.inline_candidates++;
    }
    
    return candidate->can_inline;
}

/*
** Mark all inline candidates
*/
int xr_inline_mark_candidates(XrProto *proto) {
    if (!proto) {
        return 0;
    }
    
    int candidate_count = 0;
    InlineCandidate candidate;
    
    // Analyze current function
    if (xr_inline_analyze(proto, &candidate)) {
        candidate_count++;
        // Persist inline decision into proto for JIT/AOT
        proto->inline_hint = 1;  // 1 = inline candidate
    }
    
    // Recursively analyze nested functions
    for (int i = 0; i < PROTO_PROTO_COUNT(proto); i++) {
        candidate_count += xr_inline_mark_candidates(PROTO_PROTO(proto, i));
    }
    
    return candidate_count;
}

/* ========== Statistics ========== */

void xr_inline_reset_stats(void) {
    memset(&g_inline_stats, 0, sizeof(InlineStats));
}

void xr_inline_print_stats(void) {
    if (g_inline_stats.total_functions > 0) {
        printf("\n=== Function Inline Analysis ===\n");
        printf("Total functions: %d\n", g_inline_stats.total_functions);
        printf("Inline candidates: %d\n", g_inline_stats.inline_candidates);
        printf("Too large: %d\n", g_inline_stats.too_large);
        printf("Has loops: %d\n", g_inline_stats.has_loops);
        printf("Recursive: %d\n", g_inline_stats.has_recursion);
        
        if (g_inline_stats.inline_candidates > 0) {
            int percentage = (g_inline_stats.inline_candidates * 100) / 
                            g_inline_stats.total_functions;
            printf("Inline ratio: %d%%\n", percentage);
        }
        printf("================================\n");
    }
}

