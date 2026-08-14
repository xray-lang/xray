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
#include "../../base/xmalloc.h"
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
    SLOT_FAMILY_I64 = 0,
    SLOT_FAMILY_BOOL,
} SlotFamily;

static bool rep_is_trivial(const XrTargetMachineRepRecord *rep,
                           SlotFamily family) {
    if (!rep || rep->root_kind != XR_TARGET_ROOT_NONE ||
        rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;
    if (family == SLOT_FAMILY_BOOL)
        return rep->kind == XR_MACHINE_REP_I1 && rep->register_bits == 1 &&
               rep->memory_size == 1 && rep->memory_align == 1 &&
               rep->signedness == XR_TARGET_SIGN_NONE;
    return rep->kind == XR_MACHINE_REP_I64 && rep->register_bits == 64 &&
           rep->memory_size == 8 && rep->memory_align == 8 &&
           rep->signedness == XR_TARGET_SIGN_SIGNED;
}

static bool slot_is_family(const XrTargetPlan *plan,
                           const XrTargetFunctionRecord *function,
                           uint32_t function_index, uint32_t slot_index,
                           SlotFamily family,
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
    uint32_t width = family == SLOT_FAMILY_BOOL ? 1u : 8u;
    if (slot->id != slot_index || slot->function != function_index ||
        slot->size != width || slot->align != width ||
        slot->root_kind != XR_TARGET_ROOT_NONE ||
        slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        !rep_is_trivial(register_rep, family) ||
        !rep_is_trivial(memory_rep, family))
        return false;
    if (out)
        *out = slot;
    return true;
}

static bool slot_is_i64(const XrTargetPlan *plan,
                        const XrTargetFunctionRecord *function,
                        uint32_t function_index, uint32_t slot_index,
                        const XrTargetSlotRecord **out) {
    return slot_is_family(plan, function, function_index, slot_index,
                          SLOT_FAMILY_I64, out);
}

/* Row shape only. That an operand is already defined where it is read is a
 * whole-group question once a function has several blocks, so it is proved
 * separately by the control-flow fixed point rather than by the order this
 * pass happens to walk the table in. */
static bool operand_is_function_i64(const XrTargetPlan *plan,
                                    const XrTargetFunctionRecord *function,
                                    uint32_t function_index, uint32_t slot) {
    return slot_is_i64(plan, function, function_index, slot, NULL);
}

static bool operand_is_function_bool(const XrTargetPlan *plan,
                                     const XrTargetFunctionRecord *function,
                                     uint32_t function_index, uint32_t slot) {
    return slot_is_family(plan, function, function_index, slot,
                          SLOT_FAMILY_BOOL, NULL);
}

/* Which family a row's result slot must belong to. A comparison writes the
 * truth family and every other computation writes the signed i64 one, so the
 * opcode alone decides it and a row can never be admitted against the family
 * its own result happens to have. */
static SlotFamily row_result_family(uint8_t opcode) {
    return XR_TARGET_INSTRUCTION_IS_COMPARE(opcode) ? SLOT_FAMILY_BOOL
                                                    : SLOT_FAMILY_I64;
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

/* Shared shape of every two-operand computation row: canonical arity, an
 * unused immediate, and two operand slots this function owns. A computation
 * never ends a block, so it may not be the group's last row. */
static bool binary_row_shape_is_exact(const XrTargetPlan *plan,
                                      const XrTargetFunctionRecord *function,
                                      uint32_t function_index,
                                      const XrTargetInstructionRecord *row,
                                      bool terminal) {
    return row->operand_count == 2 && row->immediate_bits == 0 &&
           operand_is_function_i64(plan, function, function_index,
                                   row->operand_slots[0]) &&
           operand_is_function_i64(plan, function, function_index,
                                   row->operand_slots[1]) &&
           !terminal;
}

static bool slot_role_is(const XrTargetPlan *plan,
                         const XrTargetFunctionRecord *function,
                         uint32_t function_index, uint32_t slot_index,
                         uint8_t role) {
    const XrTargetSlotRecord *slot = NULL;
    return slot_is_i64(plan, function, function_index, slot_index, &slot) &&
           slot->role == role;
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
    uint32_t slot_begin, uint32_t slot_count) {
    if (!rows || !row_count || !slot_count ||
        slot_count > UINT32_MAX - slot_begin ||
        !XR_TARGET_INSTRUCTION_IS_TERMINATOR(rows[row_count - 1u].opcode))
        return false;
    /* The block partition is derived, not declared: a block begins at the first
     * row and after every terminator, so every block ends in a terminator by
     * construction and the last row cannot fall off the end of the table. */
    uint32_t block_count = 1;
    for (uint32_t i = 0; i + 1u < row_count; i++) {
        if (!XR_TARGET_INSTRUCTION_IS_TERMINATOR(rows[i].opcode))
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
        if (XR_TARGET_INSTRUCTION_IS_TERMINATOR(rows[i].opcode))
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
        uint64_t immediate = terminator->immediate_bits;
        proof.successors[block * 2u] = UINT32_MAX;
        proof.successors[block * 2u + 1u] = UINT32_MAX;
        switch ((XrTargetInstructionOpcode) terminator->opcode) {
            case XR_TARGET_INSTRUCTION_RETURN_I64:
                break;
            case XR_TARGET_INSTRUCTION_JUMP:
                valid = XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(immediate) == 0 &&
                        control_flow_target_block(
                            proof.block_start, block_count,
                            XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(immediate),
                            &proof.successors[block * 2u]);
                break;
            /* Both branch rows carry the same pair of explicit edges; only the
             * width of the condition they read differs, and that is a row-shape
             * question rather than a control-flow one. */
            case XR_TARGET_INSTRUCTION_BRANCH_IF_NONZERO_I64:
            case XR_TARGET_INSTRUCTION_BRANCH_IF_TRUE_BOOL:
                valid = control_flow_target_block(
                            proof.block_start, block_count,
                            XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(immediate),
                            &proof.successors[block * 2u]) &&
                        control_flow_target_block(
                            proof.block_start, block_count,
                            XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(immediate),
                            &proof.successors[block * 2u + 1u]);
                break;
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

static bool verify_function_group(const XrTargetPlan *plan,
                                  const XrTargetInstructionRecord *rows,
                                  uint32_t row_count, uint32_t function_index,
                                  char *error, size_t error_size) {
    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(plan, &function_count);
    if (!functions || function_index >= function_count || !row_count)
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
    for (uint32_t i = 0; i < row_count && valid; i++) {
        const XrTargetInstructionRecord *row = &rows[i];
        bool terminal = i + 1u == row_count;
        if (row->function != function_index || row->reserved != 0 ||
            row->opcode <= XR_TARGET_INSTRUCTION_INVALID ||
            row->opcode >= XR_TARGET_INSTRUCTION_COUNT) {
            valid = false;
            break;
        }
        switch ((XrTargetInstructionOpcode) row->opcode) {
            case XR_TARGET_INSTRUCTION_CONST_I64:
                valid = row->operand_count == 0 &&
                        row->operand_slots[0] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        row->operand_slots[1] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        !terminal;
                break;
            case XR_TARGET_INSTRUCTION_PARAM_I64:
                valid = row->operand_count == 0 &&
                        row->operand_slots[0] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        row->operand_slots[1] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        row->immediate_bits < XR_TARGET_INSTRUCTION_MAX_PARAMETERS &&
                        (bound_arguments & (UINT64_C(1) << row->immediate_bits)) == 0 &&
                        slot_role_is(plan, function, function_index,
                                     row->result_slot, XR_TARGET_SLOT_PARAMETER) &&
                        !terminal;
                if (valid) {
                    bound_arguments |= UINT64_C(1) << row->immediate_bits;
                    parameter_rows++;
                }
                break;
            case XR_TARGET_INSTRUCTION_COPY_I64:
            case XR_TARGET_INSTRUCTION_NEG_WRAP_I64:
            case XR_TARGET_INSTRUCTION_BNOT_I64:
                valid = row->operand_count == 1 && row->immediate_bits == 0 &&
                        row->operand_slots[1] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        operand_is_function_i64(plan, function, function_index,
                                                row->operand_slots[0]) &&
                        !terminal;
                break;
            case XR_TARGET_INSTRUCTION_ADD_WRAP_I64:
            case XR_TARGET_INSTRUCTION_SUB_WRAP_I64:
            case XR_TARGET_INSTRUCTION_MUL_WRAP_I64:
            case XR_TARGET_INSTRUCTION_BAND_I64:
            case XR_TARGET_INSTRUCTION_BOR_I64:
            case XR_TARGET_INSTRUCTION_BXOR_I64:
                valid = binary_row_shape_is_exact(plan, function, function_index,
                                                  row, terminal);
                break;
            case XR_TARGET_INSTRUCTION_CMP_EQ_I64:
            case XR_TARGET_INSTRUCTION_CMP_NE_I64:
            case XR_TARGET_INSTRUCTION_CMP_LT_I64:
            case XR_TARGET_INSTRUCTION_CMP_LE_I64:
            case XR_TARGET_INSTRUCTION_CMP_GT_I64:
            case XR_TARGET_INSTRUCTION_CMP_GE_I64:
                /* A relation reads the same exact signed i64 pair every other
                 * two-operand row reads; only what it writes differs, and that
                 * is proved by the result-family rule below rather than here,
                 * so the operand shape stays one statement for the whole
                 * family. Rejecting a non-zero immediate means there is no
                 * immediate comparison form, so a relation always answers about
                 * two slots the def-use proof has seen. */
                valid = binary_row_shape_is_exact(plan, function, function_index,
                                                  row, terminal);
                break;
            case XR_TARGET_INSTRUCTION_SHL_MASKED_I64:
            case XR_TARGET_INSTRUCTION_SHR_ARITH_MASKED_I64:
                /* The count is the second operand and nothing else: rejecting a
                 * non-zero immediate means there is no immediate shift form, so
                 * every count is a defined i64 slot the executor masks modulo
                 * 64 on the way in. A defined i64 slot therefore needs no
                 * further static range proof, because the language leaves no
                 * i64 count undefined. */
                valid = binary_row_shape_is_exact(plan, function, function_index,
                                                  row, terminal);
                break;
            case XR_TARGET_INSTRUCTION_DIV_TRAP_I64:
            case XR_TARGET_INSTRUCTION_MOD_TRAP_I64:
                /* The divisor is a runtime slot value, so no static proof here
                 * can exclude a zero: the row shape is what is proved, and the
                 * executor owns the error edge. Rejecting a non-zero immediate
                 * keeps that edge reachable, since there is no immediate
                 * divisor form that could carry a zero the executor never
                 * inspects. */
                valid = binary_row_shape_is_exact(plan, function, function_index,
                                                  row, terminal);
                break;
            case XR_TARGET_INSTRUCTION_RETURN_I64:
                /* A return ends its block wherever it stands. Several blocks
                 * may each end in one, so the group's last row being a
                 * terminator is what closes the program, not this row being
                 * unique. */
                valid = row->operand_count == 1 && row->immediate_bits == 0 &&
                        row->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        row->operand_slots[1] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        operand_is_function_i64(plan, function, function_index,
                                                row->operand_slots[0]);
                break;
            case XR_TARGET_INSTRUCTION_JUMP:
                /* The whole immediate is the target: the unused half must be
                 * zero so that no second edge can hide in a jump. Whether the
                 * target names a block entry of this group is proved by the
                 * control-flow pass, which owns the block partition. */
                valid = row->operand_count == 0 &&
                        row->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        row->operand_slots[0] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        row->operand_slots[1] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(row->immediate_bits) == 0;
                break;
            case XR_TARGET_INSTRUCTION_BRANCH_IF_NONZERO_I64:
                /* The condition is an ordinary i64 slot the executor compares
                 * against zero, so it carries no truth representation of its
                 * own and needs no proof beyond the slot proof every other
                 * operand gets. Both edges are explicit, so neither depends on
                 * the branch's position in the table. */
                valid = row->operand_count == 1 &&
                        row->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        row->operand_slots[1] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        operand_is_function_i64(plan, function, function_index,
                                                row->operand_slots[0]);
                break;
            case XR_TARGET_INSTRUCTION_BRANCH_IF_TRUE_BOOL:
                /* The same edge rule over a truth slot. The condition family is
                 * fixed by the opcode rather than read off the slot while the
                 * row runs, so the executor never has to decide how wide its
                 * own condition is. */
                valid = row->operand_count == 1 &&
                        row->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        row->operand_slots[1] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        operand_is_function_bool(plan, function, function_index,
                                                 row->operand_slots[0]);
                break;
            default:
                valid = false;
                break;
        }
        /* Only a terminator has no result. Every other opcode reaches the slot
         * proof below, so an absent result slot cannot turn a computation into
         * an unchecked no-op row. */
        if (!valid || XR_TARGET_INSTRUCTION_IS_TERMINATOR(row->opcode))
            continue;
        const XrTargetSlotRecord *result = NULL;
        if (!slot_is_family(plan, function, function_index, row->result_slot,
                            row_result_family(row->opcode), &result) ||
            (row->opcode != XR_TARGET_INSTRUCTION_PARAM_I64 &&
             result->role == XR_TARGET_SLOT_PARAMETER)) {
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
    if (!valid || bound_arguments != dense_arguments ||
        function_parameter_slot_count(plan, function) != parameter_rows ||
        !xr_target_instruction_rows_control_flow_is_exact(
            rows, row_count, function->slot_begin, function->slot_count))
        return report(error, error_size, "XR_TARGET_1005",
                      "instruction program is not an exact closed i64 program");
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
    if (!instruction_count)
        return true;
    if (!instructions)
        return report(error, error_size, "XR_EXEC_5003",
                      "instruction table storage is missing");

    uint32_t begin = 0;
    while (begin < instruction_count) {
        uint32_t function = instructions[begin].function;
        uint32_t end = begin + 1u;
        if (instructions[begin].id != begin ||
            (begin && instructions[begin - 1u].function >= function))
            return report(error, error_size, "XR_TARGET_1005",
                          "instruction table order is not canonical");
        while (end < instruction_count && instructions[end].function == function) {
            if (instructions[end].id != end)
                return report(error, error_size, "XR_TARGET_1005",
                              "instruction identifiers are not dense");
            end++;
        }
        if (!verify_function_group(plan, &instructions[begin], end - begin,
                                   function, error, error_size))
            return false;
        begin = end;
    }
    return true;
}
