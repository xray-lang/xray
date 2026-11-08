/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_entry.inc.c - Conservative activation initialization requirements.
 *
 * KEY CONCEPT:
 *   Scalar locals start at zero unless every read follows a write within its
 *   basic block. Managed locals always start empty. Call entry consumes only
 *   this prepared register list and never scans bytecode.
 */

typedef struct {
    uint8_t registers[XR_RBC_REGISTER_COUNT_MAX];
    uint8_t count;
} XrRbcEntry;

typedef struct {
    uint8_t clear[XR_RBC_REGISTER_COUNT_MAX];
    size_t defined[XR_RBC_REGISTER_COUNT_MAX], stamp;
    uint32_t parameters;
} XrRbcEntryState;

static void xr_rbc_entry_read(XrRbcEntryState *state, uint32_t slot) {
    if (slot >= state->parameters && state->defined[slot] != state->stamp) state->clear[slot] = 1;
}

static void xr_rbc_entry_instruction(const XrRbcModule *module, uint32_t word,
    XrRbcEntryState *state) {
    uint32_t op = word & 255, a = (word >> 8) & 255, b = (word >> 16) & 255, c = word >> 24;
    bool writes = false;
    switch (op) {
    case XR_RBC_LOADK: case XR_RBC_LOADI: case XR_RBC_STRING_LITERAL: case XR_RBC_RELOAD:
        writes = true;
        break;
    case XR_RBC_MOV: case XR_RBC_NEG: case XR_RBC_FNEG: case XR_RBC_ITOF: case XR_RBC_FTOI:
    case XR_RBC_ADDI: case XR_RBC_SUBI: case XR_RBC_MULI: case XR_RBC_REMI:
    case XR_RBC_CELL_READ: case XR_RBC_STRING_I64:
    case XR_RBC_STRING_F64: case XR_RBC_STRING_RETAIN: case XR_RBC_STRING_TAKE:
    case XR_RBC_STRING_LEN: case XR_RBC_STRING_CELL_NEW:
        xr_rbc_entry_read(state, b);
        writes = true;
        break;
    case XR_RBC_ADD: case XR_RBC_SUB: case XR_RBC_MUL: case XR_RBC_DIV: case XR_RBC_REM:
    case XR_RBC_AND: case XR_RBC_OR: case XR_RBC_XOR: case XR_RBC_SHL: case XR_RBC_SHR:
    case XR_RBC_USHR: case XR_RBC_FADD: case XR_RBC_FSUB: case XR_RBC_FMUL: case XR_RBC_FDIV:
    case XR_RBC_FEQ: case XR_RBC_FNE: case XR_RBC_FLT: case XR_RBC_FLE: case XR_RBC_FGT:
    case XR_RBC_FGE: case XR_RBC_STRING_CONCAT:
        xr_rbc_entry_read(state, b);
        xr_rbc_entry_read(state, c);
        writes = true;
        break;
    case XR_RBC_CELL_WRITE: case XR_RBC_LT: case XR_RBC_LE: case XR_RBC_EQ:
        xr_rbc_entry_read(state, b);
        xr_rbc_entry_read(state, a);
        break;
    case XR_RBC_CELL_NEW: case XR_RBC_SPILL:
        writes = true;
        xr_rbc_entry_read(state, a);
        break;
    case XR_RBC_RET: case XR_RBC_STRING_DROP: case XR_RBC_STRING_PRINT: case XR_RBC_STRING_CELL_DROP:
    case XR_RBC_LTI: case XR_RBC_LEI: case XR_RBC_GTI: case XR_RBC_GEI: case XR_RBC_EQI:
        xr_rbc_entry_read(state, a);
        break;
    case XR_RBC_CALL: case XR_RBC_CALLN: {
        uint32_t count;
        writes = true;
        if (op == XR_RBC_CALL) count = module->functions[word >> 16].parameter_count;
        else {
            XrRbcNativeKind kind = module->natives[word >> 16];
            count = kind == XR_RBC_NATIVE_PRINT_I64 || kind == XR_RBC_NATIVE_UTC_OFFSET_AT ? 2u : 1u;
            writes = kind != XR_RBC_NATIVE_PRINT_I64;
        }
        for (uint32_t p = 1; p <= count; ++p) xr_rbc_entry_read(state, a + p);
        break;
    }
    case XR_RBC_JMP: case XR_RBC_RET0: case XR_RBC_PRINT_LITERAL:
        break;
    default:
        memset(state->clear, 1, sizeof(state->clear));
        break;
    }
    if (writes) state->defined[a] = state->stamp;
}

static bool xr_rbc_entries(const XrRbcModule *module, XrRbcEntry *entries) {
    size_t maximum = 0;
    for (size_t f = 0; f < module->function_count; ++f)
        if (maximum < module->functions[f].code_count) maximum = module->functions[f].code_count;
    uint8_t *targets = xr_malloc(maximum);
    if (!targets) return false;
    for (size_t f = 0; f < module->function_count; ++f) {
        const XrRbcFunction *function = &module->functions[f];
        XrRbcEntryState state = {.parameters = function->parameter_count};
        memset(targets, 0, function->code_count);
        targets[0] = 1;
        for (size_t pc = 0; pc < function->code_count; ++pc) {
            uint32_t word = function->code[pc], op = word & 255;
            if (op == XR_RBC_JMP) {
                uint32_t bits = word >> 8;
                int32_t offset = bits <= XR_RBC_JUMP_MAX ? (int32_t)bits : (int32_t)bits - 16777216;
                targets[pc + 1 + (size_t)offset] = 1;
            }
            if (op == XR_RBC_LT || op == XR_RBC_LE || op == XR_RBC_EQ ||
                (op >= XR_RBC_LTI && op <= XR_RBC_EQI)) targets[pc + 2] = 1;
            if ((op == XR_RBC_JMP || op == XR_RBC_RET || op == XR_RBC_RET0) &&
                pc + 1 < function->code_count) targets[pc + 1] = 1;
        }
        for (uint32_t r = function->parameter_count; r < function->register_count; ++r)
            state.clear[r] = function->register_kinds && function->register_kinds[r] != XR_RBC_BITS;
        for (size_t pc = 0; pc < function->code_count; ++pc) {
            if (targets[pc]) state.stamp = pc + 1;
            xr_rbc_entry_instruction(module, function->code[pc], &state);
        }
        entries[f].count = 0;
        for (uint32_t r = function->parameter_count; r < function->register_count; ++r)
            if (state.clear[r]) entries[f].registers[entries[f].count++] = (uint8_t)r;
    }
    xr_free(targets);
    return true;
}

static XR_AINLINE void xr_rbc_entry_clear(XrRbcRegister *registers, const XrRbcEntry *entry) {
    for (unsigned i = 0; i < entry->count; ++i)
        registers[entry->registers[i]] = (XrRbcRegister){0};
}
