/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_string_admission.inc.c - Validate explicit straight-line String owners
 */
enum { RBC_BITS, RBC_STRING_BORROWED, RBC_STRING_OWNED, RBC_CELL_BORROWED,
       RBC_CELL_OWNED, RBC_EMPTY };
typedef struct RbcStringAdmission {
    const XrRbcModule *module;
    const XrRbcFunction *function;
    uint8_t state[XR_RBC_REGISTER_COUNT_MAX];
    uint16_t origin[XR_RBC_REGISTER_COUNT_MAX];
    uint32_t published;
} RbcStringAdmission;

static XrRbcRegisterKind rbc_register_kind(const XrRbcFunction *f, uint32_t r) {
    return f->register_kinds ? f->register_kinds[r] : XR_RBC_BITS;
}
static bool rbc_string_state(uint8_t state) {
    return state == RBC_STRING_BORROWED || state == RBC_STRING_OWNED;
}
static bool rbc_cell_state(uint8_t state) {
    return state == RBC_CELL_BORROWED || state == RBC_CELL_OWNED;
}
static bool rbc_string_source(const RbcStringAdmission *s, uint32_t r) {
    return rbc_register_kind(s->function, r) == XR_RBC_STRING && rbc_string_state(s->state[r]);
}
static bool rbc_string_destination(RbcStringAdmission *s, uint32_t r) {
    return rbc_register_kind(s->function, r) == XR_RBC_STRING && s->state[r] == RBC_EMPTY;
}
static bool rbc_bits_destination(RbcStringAdmission *s, uint32_t r) {
    XrRbcRegisterKind kind = rbc_register_kind(s->function, r);
    return (kind == XR_RBC_BITS || kind == XR_RBC_CALL_WINDOW) &&
        s->state[r] != RBC_STRING_OWNED && s->state[r] != RBC_CELL_OWNED;
}
static bool rbc_strings_value(RbcStringAdmission *s, XrRbcOperands v) {
    uint8_t *state = s->state;
    switch (v.op) {
    case XR_RBC_MOV:
        if (rbc_string_source(s, v.b)) {
            if (rbc_register_kind(s->function, v.a) != XR_RBC_CALL_WINDOW ||
                !rbc_bits_destination(s, v.a)) return false;
            state[v.a] = RBC_STRING_BORROWED;
            s->origin[v.a] = (uint16_t)(v.b + 1);
        } else if (rbc_cell_state(state[v.b])) {
            XrRbcRegisterKind kind = rbc_register_kind(s->function, v.a);
            if ((kind != XR_RBC_CALL_WINDOW && kind != XR_RBC_STRING_CELL) ||
                state[v.a] == RBC_CELL_OWNED) return false;
            state[v.a] = RBC_CELL_BORROWED;
            s->origin[v.a] = s->origin[v.b];
        } else {
            if (state[v.b] != RBC_BITS || !rbc_bits_destination(s, v.a)) return false;
            state[v.a] = RBC_BITS;
        }
        return true;
    case XR_RBC_STRING_LITERAL:
        if (!rbc_string_destination(s, v.a)) return false;
        break;
    case XR_RBC_STRING_I64:
        if (!rbc_string_destination(s, v.a) || state[v.b] != RBC_BITS) return false;
        break;
    case XR_RBC_STRING_RETAIN:
        if (!rbc_string_destination(s, v.a) || !rbc_string_source(s, v.b)) return false;
        break;
    case XR_RBC_STRING_CONCAT:
        if (!rbc_string_destination(s, v.a) || !rbc_string_source(s, v.b) ||
            !rbc_string_source(s, v.c)) return false;
        break;
    case XR_RBC_STRING_TAKE:
        if (!rbc_string_destination(s, v.a) || state[v.b] != RBC_STRING_OWNED ||
            rbc_register_kind(s->function, v.b) != XR_RBC_CALL_WINDOW) return false;
        state[v.b] = RBC_EMPTY;
        break;
    case XR_RBC_STRING_DROP:
        if (rbc_register_kind(s->function, v.a) != XR_RBC_STRING || state[v.a] != RBC_STRING_OWNED) return false;
        state[v.a] = RBC_EMPTY;
        for (uint32_t r = 0; r < s->function->register_count; ++r)
            if (rbc_register_kind(s->function, r) == XR_RBC_CALL_WINDOW &&
                state[r] == RBC_STRING_BORROWED && s->origin[r] == v.a + 1) state[r] = RBC_EMPTY;
        return true;
    case XR_RBC_STRING_PRINT:
        return rbc_string_source(s, v.a);
    default: return false;
    }
    state[v.a] = RBC_STRING_OWNED;
    return true;
}
static bool rbc_strings_cell(RbcStringAdmission *s, XrRbcOperands v) {
    const XrRbcFunction *f = s->function;
    uint8_t *state = s->state;
    if (v.op == XR_RBC_STRING_CELL_NEW) {
        if (rbc_register_kind(f, v.a) != XR_RBC_STRING_CELL || state[v.a] != RBC_EMPTY ||
            !rbc_string_source(s, v.b) || v.c != s->published ||
            !f->cell_kinds || f->cell_kinds[v.c] != XR_RBC_STRING) return false;
        ++s->published;
        state[v.a] = RBC_CELL_OWNED;
        s->origin[v.a] = (uint16_t)(v.c + 1);
        return true;
    }
    if (v.op == XR_RBC_CELL_NEW) {
        uint32_t index = (uint32_t)v.immediate;
        if (state[v.a] != RBC_BITS || !rbc_bits_destination(s, v.a) || index > s->published ||
            (f->cell_kinds && f->cell_kinds[index] != XR_RBC_BITS)) return false;
        if (index == s->published) ++s->published;
        return true;
    }
    if (v.op == XR_RBC_STRING_CELL_DROP) {
        if (state[v.a] != RBC_CELL_OWNED || !s->origin[v.a]) return false;
        uint16_t origin = s->origin[v.a];
        for (uint32_t r = 0; r < f->register_count; ++r)
            if (rbc_cell_state(state[r]) && s->origin[r] == origin) state[r] = RBC_EMPTY;
        return true;
    }
    uint32_t cell = v.op == XR_RBC_CELL_READ ? v.b : v.a;
    if (rbc_register_kind(f, cell) == XR_RBC_STRING_CELL) {
        if (!rbc_cell_state(state[cell])) return false;
        if (v.op == XR_RBC_CELL_READ) {
            if (!rbc_string_destination(s, v.a)) return false;
            state[v.a] = RBC_STRING_OWNED;
        } else if (!rbc_string_source(s, v.b)) return false;
        return true;
    }
    if (state[cell] != RBC_BITS) return false;
    if (v.op == XR_RBC_CELL_READ) {
        if (!rbc_bits_destination(s, v.a)) return false;
        state[v.a] = RBC_BITS;
    } else if (state[v.b] != RBC_BITS) return false;
    return true;
}
static bool rbc_strings_call(RbcStringAdmission *s, XrRbcOperands v) {
    const XrRbcFunction *f = s->function;
    if (rbc_register_kind(f, v.a) != XR_RBC_CALL_WINDOW ||
        s->state[v.a] == RBC_STRING_OWNED || s->state[v.a] == RBC_CELL_OWNED) return false;
    if (v.op == XR_RBC_CALLN) {
        XrRbcNativeKind kind = s->module->natives[v.immediate];
        if ((unsigned)kind >= XR_RBC_NATIVE_COUNT) return false;
        uint32_t count = kind == XR_RBC_NATIVE_PRINT_I64 || kind == XR_RBC_NATIVE_UTC_OFFSET_AT ? 2u : 1u;
        for (uint32_t p = 1; p <= count; ++p) {
            if (s->state[v.a + p] != RBC_BITS) return false;
            s->state[v.a + p] = RBC_EMPTY;
        }
        s->state[v.a] = RBC_BITS;
        return true;
    }
    const XrRbcFunction *callee = &s->module->functions[v.immediate];
    for (uint32_t p = 0; p < callee->parameter_count; ++p) {
        XrRbcRegisterKind kind = rbc_register_kind(callee, p);
        uint8_t state = s->state[v.a + 1 + p];
        if (rbc_register_kind(f, v.a + 1 + p) != XR_RBC_CALL_WINDOW ||
            (kind == XR_RBC_STRING ? state != RBC_STRING_BORROWED :
            kind == XR_RBC_STRING_CELL ? !rbc_cell_state(state) : state != RBC_BITS)) return false;
    }
    for (uint32_t p = 0; p < callee->parameter_count; ++p) s->state[v.a + 1 + p] = RBC_EMPTY;
    s->state[v.a] = callee->result_kind == XR_RBC_STRING ? RBC_STRING_OWNED : RBC_BITS;
    return true;
}
static bool xr_rbc_strings_valid(const XrRbcModule *module, const XrRbcFunction *f) {
    bool managed = f->result_kind == XR_RBC_STRING;
    for (uint32_t r = 0; f->register_kinds && r < f->register_count; ++r)
        managed |= f->register_kinds[r] == XR_RBC_STRING || f->register_kinds[r] == XR_RBC_STRING_CELL;
    for (size_t pc = 0; pc < f->code_count; ++pc) {
        uint32_t word = f->code[pc], op = word & 255;
        managed |= op >= XR_RBC_STRING_LITERAL && op <= XR_RBC_STRING_CELL_DROP;
        if (op == XR_RBC_CALL) {
            const XrRbcFunction *callee = &module->functions[word >> 16];
            managed |= callee->result_kind == XR_RBC_STRING;
            for (uint32_t p = 0; p < callee->parameter_count; ++p)
                managed |= rbc_register_kind(callee, p) != XR_RBC_BITS;
        }
    }
    if (!managed) return true;
    if (!f->register_kinds) return false;
    RbcStringAdmission s = {.module = module, .function = f};
    for (uint32_t r = 0; r < f->register_count; ++r) {
        XrRbcRegisterKind kind = f->register_kinds[r];
        s.state[r] = kind == XR_RBC_BITS ? RBC_BITS : RBC_EMPTY;
        if (r < f->parameter_count && kind == XR_RBC_STRING) s.state[r] = RBC_STRING_BORROWED;
        if (r < f->parameter_count && kind == XR_RBC_STRING_CELL) s.state[r] = RBC_CELL_BORROWED;
    }
    for (size_t pc = 0; pc < f->code_count; ++pc) {
        uint32_t word = f->code[pc];
        XrRbcOperands v = {(XrRbcOp)(word & 255), (word >> 8) & 255,
            (word >> 16) & 255, word >> 24, word >> 16};
        switch (v.op) {
        case XR_RBC_JMP: if (pc || word != XR_RBC_JMP) return false; break;
        case XR_RBC_MOV: case XR_RBC_STRING_LITERAL: case XR_RBC_STRING_I64:
        case XR_RBC_STRING_RETAIN: case XR_RBC_STRING_CONCAT: case XR_RBC_STRING_TAKE:
        case XR_RBC_STRING_DROP: case XR_RBC_STRING_PRINT:
            if (!rbc_strings_value(&s, v)) return false; break;
        case XR_RBC_CELL_NEW: case XR_RBC_CELL_READ: case XR_RBC_CELL_WRITE:
        case XR_RBC_STRING_CELL_NEW: case XR_RBC_STRING_CELL_DROP:
            if (!rbc_strings_cell(&s, v)) return false; break;
        case XR_RBC_CALL: case XR_RBC_CALLN:
            if (!rbc_strings_call(&s, v)) return false; break;
        case XR_RBC_RET: case XR_RBC_RET0:
            if (f->result_kind == XR_RBC_STRING) {
                if (v.op != XR_RBC_RET || s.state[v.a] != RBC_STRING_OWNED) return false;
                s.state[v.a] = RBC_EMPTY;
            } else if (v.op == XR_RBC_RET && s.state[v.a] != RBC_BITS) return false;
            for (uint32_t r = 0; r < f->register_count; ++r)
                if (s.state[r] == RBC_STRING_OWNED || s.state[r] == RBC_CELL_OWNED) return false;
            break;
        case XR_RBC_LOADK: case XR_RBC_LOADI:
            if (!rbc_bits_destination(&s, v.a)) return false;
            s.state[v.a] = RBC_BITS; break;
        case XR_RBC_PRINT_LITERAL: break;
        case XR_RBC_NEG:
            if (s.state[v.b] != RBC_BITS || !rbc_bits_destination(&s, v.a)) return false;
            s.state[v.a] = RBC_BITS; break;
        case XR_RBC_ADD: case XR_RBC_SUB: case XR_RBC_MUL: case XR_RBC_DIV: case XR_RBC_REM:
        case XR_RBC_AND: case XR_RBC_OR: case XR_RBC_XOR: case XR_RBC_SHL: case XR_RBC_SHR: case XR_RBC_USHR:
            if (s.state[v.b] != RBC_BITS || s.state[v.c] != RBC_BITS || !rbc_bits_destination(&s, v.a)) return false;
            s.state[v.a] = RBC_BITS; break;
        default: return false;
        }
    }
    return true;
}
