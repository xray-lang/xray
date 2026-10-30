/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_string_admission.inc.c - Validate String owners and empty managed loop-head obligations
 */
enum { RBC_BITS, RBC_STRING_BORROWED, RBC_STRING_OWNED, RBC_CELL_BORROWED,
       RBC_CELL_OWNED, RBC_EMPTY };
typedef struct RbcStringAdmission {
    const XrRbcModule *module;
    const XrRbcFunction *function;
    uint8_t state[XR_RBC_REGISTER_COUNT_MAX];
    uint32_t origin[XR_RBC_REGISTER_COUNT_MAX];
    uint32_t published;
    uint8_t *spills;
} RbcStringAdmission;

typedef struct RbcStringIncoming {
    RbcStringAdmission state;
    size_t begin, end;
    uint32_t next;
    bool seen, queued;
} RbcStringIncoming;

typedef struct RbcStringGraph {
    RbcStringAdmission work;
    RbcStringIncoming *incoming;
    uint32_t *indices;
    uint8_t *spills;
    size_t count;
    uint32_t head, tail;
    bool parameters_only;
} RbcStringGraph;

static XrRbcRegisterKind rbc_register_kind(const XrRbcFunction *f, uint32_t r) {
    return f->register_kinds ? f->register_kinds[r] : XR_RBC_BITS;
}
static bool rbc_string_state(uint8_t state) {
    return state == RBC_STRING_BORROWED || state == RBC_STRING_OWNED;
}
static bool rbc_cell_state(uint8_t state) {
    return state == RBC_CELL_BORROWED || state == RBC_CELL_OWNED;
}
static bool rbc_cell_parameter_origin(uint32_t parameter, uint32_t *origin) {
    if (parameter >= XR_RBC_REGISTER_COUNT_MAX || parameter > UINT32_MAX-UINT32_C(65537))
        return false;
    *origin = UINT32_C(65537) + parameter;
    return true;
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
static void rbc_string_forget_borrows(RbcStringAdmission *s, uint32_t owner) {
    for (uint32_t r = 0; r < s->function->register_count; ++r)
        if (rbc_register_kind(s->function, r) == XR_RBC_CALL_WINDOW &&
            s->state[r] == RBC_STRING_BORROWED && s->origin[r] == owner + 1)
            s->state[r] = RBC_EMPTY;
}
static bool rbc_strings_value(RbcStringAdmission *s, XrRbcOperands v) {
    uint8_t *state = s->state;
    switch (v.op) {
    case XR_RBC_MOV:
        if (rbc_string_source(s, v.b)) {
            if (rbc_register_kind(s->function, v.a) != XR_RBC_CALL_WINDOW ||
                !rbc_bits_destination(s, v.a)) return false;
            state[v.a] = RBC_STRING_BORROWED;
            s->origin[v.a] = v.b + 1;
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
    case XR_RBC_STRING_I64: case XR_RBC_STRING_F64:
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
            (rbc_register_kind(s->function, v.b) != XR_RBC_CALL_WINDOW &&
             rbc_register_kind(s->function, v.b) != XR_RBC_STRING)) return false;
        state[v.b] = RBC_EMPTY;
        rbc_string_forget_borrows(s, v.b);
        break;
    case XR_RBC_STRING_DROP:
        if (rbc_register_kind(s->function, v.a) != XR_RBC_STRING || state[v.a] != RBC_STRING_OWNED) return false;
        state[v.a] = RBC_EMPTY;
        rbc_string_forget_borrows(s, v.a);
        return true;
    case XR_RBC_STRING_PRINT:
        return rbc_string_source(s, v.a);
    case XR_RBC_STRING_LEN:
        if (!rbc_bits_destination(s, v.a) || !rbc_string_source(s, v.b)) return false;
        state[v.a] = RBC_BITS;
        return true;
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
        s->origin[v.a] = v.c + 1;
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
        uint32_t origin = s->origin[v.a];
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
static bool rbc_string_step(RbcStringAdmission *s, XrRbcOperands v) {
    const XrRbcFunction *f = s->function;
        switch (v.op) {
        case XR_RBC_MOV: case XR_RBC_STRING_LITERAL: case XR_RBC_STRING_I64: case XR_RBC_STRING_F64:
        case XR_RBC_STRING_RETAIN: case XR_RBC_STRING_CONCAT: case XR_RBC_STRING_TAKE:
        case XR_RBC_STRING_DROP: case XR_RBC_STRING_PRINT: case XR_RBC_STRING_LEN:
            if (!rbc_strings_value(s, v)) return false; break;
        case XR_RBC_CELL_NEW: case XR_RBC_CELL_READ: case XR_RBC_CELL_WRITE:
        case XR_RBC_STRING_CELL_NEW: case XR_RBC_STRING_CELL_DROP:
            if (!rbc_strings_cell(s, v)) return false; break;
        case XR_RBC_CALL: case XR_RBC_CALLN:
            if (!rbc_strings_call(s, v)) return false; break;
        case XR_RBC_RET: case XR_RBC_RET0:
            if (f->result_kind == XR_RBC_STRING) {
                if (v.op != XR_RBC_RET || s->state[v.a] != RBC_STRING_OWNED) return false;
                s->state[v.a] = RBC_EMPTY;
            } else if (v.op == XR_RBC_RET && s->state[v.a] != RBC_BITS) return false;
            for (uint32_t r = 0; r < f->register_count; ++r)
                if (s->state[r] == RBC_STRING_OWNED || s->state[r] == RBC_CELL_OWNED) return false;
            break;
        case XR_RBC_LOADK: case XR_RBC_LOADI:
            if (!rbc_bits_destination(s, v.a)) return false;
            s->state[v.a] = RBC_BITS; break;
        case XR_RBC_PRINT_LITERAL: break;
        case XR_RBC_NEG: case XR_RBC_FNEG:
            if (s->state[v.b] != RBC_BITS || !rbc_bits_destination(s, v.a)) return false;
            s->state[v.a] = RBC_BITS; break;
        case XR_RBC_ITOF: case XR_RBC_FTOI:
            if (rbc_register_kind(s->function, v.a) != XR_RBC_BITS ||
                rbc_register_kind(s->function, v.b) != XR_RBC_BITS ||
                s->state[v.b] != RBC_BITS || !rbc_bits_destination(s, v.a)) return false;
            s->state[v.a] = RBC_BITS; break;
        case XR_RBC_ADD: case XR_RBC_SUB: case XR_RBC_MUL: case XR_RBC_DIV: case XR_RBC_REM:
        case XR_RBC_FADD: case XR_RBC_FSUB: case XR_RBC_FMUL: case XR_RBC_FDIV:
        case XR_RBC_FEQ: case XR_RBC_FNE: case XR_RBC_FLT: case XR_RBC_FLE: case XR_RBC_FGT: case XR_RBC_FGE:
        case XR_RBC_AND: case XR_RBC_OR: case XR_RBC_XOR: case XR_RBC_SHL: case XR_RBC_SHR: case XR_RBC_USHR:
            if (s->state[v.b] != RBC_BITS || s->state[v.c] != RBC_BITS || !rbc_bits_destination(s, v.a)) return false;
            s->state[v.a] = RBC_BITS; break;
        default: return false;
        }
    return true;
}
static void rbc_string_state_copy(RbcStringAdmission *to, const RbcStringAdmission *from) {
    uint8_t *spills = to->spills;
    *to = *from;
    to->spills = spills;
    if (from->function->spill_count)
        memcpy(to->spills,from->spills,from->function->spill_count);
}
static bool rbc_string_merge(RbcStringAdmission *to, const RbcStringAdmission *from,
    bool *seen, bool *changed) {
    if (!*seen) {
        rbc_string_state_copy(to,from);
        *seen = *changed = true;
        return true;
    }
    if (to->published != from->published) return false;
    for (uint32_t r = 0; r < from->function->register_count; ++r) {
        if (to->state[r] != from->state[r]) {
            if ((to->state[r] != RBC_BITS && to->state[r] != RBC_EMPTY) ||
                (from->state[r] != RBC_BITS && from->state[r] != RBC_EMPTY)) return false;
            if (to->state[r] != RBC_EMPTY) {
                to->state[r] = RBC_EMPTY;
                *changed = true;
            }
        }
        if ((to->state[r] == RBC_STRING_BORROWED || rbc_cell_state(to->state[r])) &&
            to->origin[r] != from->origin[r]) return false;
    }
    for (uint32_t i=0; i<from->function->spill_count; ++i) {
        if (to->spills[i]==from->spills[i]) continue;
        if (i<from->function->reference_spill_count ||
            (to->spills[i]!=RBC_BITS && to->spills[i]!=RBC_EMPTY) ||
            (from->spills[i]!=RBC_BITS && from->spills[i]!=RBC_EMPTY)) return false;
        if (to->spills[i] != RBC_EMPTY) {
            to->spills[i] = RBC_EMPTY;
            *changed = true;
        }
    }
    return true;
}
static bool rbc_string_jump(const XrRbcFunction *f, size_t pc, size_t *target) {
    uint32_t bits = f->code[pc] >> 8;
    int32_t offset = bits <= XR_RBC_JUMP_MAX ? (int32_t)bits : (int32_t)bits - 16777216;
    size_t next = pc + 1;
    if (offset < 0) {
        if ((size_t)-offset > next) return false;
        *target = next - (size_t)-offset;
    } else {
        if ((size_t)offset >= f->code_count - next) return false;
        *target = next + (size_t)offset;
    }
    return true;
}
/* A frame Cell may survive a loop only with the same live symbolic identity.
 * Ordinary Strings and staged borrows retain their empty-entry obligations. */
static bool rbc_string_backedge(const RbcStringAdmission *from) {
    for (uint32_t r = 0; r < from->function->register_count; ++r) {
        XrRbcRegisterKind kind = rbc_register_kind(from->function, r);
        uint8_t now = from->state[r];
        if (kind == XR_RBC_STRING_CELL) {
            if (now != RBC_EMPTY && (!rbc_cell_state(now) || !from->origin[r])) return false;
        } else if (kind == XR_RBC_STRING) {
            if (now != RBC_EMPTY) return false;
        } else if (kind == XR_RBC_CALL_WINDOW) {
            if (now != RBC_EMPTY && now != RBC_BITS) return false;
        }
    }
    for (uint32_t i = 0; i < from->function->reference_spill_count; ++i)
        if (from->spills[i] != RBC_EMPTY) return false;
    return true;
}
static bool rbc_spill_transfer(RbcStringAdmission *s, XrRbcOperands v) {
    uint32_t index = (uint32_t)v.immediate;
    bool string = index < s->function->reference_spill_count;
    XrRbcRegisterKind kind = rbc_register_kind(s->function, v.a);
    uint8_t initialized = string ? RBC_STRING_OWNED : RBC_BITS;
    if (kind != (string ? XR_RBC_STRING : XR_RBC_BITS)) return false;
    if (v.op == XR_RBC_SPILL) {
        if (s->state[v.a] != initialized ||
            (string && s->spills[index] != RBC_EMPTY)) return false;
        s->spills[index] = initialized;
        s->state[v.a] = RBC_EMPTY;
        if (string) rbc_string_forget_borrows(s, v.a);
    } else {
        if (s->spills[index] != initialized ||
            (string && s->state[v.a] != RBC_EMPTY)) return false;
        s->state[v.a] = initialized;
        s->spills[index] = RBC_EMPTY;
    }
    return true;
}
static void rbc_string_graph_free(RbcStringGraph *g) {
    xr_free(g->spills);
    xr_free(g->incoming);
    xr_free(g->indices);
}

static XrRbcStatus rbc_string_graph_build(RbcStringGraph *g) {
    const XrRbcFunction *f = g->work.function;
    if (!f->code_count) return XR_RBC_INVALID;
    if (f->code_count > SIZE_MAX / sizeof(uint32_t)) return XR_RBC_OUT_OF_MEMORY;
    g->indices = xr_malloc(f->code_count * sizeof(*g->indices));
    if (!g->indices) return XR_RBC_OUT_OF_MEMORY;
    for (size_t pc = 0; pc < f->code_count; ++pc) g->indices[pc] = UINT32_MAX;
    g->indices[0] = 0;
    for (size_t pc = 0; pc < f->code_count; ++pc) {
        XrRbcOp op = (XrRbcOp)(f->code[pc] & 255);
        if (op == XR_RBC_JMP) {
            size_t target;
            if (!rbc_string_jump(f, pc, &target)) return XR_RBC_INVALID;
            g->indices[target] = 0;
        } else if (op == XR_RBC_LT || op == XR_RBC_LE || op == XR_RBC_EQ) {
            if (pc + 2 >= f->code_count) return XR_RBC_INVALID;
            g->indices[pc + 1] = g->indices[pc + 2] = 0;
        }
    }
    for (size_t pc = 0; pc < f->code_count; ++pc)
        if (g->indices[pc] != UINT32_MAX) {
            if (g->count == UINT32_MAX) return XR_RBC_INVALID;
            g->indices[pc] = (uint32_t)g->count++;
        }
    if (g->count > SIZE_MAX / sizeof(*g->incoming)) return XR_RBC_OUT_OF_MEMORY;
    g->incoming = xr_calloc(g->count, sizeof(*g->incoming));
    if (!g->incoming) return XR_RBC_OUT_OF_MEMORY;
    size_t rows = g->count + 1;
    if (rows <= g->count || (f->spill_count && rows > SIZE_MAX / f->spill_count))
        return XR_RBC_OUT_OF_MEMORY;
    if (f->spill_count) {
        g->spills = xr_malloc(rows * f->spill_count);
        if (!g->spills) return XR_RBC_OUT_OF_MEMORY;
        memset(g->spills, RBC_EMPTY, rows * f->spill_count);
        for (size_t b = 0; b < g->count; ++b)
            g->incoming[b].state.spills = g->spills + (b + 1) * f->spill_count;
    }
    uint32_t previous = UINT32_MAX;
    for (size_t pc = 0; pc < f->code_count; ++pc) {
        uint32_t index = g->indices[pc];
        if (index == UINT32_MAX) continue;
        if (previous != UINT32_MAX) g->incoming[previous].end = pc;
        g->incoming[index].begin = pc;
        previous = index;
    }
    g->incoming[previous].end = f->code_count;
    g->work.spills = g->spills;
    for (uint32_t r = 0; r < f->register_count; ++r) {
        XrRbcRegisterKind kind = rbc_register_kind(f,r);
        g->work.state[r] = kind == XR_RBC_BITS &&
            (!g->parameters_only || r<f->parameter_count) ? RBC_BITS : RBC_EMPTY;
        if (r < f->parameter_count && kind == XR_RBC_STRING) g->work.state[r] = RBC_STRING_BORROWED;
        if (r < f->parameter_count && kind == XR_RBC_STRING_CELL) {
            g->work.state[r] = RBC_CELL_BORROWED;
            /* Local cells use ordinals 1..65536. Parameters are separate
             * symbolic identities, never a claim that actual arguments differ. */
            if (!rbc_cell_parameter_origin(r, &g->work.origin[r])) return XR_RBC_INVALID;
        }
    }
    return XR_RBC_OK;
}

/* IN facts never come from this mutable transfer state. A later predecessor
 * can remove scalar guarantees and must schedule a fresh evaluation. */
static bool rbc_string_propagate(RbcStringGraph *g, size_t target, bool backward) {
    uint32_t index = g->indices[target];
    if (index == UINT32_MAX || (backward && !rbc_string_backedge(&g->work))) return false;
    RbcStringIncoming *next = &g->incoming[index];
    bool changed = false;
    if (!rbc_string_merge(&next->state, &g->work, &next->seen, &changed)) return false;
    if (changed && !next->queued) {
        next->next = UINT32_MAX;
        if (g->tail == UINT32_MAX) g->head = index;
        else g->incoming[g->tail].next = index;
        g->tail = index;
        next->queued = true;
    }
    return true;
}

static bool rbc_string_block(RbcStringGraph *g, uint32_t index) {
    const XrRbcFunction *f = g->work.function;
    const RbcStringIncoming *block = &g->incoming[index];
    rbc_string_state_copy(&g->work, &block->state);
    for (size_t pc = block->begin; pc < block->end; ++pc) {
        uint32_t word = f->code[pc];
        XrRbcOperands v = {(XrRbcOp)(word & 255), (word >> 8) & 255,
            (word >> 16) & 255, word >> 24, word >> 16};
        if (v.op == XR_RBC_LT || v.op == XR_RBC_LE || v.op == XR_RBC_EQ) {
            return g->work.state[v.a] == RBC_BITS && g->work.state[v.b] == RBC_BITS &&
                rbc_string_propagate(g, pc + 1, false) && rbc_string_propagate(g, pc + 2, false);
        } else if (v.op == XR_RBC_JMP) {
            size_t target;
            return rbc_string_jump(f, pc, &target) && rbc_string_propagate(g, target, target <= pc);
        } else {
            bool valid = v.op==XR_RBC_SPILL || v.op==XR_RBC_RELOAD ?
                rbc_spill_transfer(&g->work,v) : rbc_string_step(&g->work,v);
            if (!valid) return false;
            if (v.op == XR_RBC_RET || v.op == XR_RBC_RET0) {
                for (uint32_t i=0; i<f->reference_spill_count; ++i)
                    if (g->work.spills[i]!=RBC_EMPTY) return false;
                return true;
            }
        }
    }
    return block->end < f->code_count && rbc_string_propagate(g, block->end, false);
}

static XrRbcStatus xr_rbc_strings_valid(const XrRbcModule *module, const XrRbcFunction *f) {
    bool managed = f->result_kind == XR_RBC_STRING;
    bool numeric_conversion = false;
    for (uint32_t r = 0; f->register_kinds && r < f->register_count; ++r)
        managed |= f->register_kinds[r] == XR_RBC_STRING || f->register_kinds[r] == XR_RBC_STRING_CELL;
    for (size_t pc = 0; pc < f->code_count; ++pc) {
        uint32_t word = f->code[pc], op = word & 255;
        numeric_conversion |= op == XR_RBC_ITOF || op == XR_RBC_FTOI;
        managed |= op >= XR_RBC_STRING_LITERAL && op <= XR_RBC_STRING_CELL_DROP;
        managed |= op == XR_RBC_STRING_LEN || op == XR_RBC_STRING_F64;
        if (op == XR_RBC_CALL) {
            const XrRbcFunction *callee = &module->functions[word >> 16];
            managed |= callee->result_kind == XR_RBC_STRING;
            for (uint32_t p = 0; p < callee->parameter_count; ++p)
                managed |= rbc_register_kind(callee, p) != XR_RBC_BITS;
        }
    }
    if (!managed && !f->spill_count && !numeric_conversion) return XR_RBC_OK;
    if (!f->register_kinds && (managed || f->reference_spill_count)) return XR_RBC_INVALID;
    RbcStringGraph g = {.work = {.module = module, .function = f},
        .head = UINT32_MAX, .tail = UINT32_MAX,
        .parameters_only = numeric_conversion || f->spill_count != 0};
    XrRbcStatus status = rbc_string_graph_build(&g);
    if (status == XR_RBC_OK) {
        /* This initial merge remains a virtual predecessor even for a jump to
         * pc zero. A cycle cannot manufacture definitions for the first entry. */
        bool valid = rbc_string_propagate(&g, 0, false);
        while (valid && g.head != UINT32_MAX) {
            uint32_t index = g.head;
            RbcStringIncoming *next = &g.incoming[index];
            g.head = next->next;
            if (g.head == UINT32_MAX) g.tail = UINT32_MAX;
            next->queued = false;
            valid = rbc_string_block(&g, index);
        }
        status = valid ? XR_RBC_OK : XR_RBC_INVALID;
    }
    rbc_string_graph_free(&g);
    return status;
}
