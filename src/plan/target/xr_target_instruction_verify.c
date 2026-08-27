/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_instruction_verify.c - Independent typed instruction verifier
 *
 * KEY CONCEPT:
 *   Instruction rows are optional per function. A non-empty function group
 *   must independently prove a closed i64 program; absence means execution is
 *   unavailable, never an empty successful program. Control flow is proved
 *   from the rows themselves: the basic-block partition, the legality of every
 *   jump target, the reachability of every block, and the definite assignment
 *   of every operand on every path are all rederived here.
 */

#include "xr_target_instruction_verify.h"
#include "xr_i64_overflow_target_instruction.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../runtime/value/xtransfer_mode.h"
#include "../semantic/xr_semantic_array_member_shape.h"
#include "../semantic/xr_semantic_array_type_shape.h"
#include "../semantic/xr_semantic_class_shape.h"
#include "../../stdlib/xstdlib_metadata.h"
#include "../semantic/xr_program_semantic_closure.h"
#include <stdio.h>
#include <string.h>

static bool report(char *error, size_t error_size, const char *code,
                   const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", code, detail);
    return false;
}

static bool range_valid(uint32_t begin, uint32_t count, uint32_t total) {
    return begin <= total && count <= total - begin;
}

/* The two slot families this executable program may name. A comparison answers
 * a truth value, and the plan gives a `bool` semantic value a one-byte I1 slot,
 * so the truth family exists because no spelling here can turn that slot into
 * the eight-byte signed one every arithmetic row writes. Both families are one
 * exact shape apiece, stated once and selected by the opcode; neither widens
 * the other and no row may read one where its opcode names the other. */
typedef enum SlotFamily {
    SLOT_FAMILY_U8 = 0,
    SLOT_FAMILY_I64,
    SLOT_FAMILY_BOOL,
    SLOT_FAMILY_DYN_VALUE,
    SLOT_FAMILY_AGGREGATE,
} SlotFamily;

static bool rep_is_family(const XrTargetMachineRepRecord *rep,
                          SlotFamily family, uint8_t ownership) {
    if (!rep)
        return false;
    if (family == SLOT_FAMILY_DYN_VALUE)
        return rep->kind == XR_MACHINE_REP_DYN_VALUE &&
               rep->root_kind == XR_TARGET_ROOT_DYNAMIC && rep->ownership == ownership &&
               rep->memory_size != 0 && rep->memory_align != 0 &&
               rep->null_encoding == XR_TARGET_NULL_TAGGED;
    if (family == SLOT_FAMILY_AGGREGATE)
        return rep->kind == XR_MACHINE_REP_AGGREGATE && rep->register_bits != 0 &&
               rep->register_bits == rep->memory_size * 8u && rep->memory_size != 0 &&
               rep->memory_align != 0 &&
               rep->signedness == XR_TARGET_SIGN_NONE &&
               rep->root_kind == XR_TARGET_ROOT_NONE &&
               rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
               rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE && rep->lane_count == 0 &&
               rep->reserved == 0;
    if (rep->root_kind != XR_TARGET_ROOT_NONE || ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;
    if (family == SLOT_FAMILY_BOOL)
        return rep->kind == XR_MACHINE_REP_I1 && rep->register_bits == 1 &&
               rep->memory_size == 1 && rep->memory_align == 1 &&
               rep->signedness == XR_TARGET_SIGN_NONE;
    if (family == SLOT_FAMILY_U8)
        return rep->kind == XR_MACHINE_REP_U8 && rep->register_bits == 8 &&
               rep->memory_size == 1 && rep->memory_align == 1 &&
               rep->signedness == XR_TARGET_SIGN_UNSIGNED;
    return rep->kind == XR_MACHINE_REP_I64 && rep->register_bits == 64 &&
           rep->memory_size == 8 && rep->memory_align == 8 &&
           rep->signedness == XR_TARGET_SIGN_SIGNED;
}

static bool slot_is_family(const XrTargetPlan *plan,
                           const XrTargetFunctionRecord *function,
                           uint32_t function_index, uint32_t slot_index,
                           SlotFamily family, uint8_t ownership,
                           const XrTargetSlotRecord **out) {
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    if (!slots || !range_valid(function->slot_begin, function->slot_count,
                               slot_count) ||
        slot_index < function->slot_begin ||
        slot_index - function->slot_begin >= function->slot_count)
        return false;
    const XrTargetSlotRecord *slot = &slots[slot_index];
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(plan, slot->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(plan, slot->memory_rep);
    uint32_t width = family == SLOT_FAMILY_BOOL || family == SLOT_FAMILY_U8
                         ? 1u
                         : family == SLOT_FAMILY_DYN_VALUE
                               ? memory_rep->memory_size
                               : family == SLOT_FAMILY_AGGREGATE
                                     ? memory_rep->memory_size
                                     : 8u;
    uint32_t align = family == SLOT_FAMILY_DYN_VALUE
                         ? memory_rep->memory_align
                         : family == SLOT_FAMILY_AGGREGATE ? memory_rep->memory_align : width;
    if (slot->id != slot_index || slot->function != function_index ||
        slot->size != width || slot->align != align ||
        slot->root_kind != (family == SLOT_FAMILY_DYN_VALUE
                                ? XR_TARGET_ROOT_DYNAMIC
                                : XR_TARGET_ROOT_NONE) ||
        slot->ownership != ownership ||
        !rep_is_family(register_rep, family, ownership) ||
        !rep_is_family(memory_rep, family, ownership) ||
        (family == SLOT_FAMILY_DYN_VALUE &&
         (register_rep->memory_size != memory_rep->memory_size ||
          register_rep->memory_align != memory_rep->memory_align)))
        return false;
    if (out)
        *out = slot;
    return true;
}

/* Row shape only. That an operand is already defined where it is read is a
 * whole-group question once a function has several blocks, so it is proved
 * separately by the control-flow fixed point rather than by the order this
 * pass happens to walk the table in. */
static bool slot_is_contract_rep(const XrTargetPlan *plan,
                                 const XrTargetFunctionRecord *function,
                                 uint32_t function_index, uint32_t slot,
                                 uint8_t rep, uint8_t instruction_ownership,
                                 bool result_ownership,
                                 const XrTargetSlotRecord **out) {
    uint8_t ownership = UINT8_MAX;
    bool trivial_rep = rep == XR_TARGET_INSTRUCTION_REP_U8 ||
                       rep == XR_TARGET_INSTRUCTION_REP_I64 ||
                       rep == XR_TARGET_INSTRUCTION_REP_BOOL ||
                       rep == XR_TARGET_INSTRUCTION_REP_AGGREGATE;
    if (trivial_rep) {
        bool exact = result_ownership
                         ? instruction_ownership ==
                               XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP_TRIVIAL
                         : instruction_ownership ==
                               XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_BORROW;
        if (!exact)
            return false;
        ownership = XR_TARGET_OWNERSHIP_TRIVIAL;
    }
    else if (result_ownership) {
        if (instruction_ownership == XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP_TRIVIAL)
            ownership = XR_TARGET_OWNERSHIP_TRIVIAL;
        else if (instruction_ownership == XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP_BORROW)
            ownership = XR_TARGET_OWNERSHIP_BORROWED;
        else if (instruction_ownership == XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP_OWNED)
            ownership = XR_TARGET_OWNERSHIP_OWNED;
    } else if (instruction_ownership == XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_BORROW)
        ownership = XR_TARGET_OWNERSHIP_BORROWED;
    else if (instruction_ownership == XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_CONSUME)
        ownership = XR_TARGET_OWNERSHIP_OWNED;
    if (ownership == UINT8_MAX)
        return false;
    if (rep == XR_TARGET_INSTRUCTION_REP_I64)
        return slot_is_family(plan, function, function_index, slot,
                              SLOT_FAMILY_I64, ownership, out);
    if (rep == XR_TARGET_INSTRUCTION_REP_U8)
        return slot_is_family(plan, function, function_index, slot,
                              SLOT_FAMILY_U8, ownership, out);
    if (rep == XR_TARGET_INSTRUCTION_REP_BOOL)
        return slot_is_family(plan, function, function_index, slot,
                              SLOT_FAMILY_BOOL, ownership, out);
    if (rep == XR_TARGET_INSTRUCTION_REP_DYN_VALUE)
        return slot_is_family(plan, function, function_index, slot,
                              SLOT_FAMILY_DYN_VALUE, ownership, out);
    if (rep == XR_TARGET_INSTRUCTION_REP_AGGREGATE)
        return slot_is_family(plan, function, function_index, slot,
                              SLOT_FAMILY_AGGREGATE, ownership, out);
    return false;
}

/* Counted independently of the rows so that a group binding fewer arguments
 * than the frame declares parameters can never become executable. */
static uint32_t function_parameter_slot_count(const XrTargetPlan *plan,
                                              const XrTargetFunctionRecord *function) {
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    if (!slots || !range_valid(function->slot_begin, function->slot_count,
                               slot_count))
        return UINT32_MAX;
    uint32_t parameters = 0;
    for (uint32_t i = 0; i < function->slot_count; i++)
        parameters += slots[function->slot_begin + i].role == XR_TARGET_SLOT_PARAMETER;
    return parameters;
}

static bool row_immediate_is_exact(
    const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract) {
    switch ((XrTargetInstructionImmediateKind) contract->immediate_kind) {
        case XR_TARGET_INSTRUCTION_IMMEDIATE_NONE:
            return row->immediate_bits == 0;
        case XR_TARGET_INSTRUCTION_IMMEDIATE_I64:
        case XR_TARGET_INSTRUCTION_IMMEDIATE_BRANCH_TARGETS:
            return true;
        case XR_TARGET_INSTRUCTION_IMMEDIATE_PARAMETER_ORDINAL:
            return row->immediate_bits < XR_TARGET_INSTRUCTION_MAX_PARAMETERS;
        case XR_TARGET_INSTRUCTION_IMMEDIATE_JUMP_TARGET:
            return XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(row->immediate_bits) == 0;
        case XR_TARGET_INSTRUCTION_IMMEDIATE_CALL_RECORD:
        case XR_TARGET_INSTRUCTION_IMMEDIATE_ENTRY_EXPECTATION:
        case XR_TARGET_INSTRUCTION_IMMEDIATE_FIELD_RECORD:
        case XR_TARGET_INSTRUCTION_IMMEDIATE_LAYOUT_RECORD:
        case XR_TARGET_INSTRUCTION_IMMEDIATE_OVERFLOW_PREDICATE_RECORD:
            return row->immediate_bits <= UINT32_MAX;
        case XR_TARGET_INSTRUCTION_IMMEDIATE_COROUTINE_STATE:
            return XR_TARGET_INSTRUCTION_SUSPEND_STATE(
                       row->immediate_bits) != UINT32_MAX &&
                   XR_TARGET_INSTRUCTION_SUSPEND_RESUME(
                       row->immediate_bits) != UINT32_MAX;
        default:
            return false;
    }
}

/* One bitmap word per 64 slots. The proof below reasons about sets of slots
 * per block, so the whole working set is blocks times words and is refused
 * before it is allocated rather than trimmed. */
#define CONTROL_FLOW_MAX_BITMAP_WORDS (UINT64_C(1) << 20)

static bool control_flow_slot_local(uint32_t slot, uint32_t slot_begin,
                                    uint32_t slot_count, uint32_t *out) {
    if (slot < slot_begin || slot - slot_begin >= slot_count)
        return false;
    *out = slot - slot_begin;
    return true;
}

/* Exact search rather than a range search: a target that is not itself the
 * first row of a block is refused, which is what forbids a jump into the
 * middle of a block. */
static bool control_flow_target_block(const uint32_t *block_start,
                                      uint32_t block_count, uint32_t target,
                                      uint32_t *out_block) {
    uint32_t low = 0;
    uint32_t high = block_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (block_start[middle] < target)
            low = middle + 1u;
        else
            high = middle;
    }
    if (low >= block_count || block_start[low] != target)
        return false;
    *out_block = low;
    return true;
}

typedef struct ControlFlowProof {
    uint32_t block_count;
    uint32_t words;
    uint32_t *block_start;   /* block_count + 1 entries, last one is row_count */
    uint32_t *successors;    /* two per block, UINT32_MAX where absent */
    uint32_t *predecessors;  /* flattened, indexed by predecessor_begin */
    uint32_t *predecessor_begin; /* block_count + 1 entries */
    uint32_t *cursor;        /* depth-first successor cursor, then postorder */
    uint32_t *order;         /* reverse postorder */
    uint8_t *visited;
    uint64_t *defines;       /* per block: slots this block assigns */
    uint64_t *assigned;      /* whole group: slots already bound to a row */
    uint64_t *entry_defined; /* per block: slots defined on every path in */
    uint64_t *exit_defined;  /* per block: entry_defined | defines */
} ControlFlowProof;

static void control_flow_proof_dispose(ControlFlowProof *proof) {
    xr_free(proof->block_start);
    xr_free(proof->successors);
    xr_free(proof->predecessors);
    xr_free(proof->predecessor_begin);
    xr_free(proof->cursor);
    xr_free(proof->order);
    xr_free(proof->visited);
    xr_free(proof->defines);
    xr_free(proof->assigned);
    xr_free(proof->entry_defined);
    xr_free(proof->exit_defined);
    memset(proof, 0, sizeof(*proof));
}

static bool control_flow_proof_allocate(ControlFlowProof *proof,
                                        uint32_t block_count, uint32_t words) {
    proof->block_count = block_count;
    proof->words = words;
    size_t bitmap = (size_t) block_count * words;
    proof->block_start = (uint32_t *) xr_calloc(block_count + 1u, sizeof(uint32_t));
    proof->successors = (uint32_t *) xr_calloc((size_t) block_count * 2u, sizeof(uint32_t));
    proof->predecessors = (uint32_t *) xr_calloc((size_t) block_count * 2u, sizeof(uint32_t));
    proof->predecessor_begin = (uint32_t *) xr_calloc(block_count + 1u, sizeof(uint32_t));
    proof->cursor = (uint32_t *) xr_calloc(block_count, sizeof(uint32_t));
    proof->order = (uint32_t *) xr_calloc(block_count, sizeof(uint32_t));
    proof->visited = (uint8_t *) xr_calloc(block_count, 1);
    proof->defines = (uint64_t *) xr_calloc(bitmap, sizeof(uint64_t));
    proof->assigned = (uint64_t *) xr_calloc(words, sizeof(uint64_t));
    proof->entry_defined = (uint64_t *) xr_calloc(bitmap, sizeof(uint64_t));
    proof->exit_defined = (uint64_t *) xr_calloc(bitmap, sizeof(uint64_t));
    return proof->block_start && proof->successors && proof->predecessors &&
           proof->predecessor_begin && proof->cursor && proof->order &&
           proof->visited && proof->defines && proof->assigned &&
           proof->entry_defined && proof->exit_defined;
}

/* Every block must be reached from the entry. An unreachable block would make
 * the intersection over its predecessors empty, which reads as "everything is
 * defined" and would let a use escape the proof; refusing the group is the
 * fail-closed answer, and it also keeps unreachable rows out of the executable
 * image. The traversal records a postorder, whose reverse is the iteration
 * order the fixed point below converges in. */
static bool control_flow_order_is_total(ControlFlowProof *proof) {
    uint32_t stack_top = 0;
    uint32_t *stack = proof->order;
    uint32_t postorder = proof->block_count;
    stack[stack_top] = 0;
    proof->visited[0] = 1;
    proof->cursor[0] = 0;
    while (true) {
        uint32_t block = stack[stack_top];
        if (proof->cursor[block] < 2u) {
            uint32_t successor =
                proof->successors[block * 2u + proof->cursor[block]++];
            if (successor != UINT32_MAX && !proof->visited[successor]) {
                proof->visited[successor] = 1;
                proof->cursor[successor] = 0;
                stack[++stack_top] = successor;
            }
            continue;
        }
        /* The stack shrinks from the front of the array exactly as fast as the
         * postorder grows from its back, so one array carries both and the
         * finished order is already reversed. */
        if (postorder == 0 || postorder - 1u < stack_top)
            return false;
        proof->order[--postorder] = block;
        if (stack_top == 0)
            break;
        stack_top--;
    }
    for (uint32_t block = 0; block < proof->block_count; block++)
        if (!proof->visited[block])
            return false;
    return postorder == 0;
}

bool xr_target_instruction_rows_control_flow_is_exact(
    const XrTargetInstructionRecord *rows, uint32_t row_count,
    uint32_t slot_begin, uint32_t slot_count,
    const XrTargetCallRecord *calls, uint32_t call_count,
    const XrTargetCallArgumentRecord *call_arguments,
    uint32_t call_argument_count,
    const XrTargetEntryExpectationRecord *entry_expectations,
    uint32_t entry_expectation_count,
    const XrTargetCoroutineStateRecord *coroutines,
    uint32_t coroutine_count) {
    if (!rows || !row_count || !slot_count ||
        slot_count > UINT32_MAX - slot_begin ||
        !xr_target_instruction_is_terminator(rows[row_count - 1u].opcode))
        return false;
    /* The block partition is derived, not declared: a block begins at the first
     * row and after every terminator, so every block ends in a terminator by
     * construction and the last row cannot fall off the end of the table. */
    uint32_t block_count = 1;
    for (uint32_t i = 0; i + 1u < row_count; i++) {
        if (!xr_target_instruction_is_terminator(rows[i].opcode))
            continue;
        if (block_count >= XR_TARGET_INSTRUCTION_MAX_BLOCKS)
            return false;
        block_count++;
    }
    uint32_t words = (slot_count + 63u) / 64u;
    if ((uint64_t) block_count * words > CONTROL_FLOW_MAX_BITMAP_WORDS)
        return false;

    ControlFlowProof proof = {0};
    if (!control_flow_proof_allocate(&proof, block_count, words)) {
        control_flow_proof_dispose(&proof);
        return false;
    }
    bool valid = true;
    uint32_t block = 0;
    proof.block_start[0] = 0;
    for (uint32_t i = 0; i + 1u < row_count; i++)
        if (xr_target_instruction_is_terminator(rows[i].opcode))
            proof.block_start[++block] = i + 1u;
    proof.block_start[block_count] = row_count;
    if (block + 1u != block_count)
        valid = false;

    /* Successor edges and the per-block definition set, both read straight off
     * the rows. A target is accepted only when it is the first row of a block
     * of this same group. */
    for (block = 0; block < block_count && valid; block++) {
        const XrTargetInstructionRecord *terminator =
            &rows[proof.block_start[block + 1u] - 1u];
        const XrTargetInstructionContract *contract =
            xr_target_instruction_contract(terminator->opcode);
        uint64_t immediate = terminator->immediate_bits;
        proof.successors[block * 2u] = UINT32_MAX;
        proof.successors[block * 2u + 1u] = UINT32_MAX;
        switch (contract ? (XrTargetInstructionControlKind) contract->control_kind
                         : XR_TARGET_INSTRUCTION_CONTROL_NONE) {
            case XR_TARGET_INSTRUCTION_CONTROL_RETURN:
                break;
            case XR_TARGET_INSTRUCTION_CONTROL_JUMP:
                valid = XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(immediate) == 0 &&
                        control_flow_target_block(
                            proof.block_start, block_count,
                            XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(immediate),
                            &proof.successors[block * 2u]);
                break;
            /* Both branch rows carry the same pair of explicit edges; only the
             * width of the condition they read differs, and that is a row-shape
             * question rather than a control-flow one. */
            case XR_TARGET_INSTRUCTION_CONTROL_BRANCH:
                valid = control_flow_target_block(
                            proof.block_start, block_count,
                            XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(immediate),
                            &proof.successors[block * 2u]) &&
                        control_flow_target_block(
                            proof.block_start, block_count,
                            XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(immediate),
                            &proof.successors[block * 2u + 1u]);
                break;
            case XR_TARGET_INSTRUCTION_CONTROL_SUSPEND: {
                uint32_t state_index =
                    XR_TARGET_INSTRUCTION_SUSPEND_STATE(immediate);
                const XrTargetCoroutineStateRecord *state =
                    coroutines && state_index < coroutine_count
                        ? &coroutines[state_index]
                        : NULL;
                if (!state || state->id != state_index ||
                    state->function != rows[0].function ||
                    state->resume_predecessor != state->suspend_block ||
                    state->resume_predecessor_ordinal != 0 ||
                    state->resume_instruction !=
                        XR_TARGET_INSTRUCTION_SUSPEND_RESUME(immediate) ||
                    state->direct_call != XR_SEMANTIC_INDEX_NONE ||
                    state->result_slot != XR_SEMANTIC_INDEX_NONE ||
                    state->flags != 0) {
                    valid = false;
                    break;
                }
                if (!control_flow_target_block(
                        proof.block_start, block_count,
                        XR_TARGET_INSTRUCTION_SUSPEND_RESUME(immediate),
                        &proof.successors[block * 2u]))
                    valid = false;
                break;
            }
            default:
                valid = false;
                break;
        }
        for (uint32_t i = proof.block_start[block];
             i < proof.block_start[block + 1u] && valid; i++) {
            uint32_t local = 0;
            if (rows[i].result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE)
                continue;
            if (!control_flow_slot_local(rows[i].result_slot, slot_begin,
                                         slot_count, &local)) {
                valid = false;
                break;
            }
            /* Single assignment across the whole group, not merely within a
             * block: two rows binding one slot would make the value a use
             * reads depend on which path ran, which no proof below could
             * reconstruct. */
            uint64_t bit = UINT64_C(1) << (local % 64u);
            if (proof.assigned[local / 64u] & bit) {
                valid = false;
                break;
            }
            proof.assigned[local / 64u] |= bit;
            proof.defines[(size_t) block * words + local / 64u] |= bit;
        }
    }

    if (valid)
        valid = control_flow_order_is_total(&proof);
    if (!valid) {
        control_flow_proof_dispose(&proof);
        return false;
    }

    for (block = 0; block < block_count; block++)
        for (uint32_t successor = 0; successor < 2u; successor++) {
            uint32_t target = proof.successors[block * 2u + successor];
            if (target != UINT32_MAX)
                proof.predecessor_begin[target + 1u]++;
        }
    for (block = 0; block < block_count; block++)
        proof.predecessor_begin[block + 1u] += proof.predecessor_begin[block];
    memcpy(proof.cursor, proof.predecessor_begin,
           (size_t) block_count * sizeof(uint32_t));
    for (block = 0; block < block_count; block++)
        for (uint32_t successor = 0; successor < 2u; successor++) {
            uint32_t target = proof.successors[block * 2u + successor];
            if (target != UINT32_MAX)
                proof.predecessors[proof.cursor[target]++] = block;
        }

    /* Definite assignment. A slot is defined on entry to a block only when it
     * is defined on exit from every predecessor, so the fixed point starts from
     * "everything" everywhere but the entry and can only shrink. The entry
     * itself starts empty, which is what makes a parameter row a real
     * definition rather than an implicitly live slot. */
    for (block = 1; block < block_count; block++)
        memset(&proof.exit_defined[(size_t) block * words], 0xff,
               (size_t) words * sizeof(uint64_t));
    for (uint32_t word = 0; word < words; word++)
        proof.exit_defined[word] = proof.defines[word];
    for (block = 1; block < block_count; block++)
        memset(&proof.entry_defined[(size_t) block * words], 0xff,
               (size_t) words * sizeof(uint64_t));
    /* Reverse postorder makes this converge in as many rounds as the graph's
     * deepest loop nesting; a graph that still moves after one round per block
     * is refused rather than iterated without bound. */
    bool changed = true;
    for (uint32_t round = 0; round <= block_count + 1u && changed; round++) {
        changed = false;
        for (uint32_t position = 0; position < block_count; position++) {
            block = proof.order[position];
            uint64_t *entry = &proof.entry_defined[(size_t) block * words];
            uint64_t *exit = &proof.exit_defined[(size_t) block * words];
            const uint64_t *defines = &proof.defines[(size_t) block * words];
            for (uint32_t word = 0; word < words; word++) {
                uint64_t incoming = block == 0 ? 0 : UINT64_MAX;
                for (uint32_t p = proof.predecessor_begin[block];
                     block != 0 && p < proof.predecessor_begin[block + 1u]; p++)
                    incoming &=
                        proof.exit_defined[(size_t) proof.predecessors[p] * words +
                                           word];
                if (entry[word] != incoming) {
                    entry[word] = incoming;
                    changed = true;
                }
                if (exit[word] != (incoming | defines[word])) {
                    exit[word] = incoming | defines[word];
                    changed = true;
                }
            }
        }
    }
    valid = !changed;

    /* Every operand read must already be in the definite set at that point:
     * what the block inherits, plus what its own earlier rows assigned. */
    for (block = 0; block < block_count && valid; block++) {
        uint64_t *live = &proof.entry_defined[(size_t) block * words];
        for (uint32_t i = proof.block_start[block];
             i < proof.block_start[block + 1u] && valid; i++) {
            const XrTargetInstructionRecord *row = &rows[i];
            if (row->operand_count > 2u) {
                valid = false;
                break;
            }
            for (uint32_t operand = 0; operand < row->operand_count; operand++) {
                uint32_t local = 0;
                if (!control_flow_slot_local(row->operand_slots[operand],
                                             slot_begin, slot_count, &local) ||
                    (live[local / 64u] & (UINT64_C(1) << (local % 64u))) == 0) {
                    valid = false;
                    break;
                }
            }
            const XrTargetInstructionContract *contract =
                xr_target_instruction_contract(row->opcode);
            if (valid && contract &&
                (contract->dispatch_kind == XR_TARGET_INSTRUCTION_DISPATCH_CALL ||
                 contract->dispatch_kind ==
                     XR_TARGET_INSTRUCTION_DISPATCH_CALL_AGGREGATE ||
                 contract->dispatch_kind ==
                     XR_TARGET_INSTRUCTION_DISPATCH_ENTRY_CALL ||
                 contract->dispatch_kind == XR_TARGET_INSTRUCTION_DISPATCH_NATIVE_LEAF)) {
                uint32_t call_index = (uint32_t) row->immediate_bits;
                if (contract->dispatch_kind ==
                    XR_TARGET_INSTRUCTION_DISPATCH_ENTRY_CALL) {
                    call_index = entry_expectations &&
                                         call_index < entry_expectation_count
                                     ? entry_expectations[call_index].call
                                     : UINT32_MAX;
                }
                const XrTargetCallRecord *call =
                    calls && call_index < call_count ? &calls[call_index] : NULL;
                if (!call || call->argument_begin > call_argument_count ||
                    call->argument_count >
                        call_argument_count - call->argument_begin) {
                    valid = false;
                    break;
                }
                for (uint16_t ordinal = 0;
                     ordinal < call->argument_count && valid; ordinal++) {
                    uint32_t local = 0;
                    uint32_t slot = call_arguments[
                        call->argument_begin + ordinal].caller_slot;
                    if (!control_flow_slot_local(slot, slot_begin, slot_count,
                                                 &local) ||
                        (live[local / 64u] &
                         (UINT64_C(1) << (local % 64u))) == 0)
                        valid = false;
                }
            }
            uint32_t local = 0;
            if (valid && row->result_slot != XR_TARGET_INSTRUCTION_SLOT_NONE &&
                control_flow_slot_local(row->result_slot, slot_begin, slot_count,
                                        &local))
                live[local / 64u] |= UINT64_C(1) << (local % 64u);
        }
    }
    control_flow_proof_dispose(&proof);
    return valid;
}

static bool call_rep_is_i64(const XrTargetPlan *plan, uint16_t rep) {
    const XrTargetMachineRepRecord *record =
        xr_target_plan_machine_rep(plan, rep);
    return record && record->kind == XR_MACHINE_REP_I64 &&
           record->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
}

static bool instruction_semantic_is_leaf_program(const XrSemanticPlan *semantic) {
    const XrSemanticProgramProvenance *provenance =
        semantic ? xr_semantic_plan_program_provenance(semantic) : NULL;
    return provenance &&
           provenance->program_family ==
               XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL &&
           provenance->schema == XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION &&
           provenance->program_schema == XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION &&
           provenance->type_count == 2 && provenance->type_field_count == 2 &&
           provenance->function_count == 2 && provenance->call_count == 1 &&
           provenance->module_count == 1 && provenance->dependency_count == 0 &&
           provenance->program_module_row == 0 &&
           provenance->program_dependency_binding_count == 0 &&
           provenance->reserved == 0 &&
           xr_semantic_plan_program_type_binding_count(semantic) == 2 &&
           xr_semantic_plan_program_type_field_binding_count(semantic) == 2 &&
           xr_semantic_plan_program_function_binding_count(semantic) == 2 &&
           xr_semantic_plan_program_call_binding_count(semantic) == 1;
}

static bool instruction_semantic_is_product_program(const XrSemanticPlan *semantic) {
    const XrSemanticProgramProvenance *provenance =
        semantic ? xr_semantic_plan_program_provenance(semantic) : NULL;
    return provenance &&
           provenance->program_family ==
               XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL &&
           provenance->schema == XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION &&
           provenance->program_schema == XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION &&
           provenance->type_count == 3 && provenance->type_field_count == 6 &&
           provenance->function_count == 3 && provenance->call_count == 2 &&
           provenance->module_count == 1 && provenance->dependency_count == 0 &&
           provenance->program_module_row == 0 &&
           provenance->program_dependency_binding_count == 0 && provenance->reserved == 0 &&
           xr_semantic_plan_program_type_binding_count(semantic) == 3 &&
           xr_semantic_plan_program_type_field_binding_count(semantic) == 6 &&
           xr_semantic_plan_program_function_binding_count(semantic) == 3 &&
           xr_semantic_plan_program_call_binding_count(semantic) == 2;
}

static bool instruction_product_required_functions(const XrSemanticPlan *semantic,
                                                   uint32_t callers[2],
                                                   uint32_t *callee_out) {
    if (callers) {
        callers[0] = XR_SEMANTIC_INDEX_NONE;
        callers[1] = XR_SEMANTIC_INDEX_NONE;
    }
    if (callee_out)
        *callee_out = XR_SEMANTIC_INDEX_NONE;
    if (!instruction_semantic_is_product_program(semantic) || !callers || !callee_out)
        return false;
    uint32_t caller_count = 0;
    for (uint32_t row = 0; row < 3; row++) {
        const XrSemanticProgramFunctionBinding *binding =
            xr_semantic_plan_program_function_binding(semantic, row);
        const XrSemanticFunctionRecord *function =
            binding ? xr_semantic_plan_function(semantic, binding->semantic_function) : NULL;
        if (!binding || !function || binding->program_row >= 3 ||
            function->parameter_count != 0 || function->block_count != 1 ||
            memcmp(binding->reserved, (uint8_t[3]) {0}, sizeof(binding->reserved)) != 0)
            return false;
        if (binding->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY && caller_count < 2)
            callers[caller_count++] = binding->semantic_function;
        else if (binding->flags == 0 && *callee_out == XR_SEMANTIC_INDEX_NONE)
            *callee_out = binding->semantic_function;
        else
            return false;
    }
    bool seen[2] = {false, false};
    for (uint32_t row = 0; row < 2; row++) {
        const XrSemanticProgramCallBinding *binding =
            xr_semantic_plan_program_call_binding(semantic, row);
        const XrSemanticOperationRecord *operation =
            binding ? xr_semantic_plan_operation(semantic, binding->operation) : NULL;
        uint32_t caller = operation && operation->function == callers[0]
                              ? 0u
                          : operation && operation->function == callers[1] ? 1u : UINT32_MAX;
        if (!binding || !operation || binding->program_row >= 2 || caller >= 2 ||
            seen[caller] || binding->target_function != *callee_out ||
            operation->opcode != XI_CALL || operation->operand_count != 1 ||
            operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT)
            return false;
        seen[caller] = true;
    }
    return caller_count == 2 && *callee_out != XR_SEMANTIC_INDEX_NONE && seen[0] && seen[1];
}

static bool instruction_stable_id_is_zero(XrStableId id) {
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (id.bytes[i] != 0)
            return false;
    return true;
}

static bool instruction_leaf_required_functions(const XrSemanticPlan *semantic,
                                                uint32_t *caller_out,
                                                uint32_t *callee_out) {
    if (caller_out)
        *caller_out = XR_SEMANTIC_INDEX_NONE;
    if (callee_out)
        *callee_out = XR_SEMANTIC_INDEX_NONE;
    if (!instruction_semantic_is_leaf_program(semantic) || !caller_out || !callee_out ||
        xr_semantic_plan_program_function_binding_count(semantic) != 2)
        return false;
    size_t semantic_function_count = xr_semantic_plan_function_count(semantic);
    bool program_rows[2] = {false, false};
    const XrSemanticProgramFunctionBinding *caller_binding = NULL;
    const XrSemanticProgramFunctionBinding *callee_binding = NULL;
    for (uint32_t i = 0; i < 2; i++) {
        const XrSemanticProgramFunctionBinding *binding =
            xr_semantic_plan_program_function_binding(semantic, i);
        if (!binding || binding->program_row >= 2 || program_rows[binding->program_row] ||
            binding->semantic_function >= semantic_function_count ||
            instruction_stable_id_is_zero(binding->program_function) ||
            (binding->flags & ~(XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY |
                                XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED)) != 0 ||
            memcmp(binding->reserved, (uint8_t[3]) {0}, sizeof(binding->reserved)) != 0 ||
            xr_semantic_plan_program_function_for_semantic_function(
                semantic, binding->semantic_function) != binding)
            return false;
        program_rows[binding->program_row] = true;
        if ((binding->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0) {
            if (*caller_out != XR_SEMANTIC_INDEX_NONE || caller_binding)
                return false;
            *caller_out = binding->semantic_function;
            caller_binding = binding;
        } else if (binding->flags == 0) {
            if (*callee_out != XR_SEMANTIC_INDEX_NONE || callee_binding)
                return false;
            *callee_out = binding->semantic_function;
            callee_binding = binding;
        } else {
            return false;
        }
    }
    const XrSemanticProgramCallBinding *call =
        xr_semantic_plan_program_call_binding(semantic, 0);
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->operation) : NULL;
    return program_rows[0] && program_rows[1] && caller_binding && callee_binding &&
           *caller_out != *callee_out && call && operation && call->program_row == 0 &&
           call->reserved == 0 && !instruction_stable_id_is_zero(call->program_call) &&
           !instruction_stable_id_is_zero(call->callsite) &&
           xr_semantic_plan_program_call_for_operation(semantic, call->operation) == call &&
           call->target_function == *callee_out && operation->function == *caller_out &&
           operation->opcode == XI_CALL &&
           xr_stable_id_equal(call->caller_program_function,
                              caller_binding->program_function) &&
           xr_stable_id_equal(call->callee_program_function,
                              callee_binding->program_function);
}

static bool call_rep_is_leaf_aggregate(const XrTargetPlan *plan, uint16_t rep,
                                       uint32_t *out_layout) {
    const XrTargetMachineRepRecord *record = xr_target_plan_machine_rep(plan, rep);
    uint32_t layout_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &layout_count);
    const XrTargetLayoutRecord *layout =
        record && layouts && record->detail < layout_count ? &layouts[record->detail] : NULL;
    if (!record || !layout || record->kind != XR_MACHINE_REP_AGGREGATE ||
        record->register_bits != 128 || record->memory_size != 16 ||
        record->memory_align != 8 || record->signedness != XR_TARGET_SIGN_NONE ||
        record->root_kind != XR_TARGET_ROOT_NONE ||
        record->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        record->null_encoding != XR_TARGET_NULL_NOT_NULLABLE || record->lane_count != 0 ||
        record->reserved != 0 || layout->id != record->detail ||
        layout->kind != XR_TARGET_LAYOUT_AGGREGATE || layout->fixed_prefix_size != 16 ||
        layout->align != 8 || layout->field_count != 2 || layout->root_field_count != 0)
        return false;
    if (out_layout)
        *out_layout = record->detail;
    return true;
}

static bool product_layout_is_exact(const XrTargetPlan *plan, uint32_t layout_index) {
    uint32_t layout_count = 0, field_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(plan, &field_count);
    const XrTargetLayoutRecord *layout =
        layouts && layout_index < layout_count ? &layouts[layout_index] : NULL;
    if (!layout || !fields || layout->id != layout_index ||
        layout->kind != XR_TARGET_LAYOUT_AGGREGATE || layout->fixed_prefix_size != 48 ||
        layout->align != 8 || layout->field_count != 6 || layout->root_field_count != 0 ||
        layout->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        !range_valid(layout->field_begin, 6, field_count))
        return false;
    for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
        const XrTargetFieldRecord *field = &fields[layout->field_begin + ordinal];
        const XrTargetMachineRepRecord *rep =
            xr_target_plan_machine_rep(plan, field->memory_rep);
        uint16_t expected_kind = ordinal == 2 ? XR_MACHINE_REP_U8 : XR_MACHINE_REP_I64;
        uint32_t expected_size = ordinal == 2 ? 1u : 8u;
        uint32_t expected_align = ordinal == 2 ? 1u : 8u;
        if (!rep || field->layout != layout_index || field->semantic_field != ordinal ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE || field->offset != ordinal * 8u ||
            field->size != expected_size || field->align != expected_align ||
            field->root_kind != XR_TARGET_ROOT_NONE || field->flags != 0 ||
            field->reserved != 0 || rep->kind != expected_kind ||
            rep->memory_size != expected_size || rep->memory_align != expected_align ||
            rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
            return false;
    }
    return true;
}

static bool call_rep_is_product(const XrTargetPlan *plan, uint16_t rep,
                                uint32_t *out_layout) {
    const XrTargetMachineRepRecord *record = xr_target_plan_machine_rep(plan, rep);
    if (!record || record->kind != XR_MACHINE_REP_AGGREGATE ||
        record->register_bits != 384 || record->memory_size != 48 ||
        record->memory_align != 8 || record->signedness != XR_TARGET_SIGN_NONE ||
        record->root_kind != XR_TARGET_ROOT_NONE ||
        record->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        record->null_encoding != XR_TARGET_NULL_NOT_NULLABLE || record->lane_count != 0 ||
        record->reserved != 0 || !product_layout_is_exact(plan, record->detail))
        return false;
    if (out_layout)
        *out_layout = record->detail;
    return true;
}

static bool call_rep_is_void(const XrTargetPlan *plan, uint16_t rep) {
    const XrTargetMachineRepRecord *record =
        xr_target_plan_machine_rep(plan, rep);
    return record && record->kind == XR_MACHINE_REP_VOID;
}

static bool callee_parameter_slot(const XrTargetPlan *plan, uint32_t function,
                                  uint16_t ordinal, uint32_t *out_slot) {
    uint32_t count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_instructions(plan, &count);
    uint32_t matches = 0;
    uint32_t slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    for (uint32_t i = 0; rows && i < count; i++) {
        const XrTargetInstructionContract *contract =
            xr_target_instruction_contract(rows[i].opcode);
        if (rows[i].function == function && contract &&
            contract->immediate_kind ==
                XR_TARGET_INSTRUCTION_IMMEDIATE_PARAMETER_ORDINAL &&
            rows[i].immediate_bits == ordinal) {
            matches++;
            slot = rows[i].result_slot;
        }
    }
    if (matches != 1)
        return false;
    if (out_slot)
        *out_slot = slot;
    return true;
}

static uint32_t callee_parameter_count(const XrTargetPlan *plan,
                                       uint32_t function) {
    uint32_t count = 0;
    uint32_t parameter_count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_instructions(plan, &count);
    for (uint32_t i = 0; rows && i < count; i++) {
        const XrTargetInstructionContract *contract =
            xr_target_instruction_contract(rows[i].opcode);
        if (rows[i].function == function && contract &&
            contract->immediate_kind ==
                XR_TARGET_INSTRUCTION_IMMEDIATE_PARAMETER_ORDINAL)
            parameter_count++;
    }
    return parameter_count;
}

static bool direct_i64_call_row_is_exact(
    const XrTargetPlan *plan, const XrTargetInstructionRecord *row,
    uint32_t function_index, const uint8_t *executable_functions,
    uint32_t function_count) {
    uint32_t call_count = 0;
    uint32_t argument_count = 0;
    uint32_t slot_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(plan, &argument_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    uint32_t call_index = (uint32_t) row->immediate_bits;
    const XrTargetCallRecord *call =
        calls && call_index < call_count ? &calls[call_index] : NULL;
    bool direct_local =
        call && call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
        call->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL;
    uint32_t program_graph_count = 0u;
    xr_target_plan_program_graphs(plan, &program_graph_count);
    bool program_direct =
        call && call->calling_convention == XR_TARGET_CALL_CONVENTION_PROGRAM_DIRECT &&
        call->target_kind == XR_TARGET_CALL_TARGET_PROGRAM_DIRECT &&
        program_graph_count == 1u;
    if (!call || (!direct_local && !program_direct) ||
        (!arguments && call->argument_count) || !slots ||
        call->id != call_index ||
        call->caller_function != function_index ||
        call->callee_function >= function_count ||
        !executable_functions[call->callee_function] || call->flags != 0 ||
        call->adapter_begin != 0 || call->adapter_count != 0 ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->array_hof_kind != XR_TARGET_ARRAY_HOF_NONE ||
        call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        row->result_slot != call->result_slot ||
        !call_rep_is_i64(plan, call->result_register_rep) ||
        !call_rep_is_i64(plan, call->result_memory_rep) ||
        !call_rep_is_void(plan, call->error_register_rep) ||
        !call_rep_is_void(plan, call->error_memory_rep) ||
        call->result_slot >= slot_count ||
        slots[call->result_slot].function != function_index ||
        slots[call->result_slot].register_rep != call->result_register_rep ||
        slots[call->result_slot].memory_rep != call->result_memory_rep ||
        call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin ||
        callee_parameter_count(plan, call->callee_function) !=
            call->argument_count)
        return false;
    for (uint16_t ordinal = 0; ordinal < call->argument_count; ordinal++) {
        const XrTargetCallArgumentRecord *argument =
            &arguments[call->argument_begin + ordinal];
        uint32_t parameter_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
        if (argument->call != call_index || argument->ordinal != ordinal ||
            argument->mode != XR_TARGET_CALL_VALUE ||
            (argument->ownership != XR_TARGET_CALL_READ &&
             argument->ownership != XR_TARGET_CALL_CONSUME) ||
            argument->transfer_mode != XR_TRANSFER_SHARE ||
            argument->flags != 0 ||
            argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
            argument->reserved8[0] != 0 || argument->reserved8[1] != 0 ||
            argument->reserved8[2] != 0 ||
            !call_rep_is_i64(plan, argument->register_rep) ||
            !call_rep_is_i64(plan, argument->memory_rep) ||
            !call_rep_is_i64(plan, argument->callee_register_rep) ||
            !call_rep_is_i64(plan, argument->callee_memory_rep) ||
            argument->caller_slot >= slot_count ||
            slots[argument->caller_slot].function != function_index ||
            argument->callee_slot >= slot_count ||
            slots[argument->callee_slot].function != call->callee_function ||
            slots[argument->caller_slot].register_rep !=
                argument->register_rep ||
            slots[argument->caller_slot].memory_rep != argument->memory_rep ||
            slots[argument->callee_slot].register_rep !=
                argument->callee_register_rep ||
            slots[argument->callee_slot].memory_rep !=
                argument->callee_memory_rep ||
            !callee_parameter_slot(plan, call->callee_function, ordinal,
                                   &parameter_slot) ||
            parameter_slot != argument->callee_slot)
            return false;
    }
    return true;
}

static bool native_target_leaf_i64_call_row_is_exact(
    const XrTargetPlan *plan, const XrTargetInstructionRecord *row,
    uint32_t function_index) {
    uint32_t call_count = 0;
    uint32_t slot_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    uint32_t call_index = (uint32_t) row->immediate_bits;
    const XrTargetCallRecord *call =
        calls && call_index < call_count ? &calls[call_index] : NULL;
    if (!call || !slots || call->id != call_index || call->caller_function != function_index ||
        call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !instruction_stable_id_is_zero(call->source_export_identity) ||
        !instruction_stable_id_is_zero(call->source_callee_identity) ||
        instruction_stable_id_is_zero(call->native_callee_identity) ||
        call->native_leaf <= XR_STDLIB_TARGET_LEAF_NONE ||
        call->native_leaf >= XR_STDLIB_TARGET_LEAF_COUNT || call->flags != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_NATIVE_TARGET_LEAF_SCALAR ||
        call->target_kind != XR_TARGET_CALL_TARGET_NATIVE_TARGET_LEAF_SCALAR ||
        call->argument_count != 0 || call->adapter_count != 0 ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->array_hof_kind != XR_TARGET_ARRAY_HOF_NONE ||
        call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        row->result_slot != call->result_slot ||
        !call_rep_is_i64(plan, call->result_register_rep) ||
        !call_rep_is_i64(plan, call->result_memory_rep) ||
        !call_rep_is_void(plan, call->error_register_rep) ||
        !call_rep_is_void(plan, call->error_memory_rep) ||
        call->result_slot >= slot_count ||
        slots[call->result_slot].function != function_index ||
        slots[call->result_slot].register_rep != call->result_register_rep ||
        slots[call->result_slot].memory_rep != call->result_memory_rep)
        return false;
    return true;
}

static bool direct_leaf_aggregate_call_row_is_exact(
    const XrTargetPlan *plan, const XrTargetInstructionRecord *row,
    uint32_t function_index, const uint8_t *executable_functions,
    uint32_t function_count) {
    uint32_t call_count = 0, argument_count = 0, slot_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(plan, &argument_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    uint32_t call_index = (uint32_t) row->immediate_bits;
    const XrTargetCallRecord *call =
        calls && call_index < call_count ? &calls[call_index] : NULL;
    uint32_t result_layout = XR_SEMANTIC_INDEX_NONE;
    if (!call || !arguments || !slots || call->id != call_index ||
        call->caller_function != function_index || call->callee_function >= function_count ||
        !executable_functions[call->callee_function] || call->flags != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        call->adapter_begin != 0 || call->adapter_count != 0 ||
        call->result_mode != XR_TARGET_CALL_CALLER_STORAGE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->caller_storage_slot != call->result_slot ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->array_hof_kind != XR_TARGET_ARRAY_HOF_NONE ||
        call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        row->result_slot != call->result_slot ||
        call->result_register_rep != call->result_memory_rep ||
        !call_rep_is_leaf_aggregate(plan, call->result_register_rep, &result_layout) ||
        !call_rep_is_void(plan, call->error_register_rep) ||
        !call_rep_is_void(plan, call->error_memory_rep) || call->result_slot >= slot_count ||
        slots[call->result_slot].function != function_index ||
        slots[call->result_slot].register_rep != call->result_register_rep ||
        slots[call->result_slot].memory_rep != call->result_memory_rep ||
        call->argument_count != 1 || call->argument_begin >= argument_count ||
        callee_parameter_count(plan, call->callee_function) != 1)
        return false;
    const XrTargetCallArgumentRecord *argument = &arguments[call->argument_begin];
    uint32_t parameter_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    uint32_t caller_layout = XR_SEMANTIC_INDEX_NONE;
    uint32_t callee_layout = XR_SEMANTIC_INDEX_NONE;
    return argument->call == call_index && argument->ordinal == 0 &&
           argument->mode == XR_TARGET_CALL_VALUE &&
           argument->ownership == XR_TARGET_CALL_READ &&
           argument->transfer_mode == XR_TRANSFER_SHARE && argument->flags == 0 &&
           argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
           argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
           argument->reserved8[2] == 0 &&
           argument->register_rep == argument->memory_rep &&
           argument->callee_register_rep == argument->callee_memory_rep &&
           call_rep_is_leaf_aggregate(plan, argument->register_rep, &caller_layout) &&
           call_rep_is_leaf_aggregate(plan, argument->callee_register_rep, &callee_layout) &&
           caller_layout == result_layout && callee_layout == result_layout &&
           argument->caller_slot < slot_count && argument->callee_slot < slot_count &&
           slots[argument->caller_slot].function == function_index &&
           slots[argument->callee_slot].function == call->callee_function &&
           slots[argument->caller_slot].register_rep == argument->register_rep &&
           slots[argument->caller_slot].memory_rep == argument->memory_rep &&
           slots[argument->callee_slot].register_rep == argument->callee_register_rep &&
           slots[argument->callee_slot].memory_rep == argument->callee_memory_rep &&
           callee_parameter_slot(plan, call->callee_function, 0, &parameter_slot) &&
           parameter_slot == argument->callee_slot;
}

static bool direct_product_call_row_is_exact(
    const XrTargetPlan *plan, const XrTargetInstructionRecord *row,
    uint32_t function_index, const uint8_t *executable_functions,
    uint32_t function_count) {
    uint32_t call_count = 0, argument_count = 0, slot_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    xr_target_plan_call_arguments(plan, &argument_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    uint32_t call_index = (uint32_t) row->immediate_bits;
    const XrTargetCallRecord *call =
        calls && call_index < call_count ? &calls[call_index] : NULL;
    uint32_t layout = XR_SEMANTIC_INDEX_NONE;
    return call && slots && call->id == call_index &&
           call->caller_function == function_index &&
           call->callee_function < function_count &&
           executable_functions[call->callee_function] && call->flags == 0 &&
           call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
           call->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL &&
           call->adapter_begin == 0 && call->adapter_count == 0 &&
           call->result_mode == XR_TARGET_CALL_CALLER_STORAGE &&
           call->result_ownership == XR_TARGET_CALL_NONE &&
           call->caller_storage_slot == call->result_slot &&
           call->error_slot == XR_SEMANTIC_INDEX_NONE &&
           call->error_mode == XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL &&
           call->array_intrinsic_kind == XR_TARGET_ARRAY_INTRINSIC_NONE &&
           call->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
           call->array_hof_kind == XR_TARGET_ARRAY_HOF_NONE &&
           call->array_result_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
           row->result_slot == call->result_slot &&
           call->result_register_rep == call->result_memory_rep &&
           call_rep_is_product(plan, call->result_register_rep, &layout) &&
           call_rep_is_void(plan, call->error_register_rep) &&
           call_rep_is_void(plan, call->error_memory_rep) &&
           call->result_slot < slot_count &&
           slots[call->result_slot].function == function_index &&
           slots[call->result_slot].register_rep == call->result_register_rep &&
           slots[call->result_slot].memory_rep == call->result_memory_rep &&
           slots[call->result_slot].size == 48 && slots[call->result_slot].align == 8 &&
           call->argument_count == 0 && argument_count == 0 &&
           callee_parameter_count(plan, call->callee_function) == 0;
}

static bool leaf_aggregate_layout_is_exact(const XrTargetPlan *plan, uint32_t layout_index) {
    uint32_t layout_count = 0, field_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(plan, &field_count);
    const XrTargetLayoutRecord *layout =
        layouts && layout_index < layout_count ? &layouts[layout_index] : NULL;
    if (!layout || !fields || layout->id != layout_index ||
        layout->kind != XR_TARGET_LAYOUT_AGGREGATE || layout->fixed_prefix_size != 16 ||
        layout->align != 8 || layout->field_count != 2 || layout->root_field_count != 0 ||
        layout->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        layout->field_begin > field_count || layout->field_count > field_count - layout->field_begin)
        return false;
    for (uint32_t ordinal = 0; ordinal < 2; ordinal++) {
        const XrTargetFieldRecord *field = &fields[layout->field_begin + ordinal];
        if (field->layout != layout_index || field->semantic_field != ordinal ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE || field->offset != ordinal * 8u ||
            field->size != 8 || field->align != 8 || field->root_kind != XR_TARGET_ROOT_NONE ||
            field->flags != 0 || field->reserved != 0 || !call_rep_is_i64(plan, field->memory_rep))
            return false;
    }
    return true;
}

static bool leaf_aggregate_slot_layout(const XrTargetPlan *plan,
                                       const XrTargetFunctionRecord *function,
                                       uint32_t function_index, uint32_t slot_index,
                                       uint32_t *out_layout) {
    const XrTargetSlotRecord *slot = NULL;
    if (!slot_is_family(plan, function, function_index, slot_index,
                        SLOT_FAMILY_AGGREGATE, XR_TARGET_OWNERSHIP_TRIVIAL, &slot))
        return false;
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(plan, slot->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(plan, slot->memory_rep);
    if (!register_rep || !memory_rep || register_rep->detail != memory_rep->detail ||
        !leaf_aggregate_layout_is_exact(plan, register_rep->detail))
        return false;
    if (out_layout)
        *out_layout = register_rep->detail;
    return true;
}

static bool product_slot_layout(const XrTargetPlan *plan,
                                const XrTargetFunctionRecord *function,
                                uint32_t function_index, uint32_t slot_index,
                                uint32_t *out_layout) {
    const XrTargetSlotRecord *slot = NULL;
    if (!slot_is_family(plan, function, function_index, slot_index,
                        SLOT_FAMILY_AGGREGATE, XR_TARGET_OWNERSHIP_TRIVIAL, &slot))
        return false;
    const XrTargetMachineRepRecord *register_rep =
        xr_target_plan_machine_rep(plan, slot->register_rep);
    const XrTargetMachineRepRecord *memory_rep =
        xr_target_plan_machine_rep(plan, slot->memory_rep);
    return register_rep && memory_rep &&
           register_rep->detail == memory_rep->detail &&
           call_rep_is_product(plan, slot->register_rep, out_layout) &&
           call_rep_is_product(plan, slot->memory_rep, NULL);
}

static bool leaf_aggregate_data_row_is_exact(
    const XrTargetPlan *plan, const XrTargetFunctionRecord *function,
    uint32_t function_index, const XrTargetInstructionRecord *row,
    const XrTargetInstructionContract *contract) {
    if (contract->dispatch_kind == XR_TARGET_INSTRUCTION_DISPATCH_AGGREGATE_GET) {
        uint32_t layout_count = 0, field_count = 0, slot_count = 0;
        const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &layout_count);
        const XrTargetFieldRecord *fields = xr_target_plan_fields(plan, &field_count);
        const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
        uint32_t layout = XR_SEMANTIC_INDEX_NONE;
        uint32_t field_index = (uint32_t) row->immediate_bits;
        bool product = instruction_semantic_is_product_program(
            xr_target_plan_semantic_plan(plan));
        if (!layouts || !fields || !slots || field_index >= field_count ||
            row->result_slot >= slot_count ||
            !(product ? product_slot_layout(plan, function, function_index,
                                            row->operand_slots[0], &layout)
                      : leaf_aggregate_slot_layout(plan, function, function_index,
                                                   row->operand_slots[0], &layout)))
            return false;
        const XrTargetFieldRecord *field = &fields[field_index];
        const XrTargetSlotRecord *result = &slots[row->result_slot];
        return field->layout == layout &&
               layout < layout_count && field_index >= layouts[layout].field_begin &&
               field_index - layouts[layout].field_begin < (product ? 6u : 2u) &&
               result->register_rep == field->memory_rep && result->memory_rep == field->memory_rep;
    }
    if (contract->dispatch_kind == XR_TARGET_INSTRUCTION_DISPATCH_AGGREGATE_MAKE) {
        uint32_t layout_count = 0, field_count = 0, slot_count = 0;
        const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &layout_count);
        const XrTargetFieldRecord *fields = xr_target_plan_fields(plan, &field_count);
        const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
        uint32_t layout_index = (uint32_t) row->immediate_bits;
        uint32_t result_layout = XR_SEMANTIC_INDEX_NONE;
        if (!layouts || !fields || !slots || layout_index >= layout_count ||
            row->operand_slots[0] >= slot_count || row->operand_slots[1] >= slot_count ||
            !leaf_aggregate_slot_layout(plan, function, function_index, row->result_slot,
                                        &result_layout) || result_layout != layout_index ||
            !leaf_aggregate_layout_is_exact(plan, layout_index))
            return false;
        const XrTargetLayoutRecord *layout = &layouts[layout_index];
        return slots[row->operand_slots[0]].register_rep == fields[layout->field_begin].memory_rep &&
               slots[row->operand_slots[0]].memory_rep == fields[layout->field_begin].memory_rep &&
               slots[row->operand_slots[1]].register_rep ==
                   fields[layout->field_begin + 1u].memory_rep &&
               slots[row->operand_slots[1]].memory_rep ==
                   fields[layout->field_begin + 1u].memory_rep;
    }
    return true;
}

static bool entry_i64_call_row_is_exact(
    const XrTargetPlan *plan, const XrTargetInstructionRecord *row,
    uint32_t function_index) {
    uint32_t expectation_count = 0;
    uint32_t call_count = 0;
    uint32_t argument_count = 0;
    uint32_t slot_count = 0;
    const XrTargetEntryExpectationRecord *expectations =
        xr_target_plan_entry_expectations(plan, &expectation_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(plan, &argument_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    uint32_t expectation_index = (uint32_t) row->immediate_bits;
    const XrTargetEntryExpectationRecord *expectation =
        expectations && expectation_index < expectation_count
            ? &expectations[expectation_index]
            : NULL;
    const XrTargetCallRecord *call =
        expectation && expectation->call < call_count
            ? &calls[expectation->call]
            : NULL;
    if (!expectation || !call || !slots ||
        expectation->id != expectation_index ||
        call->id != expectation->call ||
        call->caller_function != function_index || call->flags != 0 ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT ||
        call->target_kind != XR_TARGET_CALL_TARGET_SOURCE_EXPORT ||
        call->adapter_begin != 0 || call->adapter_count != 0 ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        row->result_slot != call->result_slot ||
        !call_rep_is_i64(plan, call->result_register_rep) ||
        !call_rep_is_i64(plan, call->result_memory_rep) ||
        call->result_slot >= slot_count ||
        slots[call->result_slot].function != function_index ||
        call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin ||
        expectation->parameter_count != call->argument_count)
        return false;
    for (uint16_t ordinal = 0; ordinal < call->argument_count; ordinal++) {
        const XrTargetCallArgumentRecord *argument =
            &arguments[call->argument_begin + ordinal];
        if (argument->call != call->id || argument->ordinal != ordinal ||
            argument->mode != XR_TARGET_CALL_VALUE ||
            (argument->ownership != XR_TARGET_CALL_READ &&
             argument->ownership != XR_TARGET_CALL_CONSUME) ||
            argument->transfer_mode != XR_TRANSFER_SHARE ||
            argument->flags != 0 ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
            !call_rep_is_i64(plan, argument->register_rep) ||
            !call_rep_is_i64(plan, argument->memory_rep) ||
            !call_rep_is_i64(plan, argument->callee_register_rep) ||
            !call_rep_is_i64(plan, argument->callee_memory_rep) ||
            argument->caller_slot >= slot_count ||
            slots[argument->caller_slot].function != function_index)
            return false;
    }
    return true;
}

static bool suspend_row_is_exact(const XrTargetPlan *plan,
                                 const XrTargetInstructionRecord *row,
                                 uint32_t function_index) {
    uint32_t coroutine_count = 0;
    const XrTargetCoroutineStateRecord *coroutines =
        xr_target_plan_coroutines(plan, &coroutine_count);
    uint32_t state_index =
        XR_TARGET_INSTRUCTION_SUSPEND_STATE(row->immediate_bits);
    const XrTargetCoroutineStateRecord *state =
        coroutines && state_index < coroutine_count
            ? &coroutines[state_index]
            : NULL;
    return state && state->id == state_index &&
           state->function == function_index &&
           state->resume_predecessor == state->suspend_block &&
           state->resume_predecessor_ordinal == 0 &&
           state->resume_instruction ==
               XR_TARGET_INSTRUCTION_SUSPEND_RESUME(row->immediate_bits) &&
           state->direct_call == XR_SEMANTIC_INDEX_NONE &&
           state->result_slot == XR_SEMANTIC_INDEX_NONE && state->flags == 0;
}

/* Re-derive the one managed mutation group without using builder state.  The
 * executable row is authority only when its call index, parameter slots,
 * semantic push shape, and BORROW/CONSUME boundary all describe the same
 * source-class Array.push site. */
static bool tagged_array_push_group_is_exact(
    const XrTargetPlan *plan, const XrTargetInstructionRecord *rows,
    uint32_t row_count, uint32_t function_index) {
    if (!plan || !rows || row_count != 4 ||
        rows[0].opcode != XR_TARGET_INSTRUCTION_PARAM_DYN_BORROW ||
        rows[1].opcode != XR_TARGET_INSTRUCTION_PARAM_DYN_OWNED ||
        rows[2].opcode != XR_TARGET_INSTRUCTION_ARRAY_PUSH_TAGGED ||
        rows[3].opcode != XR_TARGET_INSTRUCTION_RETURN_UNIT ||
        rows[0].immediate_bits != 0 || rows[1].immediate_bits != 1 ||
        rows[2].operand_slots[0] != rows[0].result_slot ||
        rows[2].operand_slots[1] != rows[1].result_slot)
        return false;

    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    const XrSemanticFunctionRecord *function =
        semantic ? xr_semantic_plan_function(semantic, function_index) : NULL;
    const XrSemanticBlockRecord *block =
        function && function->block_count == 1
            ? xr_semantic_plan_block(semantic, function->block_begin)
            : NULL;
    if (!semantic || !function || !block || function->parameter_count != 2 ||
        function->capture_count != 0 || block->function != function_index ||
        block->kind != XI_BLOCK_RETURN || block->operation_count != 3 ||
        block->successors[0] != XR_SEMANTIC_INDEX_NONE ||
        block->successors[1] != XR_SEMANTIC_INDEX_NONE)
        return false;

    uint32_t call_count = 0, argument_count = 0, operand_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(plan, &argument_count);
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t call_index = (uint32_t) rows[2].immediate_bits;
    const XrTargetCallRecord *call =
        calls && call_index < call_count ? &calls[call_index] : NULL;
    const XrSemanticOperationRecord *operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    uint32_t metadata_count = 0, child_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!call || !operation || !arguments || !operands || !metadata || !children ||
        call->id != call_index || call->caller_function != function_index ||
        call->argument_count != 2 || call->argument_begin > argument_count ||
        call->argument_count > argument_count - call->argument_begin || call->flags != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->result_slot != XR_SEMANTIC_INDEX_NONE ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_TAGGED ||
        call->array_hof_kind != XR_TARGET_ARRAY_HOF_NONE ||
        call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        operation->function != function_index || operation->block != function->block_begin ||
        operation->opcode != XI_CALL_METHOD ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR ||
        operation->operand_count != 2 || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->semantic_immediate != ((int64_t) XI_METHOD_SYMBOL_PUSH << 1) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        block->control_value != operation->result_value ||
        !xr_semantic_array_member_unit_type_is_exact(
            xr_semantic_plan_type(semantic, function->return_type)) ||
        !xr_semantic_array_member_unit_type_is_exact(
            xr_semantic_plan_type(semantic, operation->result_type)) ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        operation->return_provenance != XR_SEM_RETURN_NONE || operation->return_parameter != -1 ||
        operation->return_complete != 0)
        return false;

    const XrArrayMemberShape *shape =
        xr_array_member_shape(metadata[operation->metadata_begin], operation->operand_count);
    const XrSemanticOperandRecord *receiver_operand = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *element_operand = receiver_operand + 1;
    const XrSemanticTypeRecord *receiver_type =
        xr_semantic_plan_type(semantic, receiver_operand->type);
    if (!shape || strcmp(shape->selector, "push") != 0 ||
        shape->element_operand != 1 ||
        shape->element_access != XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE ||
        shape->reference_action != XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE ||
        shape->reference_drop != XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY ||
        !xr_semantic_array_type_row_is_exact(receiver_type) ||
        receiver_type->child_begin >= child_count)
        return false;
    uint32_t element_type_index = children[receiver_type->child_begin];
    const XrSemanticTypeRecord *element_type =
        xr_semantic_plan_type(semantic, element_type_index);
    if (!element_type ||
        xr_semantic_class_instance_type_source_class(semantic, element_type) ==
            XR_SEMANTIC_INDEX_NONE ||
        receiver_operand->role != XR_SEM_OPERAND_RECEIVER ||
        receiver_operand->parameter != -1 ||
        receiver_operand->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver_operand->ownership_action != XR_SEM_OPERAND_BORROW ||
        element_operand->type != element_type_index ||
        !xr_semantic_array_member_argument_is_exact(
            shape, element_operand, element_type, 1, element_type_index))
        return false;

    const XrSemanticParameterRecord *parameters[2] = {
        xr_semantic_plan_parameter(semantic, function->parameter_begin),
        xr_semantic_plan_parameter(semantic, function->parameter_begin + 1u),
    };
    const XrTargetCallArgumentRecord *call_arguments[2] = {
        &arguments[call->argument_begin], &arguments[call->argument_begin + 1u],
    };
    const XrSemanticOperandRecord *semantic_operands[2] = {
        receiver_operand, element_operand,
    };
    for (uint16_t ordinal = 0; ordinal < 2; ordinal++) {
        const XrSemanticParameterRecord *parameter = parameters[ordinal];
        const XrTargetCallArgumentRecord *argument = call_arguments[ordinal];
        const XrSemanticOperandRecord *operand = semantic_operands[ordinal];
        uint8_t semantic_ownership = ordinal == 0 ? XI_OWN_BORROWED : XI_OWN_OWNED;
        uint8_t target_ownership = ordinal == 0 ? XR_TARGET_CALL_BORROW
                                                : XR_TARGET_CALL_CONSUME;
        uint8_t storage = ordinal == 0 ? XR_TARGET_ARRAY_STORAGE_NONE
                                       : XR_TARGET_ARRAY_STORAGE_TAGGED;
        if (!parameter || parameter->function != function_index ||
            parameter->ordinal != ordinal || parameter->value != operand->value ||
            parameter->type != operand->type || parameter->mode != XR_PARAM_READ ||
            parameter->ownership != semantic_ownership ||
            parameter->transfer_mode != XR_TRANSFER_SHARE ||
            parameter->flags != XR_SEM_PARAMETER_REQUIRED || parameter->reserved != 0 ||
            argument->call != call_index || argument->semantic_operand !=
                operation->operand_begin + ordinal ||
            argument->semantic_value != operand->value ||
            argument->callee_parameter != XR_SEMANTIC_INDEX_NONE ||
            argument->caller_slot != rows[ordinal].result_slot ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE || argument->ordinal != ordinal ||
            argument->mode != XR_TARGET_CALL_VALUE || argument->ownership != target_ownership ||
            argument->transfer_mode != operand->transfer_mode || argument->flags != 0 ||
            argument->array_element_storage != storage || argument->reserved8[0] != 0 ||
            argument->reserved8[1] != 0 || argument->reserved8[2] != 0)
            return false;
        uint32_t parameter_operations = 0;
        for (uint32_t i = 0; i < block->operation_count; i++) {
            const XrSemanticOperationRecord *candidate =
                xr_semantic_plan_operation(semantic, block->operation_begin + i);
            if (!candidate || candidate->opcode != XI_PARAM ||
                candidate->result_value != parameter->value)
                continue;
            parameter_operations++;
            if (candidate->function != function_index ||
                candidate->result_type != parameter->type || candidate->operand_count != 0 ||
                candidate->semantic_immediate != ordinal ||
                candidate->constant != XR_SEMANTIC_INDEX_NONE ||
                candidate->effects != xi_generated_op_effects(XI_PARAM))
                return false;
        }
        if (parameter_operations != 1)
            return false;
    }
    return true;
}

static bool instruction_semantic_operand_value(const XrSemanticPlan *semantic,
                                               const XrSemanticOperationRecord *operation,
                                               uint16_t ordinal, uint32_t *out_value) {
    uint32_t count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &count);
    if (!operation || !operands || ordinal >= operation->operand_count ||
        operation->operand_begin > count || operation->operand_count > count - operation->operand_begin)
        return false;
    if (out_value)
        *out_value = operands[operation->operand_begin + ordinal].value;
    return true;
}

static const XrTargetSlotRecord *instruction_group_slot(const XrTargetPlan *plan,
                                                        uint32_t slot_index) {
    uint32_t count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &count);
    return slots && slot_index < count ? &slots[slot_index] : NULL;
}

/* Independent program-level proof for the two bounded leaf groups.  It joins
 * instruction slots back to frozen SemanticPlan result values and verifies the
 * structural get/set permutation.  Program/type/function bindings are the
 * authority; source names and legacy aggregate-shape helpers are absent. */
static bool leaf_aggregate_instruction_group_is_exact(const XrTargetPlan *plan,
                                                      const XrTargetInstructionRecord *rows,
                                                      uint32_t row_count, uint32_t function_index) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    const XrSemanticProgramFunctionBinding *binding =
        semantic ? xr_semantic_plan_program_function_for_semantic_function(semantic, function_index)
                 : NULL;
    const XrSemanticFunctionRecord *function =
        semantic ? xr_semantic_plan_function(semantic, function_index) : NULL;
    const XrSemanticBlockRecord *block =
        function && function->block_count == 1
            ? xr_semantic_plan_block(semantic, function->block_begin)
            : NULL;
    if (!instruction_semantic_is_leaf_program(semantic) || !binding || !function || !block ||
        row_count != 5 || block->function != function_index || block->kind != XI_BLOCK_RETURN ||
        block->successors[0] != XR_SEMANTIC_INDEX_NONE ||
        block->successors[1] != XR_SEMANTIC_INDEX_NONE)
        return false;

    const XrTargetSlotRecord *slot0 = instruction_group_slot(plan, rows[0].result_slot);
    const XrTargetSlotRecord *slot1 = instruction_group_slot(plan, rows[1].result_slot);
    const XrTargetSlotRecord *slot2 = instruction_group_slot(plan, rows[2].result_slot);
    const XrTargetSlotRecord *slot3 = instruction_group_slot(plan, rows[3].result_slot);
    if (!slot1 || !slot2 || !slot3 || rows[4].operand_slots[0] != rows[3].result_slot)
        return false;

    if (binding->flags == 0) {
        if (!slot0 || function->parameter_count != 1 || block->operation_count != 10 ||
            rows[0].opcode != XR_TARGET_INSTRUCTION_PARAM_AGGREGATE ||
            rows[1].opcode != XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64 ||
            rows[2].opcode != XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64 ||
            rows[3].opcode != XR_TARGET_INSTRUCTION_AGGREGATE_MAKE_I64X2 ||
            rows[4].opcode != XR_TARGET_INSTRUCTION_RETURN_AGGREGATE ||
            rows[1].operand_slots[0] != rows[0].result_slot ||
            rows[2].operand_slots[0] != rows[0].result_slot ||
            rows[3].operand_slots[0] != rows[1].result_slot ||
            rows[3].operand_slots[1] != rows[2].result_slot)
            return false;
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, function->parameter_begin);
        const XrSemanticOperationRecord *operation[10] = {0};
        for (uint32_t i = 0; i < 10; i++) {
            operation[i] = xr_semantic_plan_operation(semantic, block->operation_begin + i);
            if (!operation[i] || operation[i]->function != function_index ||
                operation[i]->block != function->block_begin)
                return false;
        }
        uint32_t load1_input = XR_SEMANTIC_INDEX_NONE;
        uint32_t get1_input = XR_SEMANTIC_INDEX_NONE;
        uint32_t load0_input = XR_SEMANTIC_INDEX_NONE;
        uint32_t get0_input = XR_SEMANTIC_INDEX_NONE;
        uint32_t retain_input = XR_SEMANTIC_INDEX_NONE;
        uint32_t aggregate_input = XR_SEMANTIC_INDEX_NONE;
        uint32_t set0_target = XR_SEMANTIC_INDEX_NONE;
        uint32_t set0_value = XR_SEMANTIC_INDEX_NONE;
        uint32_t set1_target = XR_SEMANTIC_INDEX_NONE;
        uint32_t set1_value = XR_SEMANTIC_INDEX_NONE;
        const XrTargetFieldRecord *get1 = NULL, *get0 = NULL;
        uint32_t field_count = 0;
        const XrTargetFieldRecord *fields = xr_target_plan_fields(plan, &field_count);
        if (!parameter || parameter->ordinal != 0 || parameter->function != function_index ||
            rows[0].immediate_bits != parameter->ordinal || operation[0]->opcode != XI_PARAM ||
            operation[0]->result_value != parameter->value ||
            operation[0]->result_type != parameter->type || operation[0]->operand_count != 0 ||
            operation[0]->semantic_immediate != parameter->ordinal ||
            operation[1]->opcode != XI_PLACE_LOAD || operation[2]->opcode != XI_AGG_GET ||
            operation[2]->semantic_immediate != 1 || operation[3]->opcode != XI_PLACE_LOAD ||
            operation[4]->opcode != XI_AGG_GET || operation[4]->semantic_immediate != 0 ||
            operation[5]->opcode != XI_GET_SHARED || operation[6]->opcode != XI_RETAIN ||
            operation[6]->result_type != operation[5]->result_type ||
            operation[7]->opcode != XI_AGG_NEW ||
            operation[8]->opcode != XI_AGG_SET || operation[8]->semantic_immediate != 0 ||
            operation[9]->opcode != XI_AGG_SET || operation[9]->semantic_immediate != 1 ||
            !instruction_semantic_operand_value(semantic, operation[1], 0, &load1_input) ||
            !instruction_semantic_operand_value(semantic, operation[2], 0, &get1_input) ||
            !instruction_semantic_operand_value(semantic, operation[3], 0, &load0_input) ||
            !instruction_semantic_operand_value(semantic, operation[4], 0, &get0_input) ||
            !instruction_semantic_operand_value(semantic, operation[6], 0, &retain_input) ||
            !instruction_semantic_operand_value(semantic, operation[7], 0, &aggregate_input) ||
            !instruction_semantic_operand_value(semantic, operation[8], 0, &set0_target) ||
            !instruction_semantic_operand_value(semantic, operation[8], 1, &set0_value) ||
            !instruction_semantic_operand_value(semantic, operation[9], 0, &set1_target) ||
            !instruction_semantic_operand_value(semantic, operation[9], 1, &set1_value) ||
            rows[1].immediate_bits >= field_count || rows[2].immediate_bits >= field_count)
            return false;
        get1 = &fields[(uint32_t) rows[1].immediate_bits];
        get0 = &fields[(uint32_t) rows[2].immediate_bits];
        return get1->semantic_field == 1 && get0->semantic_field == 0 &&
               slot0->semantic_value == parameter->value &&
               slot0->semantic_operation == XR_SEMANTIC_INDEX_NONE &&
               slot0->role == XR_TARGET_SLOT_PARAMETER && load1_input == parameter->value &&
               get1_input == operation[1]->result_value && load0_input == parameter->value &&
               get0_input == operation[3]->result_value &&
               retain_input == operation[5]->result_value &&
               aggregate_input == operation[5]->result_value &&
               slot1->semantic_value == operation[2]->result_value &&
               slot1->semantic_operation == block->operation_begin + 2u &&
               slot2->semantic_value == operation[4]->result_value &&
               slot2->semantic_operation == block->operation_begin + 4u &&
               slot3->semantic_value == operation[7]->result_value &&
               slot3->semantic_operation == block->operation_begin + 7u &&
               set0_target == slot3->semantic_value && set1_target == slot3->semantic_value &&
               set0_value == slot1->semantic_value && set1_value == slot2->semantic_value &&
               block->control_value == slot3->semantic_value;
    }

    if ((binding->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) == 0 ||
        function->parameter_count != 0 ||
        block->operation_count != 9 || rows[0].opcode != XR_TARGET_INSTRUCTION_CONST_I64 ||
        rows[1].opcode != XR_TARGET_INSTRUCTION_CONST_I64 ||
        rows[2].opcode != XR_TARGET_INSTRUCTION_AGGREGATE_MAKE_I64X2 ||
        rows[3].opcode != XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE ||
        rows[4].opcode != XR_TARGET_INSTRUCTION_RETURN_AGGREGATE ||
        rows[2].operand_slots[0] != rows[0].result_slot ||
        rows[2].operand_slots[1] != rows[1].result_slot)
        return false;
    const XrSemanticOperationRecord *operation[9] = {0};
    for (uint32_t i = 0; i < 9; i++) {
        operation[i] = xr_semantic_plan_operation(semantic, block->operation_begin + i);
        if (!operation[i] || operation[i]->function != function_index ||
            operation[i]->block != function->block_begin)
            return false;
    }
    uint32_t set0_target = XR_SEMANTIC_INDEX_NONE, set0_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t set1_target = XR_SEMANTIC_INDEX_NONE, set1_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t retain_input = XR_SEMANTIC_INDEX_NONE, aggregate_input = XR_SEMANTIC_INDEX_NONE;
    uint32_t call_argument_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t call_count = 0, argument_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetCallArgumentRecord *arguments =
        xr_target_plan_call_arguments(plan, &argument_count);
    const XrTargetCallRecord *call = calls && rows[3].immediate_bits < call_count
                                         ? &calls[(uint32_t) rows[3].immediate_bits]
                                         : NULL;
    const XrTargetCallArgumentRecord *argument =
        call && arguments && call->argument_count == 1 && call->argument_begin < argument_count
            ? &arguments[call->argument_begin]
            : NULL;
    const XrSemanticConstantRecord *constant0 =
        xr_semantic_plan_constant(semantic, operation[1]->constant);
    const XrSemanticConstantRecord *constant1 =
        xr_semantic_plan_constant(semantic, operation[2]->constant);
    return operation[0]->opcode == XI_GET_SHARED && operation[1]->opcode == XI_CONST &&
           operation[2]->opcode == XI_CONST && operation[3]->opcode == XI_GET_SHARED &&
           operation[4]->opcode == XI_RETAIN &&
           operation[4]->result_type == operation[3]->result_type &&
           operation[5]->opcode == XI_AGG_NEW && operation[6]->opcode == XI_AGG_SET &&
           operation[6]->semantic_immediate == 0 && operation[7]->opcode == XI_AGG_SET &&
           operation[7]->semantic_immediate == 1 && operation[8]->opcode == XI_CALL && call &&
           argument && constant0 && constant1 && constant0->kind == XR_SEM_CONST_INT &&
           constant1->kind == XR_SEM_CONST_INT && constant0->type == operation[1]->result_type &&
           constant1->type == operation[2]->result_type &&
           rows[0].immediate_bits == (uint64_t) constant0->integer &&
           rows[1].immediate_bits == (uint64_t) constant1->integer &&
           call->semantic_operation == block->operation_begin + 8u &&
           call->result_value == operation[8]->result_value &&
           call->result_slot == rows[3].result_slot &&
           instruction_semantic_operand_value(semantic, operation[4], 0, &retain_input) &&
           instruction_semantic_operand_value(semantic, operation[5], 0, &aggregate_input) &&
           instruction_semantic_operand_value(semantic, operation[6], 0, &set0_target) &&
           instruction_semantic_operand_value(semantic, operation[6], 1, &set0_value) &&
           instruction_semantic_operand_value(semantic, operation[7], 0, &set1_target) &&
           instruction_semantic_operand_value(semantic, operation[7], 1, &set1_value) &&
           instruction_semantic_operand_value(semantic, operation[8], 1, &call_argument_value) &&
           retain_input == operation[3]->result_value &&
           aggregate_input == operation[3]->result_value &&
           slot0->semantic_value == operation[1]->result_value &&
           slot0->semantic_operation == block->operation_begin + 1u &&
           slot1->semantic_value == operation[2]->result_value &&
           slot1->semantic_operation == block->operation_begin + 2u &&
           slot2->semantic_value == operation[5]->result_value &&
           slot2->semantic_operation == block->operation_begin + 5u &&
           set0_target == slot2->semantic_value && set1_target == slot2->semantic_value &&
           set0_value == slot0->semantic_value && set1_value == slot1->semantic_value &&
           argument->semantic_operand == operation[8]->operand_begin + 1u &&
           argument->semantic_value == call_argument_value && argument->caller_slot == slot2->id &&
           call_argument_value == slot2->semantic_value &&
           slot3->semantic_value == operation[8]->result_value &&
           slot3->semantic_operation == block->operation_begin + 8u &&
           block->control_value == slot3->semantic_value;
}

static bool product_instruction_group_is_exact(const XrTargetPlan *plan,
                                                const XrTargetInstructionRecord *rows,
                                                uint32_t row_count,
                                                uint32_t function_index) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    uint32_t callers[2] = {XR_SEMANTIC_INDEX_NONE, XR_SEMANTIC_INDEX_NONE};
    uint32_t callee = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticProgramFunctionBinding *binding =
        semantic ? xr_semantic_plan_program_function_for_semantic_function(semantic,
                                                                           function_index)
                 : NULL;
    const XrSemanticFunctionRecord *function =
        semantic ? xr_semantic_plan_function(semantic, function_index) : NULL;
    const XrSemanticBlockRecord *block =
        function && function->block_count == 1
            ? xr_semantic_plan_block(semantic, function->block_begin)
            : NULL;
    uint32_t target_function_count = 0;
    const XrTargetFunctionRecord *target_functions =
        xr_target_plan_functions(plan, &target_function_count);
    if (!instruction_product_required_functions(semantic, callers, &callee) || !binding ||
        !function || !block || !target_functions || function_index >= target_function_count ||
        block->function != function_index ||
        block->kind != XI_BLOCK_RETURN || function->parameter_count != 0)
        return false;
    const XrSemanticProgramTypeBinding *product_type = NULL;
    const XrSemanticProgramTypeBinding *i64_type = NULL;
    const XrSemanticProgramTypeBinding *u8_type = NULL;
    for (uint32_t row = 0; row < 3; row++) {
        const XrSemanticProgramTypeBinding *type =
            xr_semantic_plan_program_type_for_row(semantic, row);
        if (!type)
            return false;
        if (type->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT &&
            type->field_count == 6 && !product_type)
            product_type = type;
        else if (type->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                 type->exact_scalar == XR_EXACT_SCALAR_I64 && !i64_type)
            i64_type = type;
        else if (type->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                 type->exact_scalar == XR_EXACT_SCALAR_U8 && !u8_type)
            u8_type = type;
        else
            return false;
    }
    if (!product_type || !i64_type || !u8_type ||
        function->return_type != product_type->semantic_type)
        return false;
    bool is_callee = function_index == callee;
    uint32_t caller = function_index == callers[0] ? 0u
                      : function_index == callers[1] ? 1u
                                                     : UINT32_MAX;
    if ((!is_callee && caller >= 2) || row_count != (is_callee ? 14u : 15u))
        return false;

    const XrSemanticOperationRecord *construct = NULL;
    uint32_t construct_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *call_operation = NULL;
    uint32_t call_operation_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *projects[6] = {0};
    uint32_t project_indices[6] = {0};
    const XrSemanticOperationRecord *constants[6] = {0};
    uint32_t constant_indices[6] = {0};
    const XrSemanticOperationRecord *u8_conversion = NULL;
    uint32_t u8_conversion_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *callable = NULL;
    uint32_t construct_inputs[6] = {0};
    uint32_t construct_count = 0, project_count = 0, constant_count = 0, call_count = 0;
    uint32_t conversion_count = 0;
    uint32_t callable_count = 0;
    for (uint32_t i = 0; i < block->operation_count; i++) {
        uint32_t index = block->operation_begin + i;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, index);
        if (!operation || operation->function != function_index ||
            operation->block != function->block_begin)
            return false;
        if (operation->opcode == XI_VALUE_PRODUCT_CONSTRUCT) {
            construct = operation;
            construct_index = index;
            construct_count++;
        } else if (operation->opcode == XI_VALUE_PRODUCT_PROJECT) {
            if (operation->semantic_immediate < 0 || operation->semantic_immediate >= 6 ||
                projects[(uint32_t) operation->semantic_immediate])
                return false;
            projects[(uint32_t) operation->semantic_immediate] = operation;
            project_indices[(uint32_t) operation->semantic_immediate] = index;
            project_count++;
        } else if (operation->opcode == XI_CONST) {
            if (constant_count >= 6)
                return false;
            constants[constant_count] = operation;
            constant_indices[constant_count++] = index;
        } else if (operation->opcode == XI_NARROW_U8) {
            u8_conversion = operation;
            u8_conversion_index = index;
            conversion_count++;
        } else if (operation->opcode == XI_GET_SHARED) {
            callable = operation;
            callable_count++;
        } else if (operation->opcode == XI_CALL) {
            call_operation = operation;
            call_operation_index = index;
            call_count++;
        } else {
            return false;
        }
    }
    if (construct_count != 1 || !construct || construct->operand_count != 6 ||
        construct->result_type != product_type->semantic_type ||
        construct->result_value == XR_SEMANTIC_INDEX_NONE ||
        block->control_value != construct->result_value ||
        (is_callee ? (constant_count != 6 || conversion_count != 1 || callable_count != 0 ||
                      project_count != 0 || call_count != 0)
                   : (constant_count != 0 || conversion_count != 0 || callable_count != 1 ||
                      project_count != 6 || call_count != 1)))
        return false;
    for (uint32_t ordinal = 0; ordinal < 6; ordinal++)
        if (!instruction_semantic_operand_value(semantic, construct, ordinal,
                                                &construct_inputs[ordinal]))
            return false;

    uint32_t layout = XR_SEMANTIC_INDEX_NONE;
    uint32_t init_row = is_callee ? 6u : 7u;
    const XrTargetSlotRecord *construct_slot =
        instruction_group_slot(plan, rows[init_row].result_slot);
    if (!construct_slot ||
        !product_slot_layout(plan,
                             &target_functions[function_index], function_index,
                             rows[init_row].result_slot, &layout) ||
        rows[init_row].opcode != XR_TARGET_INSTRUCTION_VALUE_PRODUCT_INIT ||
        rows[init_row].immediate_bits != layout ||
        construct_slot->semantic_value != construct->result_value ||
        construct_slot->semantic_operation != construct_index)
        return false;
    uint32_t layout_count = 0, field_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(plan, &field_count);
    if (!layouts || !fields || layout >= layout_count ||
        !product_layout_is_exact(plan, layout))
        return false;
    const XrTargetLayoutRecord *product_layout = &layouts[layout];

    uint32_t scalar_rows = is_callee ? 0u : 1u;
    if (!is_callee) {
        const XrSemanticProgramCallBinding *semantic_call =
            xr_semantic_plan_program_call_for_operation(semantic, call_operation_index);
        uint32_t target_call_count = 0;
        const XrTargetCallRecord *target_calls =
            xr_target_plan_calls(plan, &target_call_count);
        const XrTargetCallRecord *target_call =
            target_calls && rows[0].immediate_bits < target_call_count
                ? &target_calls[(uint32_t) rows[0].immediate_bits]
                : NULL;
        const XrTargetSlotRecord *call_slot = instruction_group_slot(plan, rows[0].result_slot);
        uint32_t callable_value = XR_SEMANTIC_INDEX_NONE;
        bool call_operand_exact =
            call_operation && call_operation->operand_count == 1 && callable &&
            instruction_semantic_operand_value(semantic, call_operation, 0, &callable_value) &&
            callable_value == callable->result_value;
        if (!semantic_call || !call_operation || !target_call || !call_slot ||
            !call_operand_exact ||
            rows[0].opcode != XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE ||
            call_operation->result_type != product_type->semantic_type ||
            semantic_call->operation != call_operation_index ||
            semantic_call->target_function != callee ||
            target_call->semantic_operation != call_operation_index ||
            target_call->caller_function != function_index ||
            target_call->callee_function != callee ||
            target_call->result_slot != rows[0].result_slot ||
            call_slot->semantic_value != call_operation->result_value ||
            call_slot->semantic_operation != call_operation_index)
            return false;
        for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
            uint32_t project_input = XR_SEMANTIC_INDEX_NONE;
            const XrTargetSlotRecord *result =
                instruction_group_slot(plan, rows[1u + ordinal].result_slot);
            const XrTargetFieldRecord *field =
                &fields[product_layout->field_begin + ordinal];
            uint16_t opcode = ordinal == 2
                                  ? XR_TARGET_INSTRUCTION_VALUE_PRODUCT_GET_U8
                                  : XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64;
            if (!projects[ordinal] || !result ||
                !instruction_semantic_operand_value(semantic, projects[ordinal], 0,
                                                    &project_input) ||
                project_input != call_operation->result_value ||
                rows[1u + ordinal].opcode != opcode ||
                rows[1u + ordinal].operand_slots[0] != rows[0].result_slot ||
                rows[1u + ordinal].immediate_bits != product_layout->field_begin + ordinal ||
                result->semantic_value != projects[ordinal]->result_value ||
                result->semantic_operation != project_indices[ordinal] ||
                projects[ordinal]->result_type !=
                    (ordinal == 2 ? u8_type->semantic_type : i64_type->semantic_type) ||
                result->register_rep != field->memory_rep ||
                result->memory_rep != field->memory_rep ||
                construct_inputs[ordinal] != projects[ordinal]->result_value)
                return false;
        }
    } else {
        bool used[6] = {false, false, false, false, false, false};
        uint32_t conversion_input = XR_SEMANTIC_INDEX_NONE;
        if (!u8_conversion || u8_conversion->result_type != u8_type->semantic_type ||
            u8_conversion->operand_count != 1 || u8_conversion->semantic_immediate != 0 ||
            !instruction_semantic_operand_value(semantic, u8_conversion, 0,
                                                &conversion_input) ||
            construct_inputs[2] != u8_conversion->result_value)
            return false;
        for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
            const XrSemanticOperationRecord *constant = NULL;
            uint32_t constant_index = XR_SEMANTIC_INDEX_NONE;
            uint32_t expected_input = ordinal == 2 ? conversion_input : construct_inputs[ordinal];
            for (uint32_t i = 0; i < 6; i++)
                if (constants[i]->result_value == expected_input) {
                    if (constant)
                        return false;
                    constant = constants[i];
                    constant_index = constant_indices[i];
                    if (used[i])
                        return false;
                    used[i] = true;
                }
            const XrSemanticConstantRecord *literal =
                constant ? xr_semantic_plan_constant(semantic, constant->constant) : NULL;
            const XrTargetSlotRecord *result =
                instruction_group_slot(plan, rows[ordinal].result_slot);
            const XrSemanticOperationRecord *value_operation =
                ordinal == 2 ? u8_conversion : constant;
            uint32_t value_operation_index =
                ordinal == 2 ? u8_conversion_index : constant_index;
            uint16_t opcode = ordinal == 2 ? XR_TARGET_INSTRUCTION_CONST_U8
                                           : XR_TARGET_INSTRUCTION_CONST_I64;
            if (!constant || !literal || !result || literal->kind != XR_SEM_CONST_INT ||
                constant->result_type != i64_type->semantic_type ||
                literal->type != constant->result_type ||
                value_operation->result_type !=
                    (ordinal == 2 ? u8_type->semantic_type : i64_type->semantic_type) ||
                (ordinal == 2 && (literal->integer < 0 || literal->integer > UINT8_MAX)) ||
                rows[ordinal].opcode != opcode ||
                rows[ordinal].immediate_bits != (uint64_t) literal->integer ||
                result->semantic_value != value_operation->result_value ||
                result->semantic_operation != value_operation_index)
                return false;
        }
    }
    for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
        uint32_t set_row = init_row + 1u + ordinal;
        uint32_t value_row = scalar_rows + ordinal;
        uint16_t opcode = ordinal == 2 ? XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_U8
                                       : XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_I64;
        if (rows[set_row].opcode != opcode ||
            rows[set_row].result_slot != XR_TARGET_INSTRUCTION_SLOT_NONE ||
            rows[set_row].operand_slots[0] != rows[init_row].result_slot ||
            rows[set_row].operand_slots[1] != rows[value_row].result_slot ||
            rows[set_row].immediate_bits != product_layout->field_begin + ordinal)
            return false;
    }
    uint32_t return_row = row_count - 1u;
    return rows[return_row].opcode == XR_TARGET_INSTRUCTION_RETURN_AGGREGATE &&
           rows[return_row].operand_slots[0] == rows[init_row].result_slot;
}

static bool verify_function_group(const XrTargetPlan *plan, const XrTargetInstructionRecord *rows,
                                  uint32_t row_count, uint32_t function_index,
                                  const uint8_t *executable_functions, uint32_t function_count,
                                  char *error, size_t error_size) {
    uint32_t plan_function_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(plan, &plan_function_count);
    if (!functions || plan_function_count != function_count ||
        function_index >= function_count || !row_count)
        return report(error, error_size, "XR_TARGET_1005",
                      "instruction function identity is invalid");
    const XrTargetFunctionRecord *function = &functions[function_index];
    if (function->id != function_index || !function->slot_count)
        return report(error, error_size, "XR_TARGET_1005",
                      "instruction function has no exact slot range");
    uint8_t *defined = (uint8_t *) xr_calloc(function->slot_count, 1);
    if (!defined)
        return report(error, error_size, "XR_EXEC_5003",
                      "instruction verifier budget exhausted");

    uint64_t bound_arguments = 0;
    uint32_t parameter_rows = 0;
    bool valid = true;
    bool call_invalid = false;
    bool suspend_invalid = false;
    bool tagged_push = false;
    bool leaf_aggregate = false;
    for (uint32_t i = 0; i < row_count && valid; i++) {
        const XrTargetInstructionRecord *row = &rows[i];
        bool terminal = i + 1u == row_count;
        const XrTargetInstructionContract *contract =
            xr_target_instruction_contract(row->opcode);
        if (row->function != function_index || row->reserved != 0 || !contract ||
            row->operand_count != contract->arity ||
            (!contract->terminator && terminal) ||
            !row_immediate_is_exact(row, contract)) {
            valid = false;
            break;
        }
        for (uint32_t operand = 0; operand < 2u && valid; operand++) {
            if (operand < contract->arity)
                valid = slot_is_contract_rep(
                    plan, function, function_index, row->operand_slots[operand],
                    contract->operand_rep[operand], contract->operand_ownership[operand], false,
                    NULL);
            else
                valid = row->operand_slots[operand] ==
                        XR_TARGET_INSTRUCTION_SLOT_NONE;
        }
        if (!valid)
            break;

        if (contract->dispatch_kind == XR_TARGET_INSTRUCTION_DISPATCH_CALL &&
            !direct_i64_call_row_is_exact(plan, row, function_index,
                                          executable_functions,
                                          function_count)) {
            valid = false;
            call_invalid = true;
            break;
        }
        if (contract->dispatch_kind ==
                XR_TARGET_INSTRUCTION_DISPATCH_CALL_AGGREGATE &&
            !(instruction_semantic_is_product_program(
                  xr_target_plan_semantic_plan(plan))
                  ? direct_product_call_row_is_exact(
                        plan, row, function_index, executable_functions, function_count)
                  : direct_leaf_aggregate_call_row_is_exact(
                        plan, row, function_index, executable_functions, function_count))) {
            valid = false;
            call_invalid = true;
            break;
        }
        if (contract->dispatch_kind ==
                XR_TARGET_INSTRUCTION_DISPATCH_ENTRY_CALL &&
            !entry_i64_call_row_is_exact(plan, row, function_index)) {
            valid = false;
            call_invalid = true;
            break;
        }
        if (contract->dispatch_kind == XR_TARGET_INSTRUCTION_DISPATCH_NATIVE_LEAF &&
            !native_target_leaf_i64_call_row_is_exact(plan, row, function_index)) {
            valid = false;
            call_invalid = true;
            break;
        }
        if (contract->dispatch_kind ==
                XR_TARGET_INSTRUCTION_DISPATCH_SUSPEND &&
            !suspend_row_is_exact(plan, row, function_index)) {
            valid = false;
            suspend_invalid = true;
            break;
        }
        if (contract->dispatch_kind == XR_TARGET_INSTRUCTION_DISPATCH_OVERFLOW) {
            uint32_t predicate_count = 0;
            const XrTargetI64OverflowPredicateRecord *predicates =
                xr_target_plan_i64_overflow_predicates(plan, &predicate_count);
            uint32_t predicate_index = (uint32_t) row->immediate_bits;
            const XrTargetI64OverflowPredicateRecord *predicate =
                predicates && predicate_index < predicate_count
                    ? &predicates[predicate_index]
                    : NULL;
            if (!predicate || predicate->id != predicate_index ||
                predicate->function != function_index ||
                predicate->result_slot != row->result_slot ||
                predicate->receiver_slot != row->operand_slots[0] ||
                predicate->argument_slot != row->operand_slots[1]) {
                valid = false;
                break;
            }
        }
        tagged_push |= contract->dispatch_kind ==
                       XR_TARGET_INSTRUCTION_DISPATCH_ARRAY_PUSH;
        leaf_aggregate |= contract->result_rep == XR_TARGET_INSTRUCTION_REP_AGGREGATE ||
                          contract->operand_rep[0] == XR_TARGET_INSTRUCTION_REP_AGGREGATE ||
                          contract->operand_rep[1] == XR_TARGET_INSTRUCTION_REP_AGGREGATE;

        if (contract->terminator || contract->result_rep == XR_TARGET_INSTRUCTION_REP_NONE) {
            valid = row->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE;
        } else {
            const XrTargetSlotRecord *result = NULL;
            valid = slot_is_contract_rep(
                plan, function, function_index, row->result_slot,
                contract->result_rep, contract->result_ownership, true, &result);
            bool parameter = contract->immediate_kind ==
                             XR_TARGET_INSTRUCTION_IMMEDIATE_PARAMETER_ORDINAL;
            if (valid && parameter) {
                valid = (bound_arguments &
                         (UINT64_C(1) << row->immediate_bits)) == 0 &&
                        result->role == XR_TARGET_SLOT_PARAMETER;
                if (valid) {
                    bound_arguments |= UINT64_C(1) << row->immediate_bits;
                    parameter_rows++;
                }
            } else if (valid && result->role == XR_TARGET_SLOT_PARAMETER) {
                valid = false;
            }
        }
        if (!valid || contract->terminator ||
            contract->result_rep == XR_TARGET_INSTRUCTION_REP_NONE)
            continue;
        if (!leaf_aggregate_data_row_is_exact(plan, function, function_index, row, contract)) {
            valid = false;
            break;
        }
        uint32_t local = row->result_slot - function->slot_begin;
        if (defined[local]) {
            valid = false;
            break;
        }
        defined[local] = 1;
    }
    xr_free(defined);
    /* Argument ordinals must be exactly 0..parameter_rows-1 and must cover
     * every parameter slot the function frame declares, so the executor can
     * read the incoming argument count straight off the verified rows. */
    uint64_t dense_arguments =
        parameter_rows == XR_TARGET_INSTRUCTION_MAX_PARAMETERS
            ? UINT64_MAX
            : (UINT64_C(1) << parameter_rows) - 1u;
    /* Row shapes alone say nothing about which rows can run before which. The
     * block partition, the legality of every jump target, the reachability of
     * every block, and definite assignment on every path are all proved by the
     * one control-flow judgement the production builder admits against. */
    uint32_t call_count = 0;
    uint32_t call_argument_count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(plan, &call_count);
    const XrTargetCallArgumentRecord *call_arguments =
        xr_target_plan_call_arguments(plan, &call_argument_count);
    uint32_t entry_expectation_count = 0;
    const XrTargetEntryExpectationRecord *entry_expectations =
        xr_target_plan_entry_expectations(plan, &entry_expectation_count);
    uint32_t coroutine_count = 0;
    const XrTargetCoroutineStateRecord *coroutines =
        xr_target_plan_coroutines(plan, &coroutine_count);
    if (call_invalid)
        return report(error, error_size, "XR_TARGET_1003",
                      "instruction call row does not match its exact call record");
    if (suspend_invalid)
        return report(error, error_size, "XR_CORO_4000",
                      "instruction suspend row does not match its exact coroutine state");
    if (tagged_push &&
        !tagged_array_push_group_is_exact(plan, rows, row_count, function_index))
        return report(error, error_size, "XR_TARGET_1003",
                      "managed Array.push row does not match its exact tagged call site");
    bool product_program = instruction_semantic_is_product_program(
        xr_target_plan_semantic_plan(plan));
    if (leaf_aggregate && !product_program &&
        !instruction_semantic_is_leaf_program(xr_target_plan_semantic_plan(plan)))
        return report(error, error_size, "XR_TARGET_1003",
                      "leaf aggregate instruction row lacks exact program authority");
    if (leaf_aggregate &&
        !(product_program
              ? product_instruction_group_is_exact(plan, rows, row_count, function_index)
              : leaf_aggregate_instruction_group_is_exact(plan, rows, row_count,
                                                          function_index)))
        return report(error, error_size, "XR_TARGET_1003",
                      "leaf aggregate instruction group does not match its typed program");
    if (!valid || bound_arguments != dense_arguments ||
        function_parameter_slot_count(plan, function) != parameter_rows ||
        !xr_target_instruction_rows_control_flow_is_exact(
            rows, row_count, function->slot_begin, function->slot_count,
            calls, call_count, call_arguments, call_argument_count,
            entry_expectations, entry_expectation_count, coroutines,
            coroutine_count))
        return report(error, error_size, "XR_TARGET_1005",
                      "instruction program is not an exact typed executable program");
    return true;
}

bool xr_target_instruction_program_verify(const XrTargetPlan *plan,
                                          char *error, size_t error_size) {
    if (!plan || !xr_target_plan_is_frozen(plan))
        return report(error, error_size, "XR_EXEC_5000",
                      "instruction verifier requires a frozen TargetPlan");
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(plan, &instruction_count);
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    const XrSemanticProgramProvenance *published =
        semantic ? xr_semantic_plan_program_provenance(semantic) : NULL;
    bool requires_leaf_aggregate =
        published && published->program_family ==
                         XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL;
    bool requires_leaf_product =
        published && published->program_family ==
                         XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL;
    bool requires_overflow =
        published && published->program_family ==
                         XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE;
    uint32_t leaf_caller = XR_SEMANTIC_INDEX_NONE;
    uint32_t leaf_callee = XR_SEMANTIC_INDEX_NONE;
    if (requires_leaf_aggregate &&
        !instruction_leaf_required_functions(semantic, &leaf_caller, &leaf_callee))
        return report(error, error_size, "XR_TARGET_1003",
                      "leaf aggregate instruction requirements lack exact program bindings");
    uint32_t product_callers[2] = {XR_SEMANTIC_INDEX_NONE, XR_SEMANTIC_INDEX_NONE};
    uint32_t product_callee = XR_SEMANTIC_INDEX_NONE;
    if (requires_leaf_product &&
        !instruction_product_required_functions(semantic, product_callers, &product_callee))
        return report(error, error_size, "XR_TARGET_1003",
                      "leaf product instruction requirements lack exact program bindings");
    if (!instruction_count)
        return (!requires_leaf_aggregate && !requires_leaf_product && !requires_overflow) ||
               report(error, error_size, "XR_TARGET_1003",
                      "required program has no typed instruction rows");
    if (!instructions)
        return report(error, error_size, "XR_EXEC_5003",
                      "instruction table storage is missing");

    uint32_t function_count = 0;
    xr_target_plan_functions(plan, &function_count);
    uint8_t *executable = (uint8_t *) xr_calloc(function_count, 1);
    if (function_count && !executable)
        return report(error, error_size, "XR_EXEC_5003",
                      "instruction verifier budget exhausted");
    uint32_t begin = 0;
    while (begin < instruction_count) {
        uint32_t function = instructions[begin].function;
        uint32_t end = begin + 1u;
        if (instructions[begin].id != begin ||
            (begin && instructions[begin - 1u].function >= function))
            goto invalid_order;
        while (end < instruction_count && instructions[end].function == function) {
            if (instructions[end].id != end)
                goto invalid_order;
            end++;
        }
        if (function >= function_count || executable[function])
            goto invalid_order;
        executable[function] = 1;
        begin = end;
    }
    if (requires_leaf_aggregate) {
        if (leaf_caller >= function_count || leaf_callee >= function_count ||
            !executable[leaf_caller] || !executable[leaf_callee]) {
            xr_free(executable);
            return report(error, error_size, "XR_TARGET_1003",
                          "leaf aggregate caller or callee instruction group is missing");
        }
        uint32_t required[2] = {leaf_caller, leaf_callee};
        for (uint32_t r = 0; r < 2; r++) {
            uint32_t group_begin = 0;
            while (group_begin < instruction_count &&
                   instructions[group_begin].function < required[r])
                group_begin++;
            uint32_t group_end = group_begin;
            while (group_end < instruction_count &&
                   instructions[group_end].function == required[r])
                group_end++;
            if (group_begin == group_end ||
                !leaf_aggregate_instruction_group_is_exact(
                    plan, &instructions[group_begin], group_end - group_begin, required[r])) {
                xr_free(executable);
                return report(error, error_size, "XR_TARGET_1003",
                              "leaf aggregate required instruction group is inexact");
            }
        }
    }
    if (requires_leaf_product) {
        uint32_t required[3] = {product_callers[0], product_callers[1], product_callee};
        for (uint32_t r = 0; r < 3; r++) {
            if (required[r] >= function_count || !executable[required[r]]) {
                xr_free(executable);
                return report(error, error_size, "XR_TARGET_1003",
                              "leaf product caller or callee instruction group is missing");
            }
            uint32_t group_begin = 0;
            while (group_begin < instruction_count &&
                   instructions[group_begin].function < required[r])
                group_begin++;
            uint32_t group_end = group_begin;
            while (group_end < instruction_count &&
                   instructions[group_end].function == required[r])
                group_end++;
            if (group_begin == group_end ||
                !product_instruction_group_is_exact(
                    plan, &instructions[group_begin], group_end - group_begin, required[r])) {
                xr_free(executable);
                return report(error, error_size, "XR_TARGET_1003",
                              "leaf product required instruction group is inexact");
            }
        }
    }
    begin = 0;
    while (begin < instruction_count) {
        uint32_t function = instructions[begin].function;
        uint32_t end = begin + 1u;
        while (end < instruction_count && instructions[end].function == function)
            end++;
        if (!verify_function_group(plan, &instructions[begin], end - begin,
                                   function, executable, function_count, error,
                                   error_size)) {
            xr_free(executable);
            return false;
        }
        begin = end;
    }
    xr_free(executable);
    return true;

invalid_order:
    xr_free(executable);
    return report(error, error_size, "XR_TARGET_1005",
                  "instruction table order is not canonical");
}
