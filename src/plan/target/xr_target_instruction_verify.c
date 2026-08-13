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
 *   must independently prove a closed straight-line i64 program; absence
 *   means execution is unavailable, never an empty successful program.
 */

#include "xr_target_instruction_verify.h"
#include "../../base/xmalloc.h"
#include <stdio.h>

static bool report(char *error, size_t error_size, const char *code,
                   const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", code, detail);
    return false;
}

static bool range_valid(uint32_t begin, uint32_t count, uint32_t total) {
    return begin <= total && count <= total - begin;
}

static bool rep_is_trivial_i64(const XrTargetMachineRepRecord *rep) {
    return rep && rep->kind == XR_MACHINE_REP_I64 &&
           rep->register_bits == 64 && rep->memory_size == 8 &&
           rep->memory_align == 8 && rep->signedness == XR_TARGET_SIGN_SIGNED &&
           rep->root_kind == XR_TARGET_ROOT_NONE &&
           rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
}

static bool slot_is_i64(const XrTargetPlan *plan,
                        const XrTargetFunctionRecord *function,
                        uint32_t function_index, uint32_t slot_index,
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
    if (slot->id != slot_index || slot->function != function_index ||
        slot->size != 8 || slot->align != 8 ||
        slot->root_kind != XR_TARGET_ROOT_NONE ||
        slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        !rep_is_trivial_i64(register_rep) || !rep_is_trivial_i64(memory_rep))
        return false;
    if (out)
        *out = slot;
    return true;
}

static bool operand_is_defined(const XrTargetPlan *plan,
                               const XrTargetFunctionRecord *function,
                               uint32_t function_index, uint32_t slot,
                               const uint8_t *defined) {
    return slot_is_i64(plan, function, function_index, slot, NULL) &&
           defined[slot - function->slot_begin] != 0;
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
            case XR_TARGET_INSTRUCTION_COPY_I64:
            case XR_TARGET_INSTRUCTION_NEG_WRAP_I64:
            case XR_TARGET_INSTRUCTION_BNOT_I64:
                valid = row->operand_count == 1 && row->immediate_bits == 0 &&
                        row->operand_slots[1] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        operand_is_defined(plan, function, function_index,
                                           row->operand_slots[0], defined) &&
                        !terminal;
                break;
            case XR_TARGET_INSTRUCTION_ADD_WRAP_I64:
            case XR_TARGET_INSTRUCTION_SUB_WRAP_I64:
            case XR_TARGET_INSTRUCTION_MUL_WRAP_I64:
            case XR_TARGET_INSTRUCTION_BAND_I64:
            case XR_TARGET_INSTRUCTION_BOR_I64:
            case XR_TARGET_INSTRUCTION_BXOR_I64:
                valid = row->operand_count == 2 && row->immediate_bits == 0 &&
                        operand_is_defined(plan, function, function_index,
                                           row->operand_slots[0], defined) &&
                        operand_is_defined(plan, function, function_index,
                                           row->operand_slots[1], defined) &&
                        !terminal;
                break;
            case XR_TARGET_INSTRUCTION_RETURN_I64:
                valid = row->operand_count == 1 && row->immediate_bits == 0 &&
                        row->result_slot == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        row->operand_slots[1] == XR_TARGET_INSTRUCTION_SLOT_NONE &&
                        operand_is_defined(plan, function, function_index,
                                           row->operand_slots[0], defined) &&
                        terminal;
                break;
            default:
                valid = false;
                break;
        }
        if (!valid || row->opcode == XR_TARGET_INSTRUCTION_RETURN_I64)
            continue;
        if (!slot_is_i64(plan, function, function_index, row->result_slot,
                         NULL)) {
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
    if (!valid || rows[row_count - 1u].opcode != XR_TARGET_INSTRUCTION_RETURN_I64)
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
