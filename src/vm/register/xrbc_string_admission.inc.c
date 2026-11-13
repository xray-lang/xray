/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_string_admission.inc.c - Validate String owners and empty managed loop-head obligations
 */
enum { RBC_BITS, RBC_STRING_BORROWED, RBC_STRING_OWNED, RBC_CELL_BORROWED,
       RBC_CELL_OWNED, RBC_EMPTY, RBC_STATE_BORROWED, RBC_STATE_REFERENCE,
       RBC_ARRAY_REF_CELL, RBC_ARRAY_REF_STATE, RBC_ARRAY_REF_FORMAL,
       RBC_VIEW_FRAME, RBC_VIEW_CELL, RBC_VIEW_SLICE, RBC_VIEW_RETURNED, RBC_FUNCTION };
typedef struct RbcSpillState { uint32_t origin; uint8_t state; } RbcSpillState;
typedef struct RbcStringAdmission {
    const XrRbcModule *module;
    const XrRbcFunction *function;
    uint8_t state[XR_RBC_REGISTER_COUNT_MAX];
    uint32_t origin[XR_RBC_REGISTER_COUNT_MAX];
    uint32_t published;
    RbcSpillState *spills;
    uint8_t *aggregates;
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
    RbcSpillState *spills;
    uint8_t *aggregates;
    size_t count;
    uint32_t head, tail;
    bool parameters_only;
} RbcStringGraph;

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
#include "xrbc_array_transfer.inc.c"
#include "xrbc_view_transfer.inc.c"
static bool rbc_strings_call(RbcStringAdmission *s, XrRbcOperands v);
static bool rbc_string_step(RbcStringAdmission *s, XrRbcOperands v);
#include "xrbc_aggregate_transfer.inc.c"
static bool rbc_strings_value(RbcStringAdmission *s, XrRbcOperands v) {
    uint8_t *state = s->state;
    switch (v.op) {
    case XR_RBC_MOV:
        if (state[v.b] == RBC_FUNCTION) {
            XrRbcRegisterKind kind = rbc_register_kind(s->function, v.a);
            if ((kind != XR_RBC_CALL_WINDOW && (kind != XR_RBC_FUNCTION ||
                    s->function->register_callables[v.a] != s->origin[v.b])) ||
                state[v.a] == RBC_STRING_OWNED || state[v.a] == RBC_CELL_OWNED) return false;
            state[v.a] = RBC_FUNCTION;
            s->origin[v.a] = s->origin[v.b];
            return true;
        }
        if (state[v.b] == RBC_VIEW_FRAME || state[v.b] == RBC_VIEW_CELL || state[v.b] == RBC_VIEW_SLICE) {
            XrRbcRegisterKind kind = rbc_register_kind(s->function, v.a);
            XrRbcRegisterKind source = state[v.b] == RBC_VIEW_FRAME ? XR_RBC_VIEW_FRAME :
                state[v.b] == RBC_VIEW_CELL ? XR_RBC_VIEW_CELL : XR_RBC_SLICE_I64;
            if ((kind != source && kind != XR_RBC_CALL_WINDOW) ||
                state[v.a] == RBC_STRING_OWNED || state[v.a] == RBC_CELL_OWNED) return false;
            state[v.a] = state[v.b]; s->origin[v.a] = s->origin[v.b];
            return true;
        }
        if (rbc_array_ref_state(state[v.b])) {
            XrRbcRegisterKind payload = rbc_array_ref_payload(s, state[v.b], s->origin[v.b]);
            XrRbcRegisterKind kind = rbc_register_kind(s->function, v.a);
            XrRbcRegisterKind reference = payload == XR_RBC_STRING ? XR_RBC_STRING_REF :
                xr_rbc_array_ref_at(xr_rbc_array_family(payload));
            if ((!rbc_array_kind(payload) && payload != XR_RBC_STRING) ||
                (kind != XR_RBC_CALL_WINDOW && kind != reference) ||
                state[v.a] == RBC_STRING_OWNED || state[v.a] == RBC_CELL_OWNED) return false;
            state[v.a] = state[v.b]; s->origin[v.a] = s->origin[v.b];
            return true;
        }
        if (state[v.b] == RBC_STATE_BORROWED || state[v.b] == RBC_STATE_REFERENCE) {
            XrRbcRegisterKind kind = rbc_register_kind(s->function, v.a);
            if (state[v.a] == RBC_STRING_OWNED || state[v.a] == RBC_CELL_OWNED) return false;
            if (state[v.b] == RBC_STATE_REFERENCE ? kind != XR_RBC_STATE_REF :
                (kind != XR_RBC_STATE && kind != XR_RBC_CALL_WINDOW)) return false;
            state[v.a] = state[v.b]; s->origin[v.a] = s->origin[v.b];
            return true;
        }
        if (rbc_array_kind(rbc_register_kind(s->function, v.b))) {
            if (!rbc_string_state(state[v.b]) || rbc_register_kind(s->function, v.a) != XR_RBC_CALL_WINDOW ||
                !rbc_bits_destination(s, v.a)) return false;
            state[v.a] = RBC_STRING_BORROWED; s->origin[v.a] = v.b + 1;
        } else if (rbc_string_source(s, v.b)) {
            if (rbc_register_kind(s->function, v.a) != XR_RBC_CALL_WINDOW ||
                !rbc_bits_destination(s, v.a)) return false;
            state[v.a] = RBC_STRING_BORROWED;
            s->origin[v.a] = v.b + 1;
        } else if (rbc_cell_state(state[v.b])) {
            XrRbcRegisterKind kind = rbc_register_kind(s->function, v.a);
            if ((kind != XR_RBC_CALL_WINDOW && kind != rbc_register_kind(s->function, v.b)) ||
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
             rbc_register_kind(s->function, v.b) != XR_RBC_STRING) ||
            (rbc_register_kind(s->function, v.b) == XR_RBC_CALL_WINDOW && s->origin[v.b] != XR_RBC_STRING)) return false;
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
    if (v.op == XR_RBC_STRING_CELL_NEW || v.op == XR_RBC_CELL_NEW)
        while (s->published < f->cell_count && f->cell_kinds && rbc_array_kind(f->cell_kinds[s->published]))
            ++s->published;
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
            if ((rbc_cell_state(state[r]) || state[r] == RBC_ARRAY_REF_CELL) && s->origin[r] == origin) state[r] = RBC_EMPTY;
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
#include "xrbc_state_transfer.inc.c"

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
        if (kind == XR_RBC_FUNCTION) {
            if (s->state[v.a + 1 + p] != RBC_FUNCTION ||
                s->origin[v.a + 1 + p] != callee->register_callables[p]) return false;
            continue;
        }
        if (rbc_register_kind(f, v.a + 1 + p) != XR_RBC_CALL_WINDOW ||
            !rbc_call_parameter_matches(s, v.a + 1 + p, kind)) return false;
    }
    for (uint32_t p = 0; p < callee->parameter_count; ++p) s->state[v.a + 1 + p] = RBC_EMPTY;
    s->state[v.a] = callee->result_kind == XR_RBC_SLICE_I64 ? RBC_VIEW_SLICE :
        rbc_managed_kind(callee->result_kind) ? RBC_STRING_OWNED : RBC_BITS;
    s->origin[v.a] = callee->result_kind;
    return true;
}
static bool rbc_string_step(RbcStringAdmission *s, XrRbcOperands v) {
    const XrRbcFunction *f = s->function;
        switch (v.op) {
        case XR_RBC_VIEW_ACCESS: return rbc_view_transfer(s, (uint32_t)v.immediate);
        case XR_RBC_ARRAY_ACCESS: return rbc_array_transfer(s, (uint32_t)v.immediate);
        case XR_RBC_AGGREGATE_ACCESS: return rbc_aggregate_transfer(s, (uint32_t)v.immediate);
        case XR_RBC_MODULE_BEGIN: case XR_RBC_MODULE_READY:
        case XR_RBC_STATE_CONSTRUCT: case XR_RBC_STATE_PUBLISH:
        case XR_RBC_STATE_INITIALIZE: case XR_RBC_STATE_REFERENCE:
        case XR_RBC_STATE_READ: case XR_RBC_STATE_WRITE:
            return rbc_state_transfer(s, v);
        case XR_RBC_MOV: case XR_RBC_STRING_LITERAL: case XR_RBC_STRING_I64: case XR_RBC_STRING_F64:
        case XR_RBC_STRING_RETAIN: case XR_RBC_STRING_CONCAT: case XR_RBC_STRING_TAKE:
        case XR_RBC_STRING_DROP: case XR_RBC_STRING_PRINT: case XR_RBC_STRING_LEN:
            if (!rbc_strings_value(s, v)) return false;
            break;
        case XR_RBC_CELL_NEW: case XR_RBC_CELL_READ: case XR_RBC_CELL_WRITE:
        case XR_RBC_STRING_CELL_NEW: case XR_RBC_STRING_CELL_DROP:
            if (!rbc_strings_cell(s, v)) return false;
            break;
        case XR_RBC_CALL: case XR_RBC_CALLN:
            if (!rbc_strings_call(s, v)) return false;
            break;
        case XR_RBC_RET: case XR_RBC_RET0:
            if (rbc_managed_kind(f->result_kind)) {
                if (v.op != XR_RBC_RET || s->state[v.a] != RBC_STRING_OWNED ||
                    rbc_register_kind(f, v.a) != f->result_kind) return false;
                s->state[v.a] = RBC_EMPTY;
            } else if (f->result_kind == XR_RBC_SLICE_I64) {
                if (v.op != XR_RBC_RET || s->state[v.a] != RBC_VIEW_RETURNED ||
                    rbc_register_kind(f, v.a) != XR_RBC_SLICE_I64) return false;
                s->state[v.a] = RBC_EMPTY;
            } else if (v.op == XR_RBC_RET && s->state[v.a] != RBC_BITS) return false;
            for (uint32_t r = 0; r < f->register_count; ++r)
                if (s->state[r] == RBC_STRING_OWNED || s->state[r] == RBC_CELL_OWNED ||
                    s->state[r] == RBC_VIEW_FRAME || s->state[r] == RBC_VIEW_CELL ||
                    s->state[r] == RBC_VIEW_SLICE || s->state[r] == RBC_VIEW_RETURNED) return false;
            break;
        case XR_RBC_LOADK: case XR_RBC_LOADI:
            if (!rbc_bits_destination(s, v.a)) return false;
            s->state[v.a] = RBC_BITS; break;
        case XR_RBC_PRINT_LITERAL: break;
        case XR_RBC_NEG: case XR_RBC_FNEG:
        case XR_RBC_ADDI: case XR_RBC_SUBI: case XR_RBC_MULI: case XR_RBC_REMI:
        case XR_RBC_FLEK:
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
    RbcSpillState *spills = to->spills;
    uint8_t *aggregates = to->aggregates;
    *to = *from;
    to->spills = spills;
    to->aggregates = aggregates;
    if (from->function->spill_count)
        memcpy(to->spills,from->spills,(size_t)from->function->spill_count * sizeof(*from->spills));
    if (from->function->aggregate_value_count)
        memcpy(to->aggregates, from->aggregates, from->function->aggregate_value_count);
}
static bool rbc_string_merge(RbcStringAdmission *to, const RbcStringAdmission *from,
    bool *seen, bool *changed) {
    if (!*seen) {
        rbc_string_state_copy(to,from);
        *seen = *changed = true;
        return true;
    }
    if (to->published != from->published) return false;
    for (size_t n = 0; n < from->function->aggregate_value_count; ++n) {
        uint8_t initialized = to->aggregates[n] & from->aggregates[n];
        if (initialized != to->aggregates[n]) { to->aggregates[n] = initialized; *changed = true; }
    }
    for (uint32_t r = 0; r < from->function->register_count; ++r) {
        /* Borrowed capabilities own no value to drop. A missing producer on
         * either path removes the guarantee until a fresh producer executes. */
        XrRbcRegisterKind kind = rbc_register_kind(from->function, r);
        if (kind == XR_RBC_FUNCTION || xr_rbc_view_kind(kind) || kind == XR_RBC_STATE ||
            kind == XR_RBC_STATE_REF || kind == XR_RBC_STRING_REF ||
            xr_rbc_array_ref_family(kind) < XR_RBC_ARRAY_FAMILY_COUNT) {
            if (to->state[r] != from->state[r] ||
                (kind != XR_RBC_STATE && to->origin[r] != from->origin[r])) {
                if (to->state[r] != RBC_EMPTY || to->origin[r]) {
                    to->state[r] = RBC_EMPTY;
                    to->origin[r] = 0;
                    *changed = true;
                }
            }
            continue;
        }
        if (to->state[r] != from->state[r]) {
            if ((to->state[r] != RBC_BITS && to->state[r] != RBC_EMPTY) ||
                (from->state[r] != RBC_BITS && from->state[r] != RBC_EMPTY)) return false;
            if (to->state[r] != RBC_EMPTY) {
                to->state[r] = RBC_EMPTY;
                *changed = true;
            }
        }
        if ((to->state[r] == RBC_STRING_BORROWED || rbc_cell_state(to->state[r]) ||
             to->state[r] == RBC_FUNCTION || to->state[r] == RBC_STATE_REFERENCE || rbc_array_ref_state(to->state[r])) &&
            to->origin[r] != from->origin[r]) return false;
    }
    for (uint32_t i=0; i<from->function->spill_count; ++i) {
        if (i >= from->function->reference_spill_count &&
            (to->spills[i].state == RBC_STATE_BORROWED ||
             to->spills[i].state == RBC_STATE_REFERENCE ||
             from->spills[i].state == RBC_STATE_BORROWED ||
             from->spills[i].state == RBC_STATE_REFERENCE)) {
            if (to->spills[i].state != from->spills[i].state ||
                to->spills[i].origin != from->spills[i].origin) {
                if (to->spills[i].state != RBC_EMPTY || to->spills[i].origin) {
                    to->spills[i] = (RbcSpillState){0, RBC_EMPTY};
                    *changed = true;
                }
            }
            continue;
        }
        if (to->spills[i].state==from->spills[i].state) {
            if (to->spills[i].state == RBC_STATE_REFERENCE &&
                to->spills[i].origin != from->spills[i].origin) return false;
            continue;
        }
        if (i<from->function->reference_spill_count ||
            (to->spills[i].state!=RBC_BITS && to->spills[i].state!=RBC_EMPTY) ||
            (from->spills[i].state!=RBC_BITS && from->spills[i].state!=RBC_EMPTY)) return false;
        if (to->spills[i].state != RBC_EMPTY) {
            to->spills[i].state = RBC_EMPTY;
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
/* String and Array owners cross a backedge through the same exact IN-state
 * merge as every predecessor. The virtual entry cannot manufacture an owner,
 * and call-window borrows retire before transferring control. */
static bool rbc_string_backedge(const RbcStringAdmission *from) {
    for (uint32_t r = 0; r < from->function->register_count; ++r) {
        XrRbcRegisterKind kind = rbc_register_kind(from->function, r);
        uint8_t now = from->state[r];
        if (kind == XR_RBC_STRING_CELL || rbc_array_cell_kind(kind)) {
            if (now != RBC_EMPTY && (!rbc_cell_state(now) || !from->origin[r])) return false;
        } else if (rbc_array_kind(kind) || kind == XR_RBC_STRING) {
            if (now != RBC_EMPTY && now != RBC_STRING_OWNED &&
                !(r < from->function->parameter_count && now == RBC_STRING_BORROWED)) return false;
        } else if (kind == XR_RBC_CALL_WINDOW) {
            if (now != RBC_EMPTY && now != RBC_BITS) return false;
        }
    }
    for (uint32_t i = 0; i < from->function->reference_spill_count; ++i)
        if (from->spills[i].state != RBC_EMPTY &&
            ((!rbc_array_kind(rbc_reference_spill_kind(from->function,i)) &&
              rbc_reference_spill_kind(from->function,i) != XR_RBC_STRING) ||
             from->spills[i].state != RBC_STRING_OWNED)) return false;
    return true;
}
static bool rbc_spill_transfer(RbcStringAdmission *s, XrRbcOperands v) {
    uint32_t index = (uint32_t)v.immediate;
    bool string = index < s->function->reference_spill_count;
    XrRbcRegisterKind kind = rbc_register_kind(s->function, v.a);
    uint8_t initialized = string ? RBC_STRING_OWNED : kind == XR_RBC_STATE ? RBC_STATE_BORROWED :
        kind == XR_RBC_STATE_REF ? RBC_STATE_REFERENCE : RBC_BITS;
    if (string ? kind != rbc_reference_spill_kind(s->function, index) :
        (kind != XR_RBC_BITS && kind != XR_RBC_STATE && kind != XR_RBC_STATE_REF)) return false;
    if (v.op == XR_RBC_SPILL) {
        if (s->state[v.a] != initialized ||
            (string && s->spills[index].state != RBC_EMPTY)) return false;
        s->spills[index] = (RbcSpillState){s->origin[v.a], initialized};
        s->state[v.a] = RBC_EMPTY;
        if (string) rbc_string_forget_borrows(s, v.a);
    } else {
        if (s->spills[index].state != initialized ||
            (string && s->state[v.a] != RBC_EMPTY)) return false;
        s->state[v.a] = initialized;
        s->origin[v.a] = s->spills[index].origin;
        if (string) s->spills[index].state = RBC_EMPTY;
    }
    return true;
}
static void rbc_string_graph_free(RbcStringGraph *g) {
    xr_free(g->aggregates);
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
        } else if (op == XR_RBC_LT || op == XR_RBC_LE || op == XR_RBC_EQ ||
            (op >= XR_RBC_LTI && op <= XR_RBC_EQI)) {
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
    if (rows <= g->count || (f->spill_count && rows > SIZE_MAX / sizeof(*g->spills) / f->spill_count))
        return XR_RBC_OUT_OF_MEMORY;
    if (f->spill_count) {
        g->spills = xr_malloc(rows * f->spill_count * sizeof(*g->spills));
        if (!g->spills) return XR_RBC_OUT_OF_MEMORY;
        for (size_t i = 0; i < rows * f->spill_count; ++i)
            g->spills[i] = (RbcSpillState){0, RBC_EMPTY};
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
    if (f->aggregate_value_count) {
        if (rows > SIZE_MAX / f->aggregate_value_count) return XR_RBC_OUT_OF_MEMORY;
        g->aggregates = xr_calloc(rows, f->aggregate_value_count);
        if (!g->aggregates) return XR_RBC_OUT_OF_MEMORY;
        g->work.aggregates = g->aggregates;
        for (size_t b = 0; b < g->count; ++b)
            g->incoming[b].state.aggregates = g->aggregates + (b + 1) * f->aggregate_value_count;
        for (size_t n = 0; n < f->aggregate_parameter_count; ++n)
            g->work.aggregates[f->aggregate_parameters[n]] = 1;
    }
    for (uint32_t r = 0; r < f->register_count; ++r) {
        XrRbcRegisterKind kind = rbc_register_kind(f,r);
        g->work.state[r] = kind == XR_RBC_BITS &&
            (!g->parameters_only || r<f->parameter_count) ? RBC_BITS : RBC_EMPTY;
        if (r < f->parameter_count && kind == XR_RBC_FUNCTION) {
            g->work.state[r] = RBC_FUNCTION;
            g->work.origin[r] = f->register_callables[r];
        }
        if (r < f->parameter_count && rbc_managed_kind(kind)) g->work.state[r] = RBC_STRING_BORROWED;
        if (r < f->parameter_count && kind == XR_RBC_STATE) g->work.state[r] = RBC_STATE_BORROWED;
        if (r < f->parameter_count && xr_rbc_view_kind(kind)) g->work.state[r] = rbc_view_state(kind);
        if (r < f->parameter_count && (kind == XR_RBC_STRING_REF || xr_rbc_array_ref_family(kind) < XR_RBC_ARRAY_FAMILY_COUNT)) {
            g->work.state[r] = RBC_ARRAY_REF_FORMAL;
            if (!rbc_cell_parameter_origin(r, &g->work.origin[r])) return XR_RBC_INVALID;
        }
        if (r < f->parameter_count && (kind == XR_RBC_STRING_CELL || rbc_array_cell_kind(kind))) {
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
        } else if (v.op >= XR_RBC_LTI && v.op <= XR_RBC_EQI) {
            return g->work.state[v.a] == RBC_BITS &&
                rbc_string_propagate(g, pc + 1, false) && rbc_string_propagate(g, pc + 2, false);
        } else if (v.op == XR_RBC_JMP) {
            size_t target;
            return rbc_string_jump(f, pc, &target) && rbc_string_propagate(g, target, target <= pc);
        } else {
            bool valid = v.op==XR_RBC_SPILL || v.op==XR_RBC_RELOAD ?
                rbc_spill_transfer(&g->work,v) : rbc_string_step(&g->work,v);
            if (!valid) return false;
            if (v.op == XR_RBC_RET || v.op == XR_RBC_RET0 || rbc_aggregate_return_word(f, f->code[pc])) {
                for (uint32_t i=0; i<f->reference_spill_count; ++i)
                    if (g->work.spills[i].state!=RBC_EMPTY) return false;
                return true;
            }
        }
    }
    return block->end < f->code_count && rbc_string_propagate(g, block->end, false);
}

static XrRbcStatus xr_rbc_strings_valid(const XrRbcModule *module, const XrRbcFunction *f) {
    bool managed = rbc_managed_kind(f->result_kind) || f->array_access_count != 0 || f->view_access_count != 0;
    bool range = false;
    for (size_t n = 0; n < f->aggregate_access_count; ++n)
        range |= f->aggregate_accesses[n].operation == XR_RBC_RANGE_CHECK;
    bool numeric_conversion = false;
    for (uint32_t r = 0; f->register_kinds && r < f->register_count; ++r)
        managed |= f->register_kinds[r] == XR_RBC_STRING || f->register_kinds[r] == XR_RBC_STRING_CELL ||
            f->register_kinds[r] == XR_RBC_STRING_REF || f->register_kinds[r] == XR_RBC_STATE || f->register_kinds[r] == XR_RBC_STATE_REF ||
            f->register_kinds[r] == XR_RBC_FUNCTION || xr_rbc_view_kind(f->register_kinds[r]) ||
            rbc_array_kind(f->register_kinds[r]) || rbc_array_cell_kind(f->register_kinds[r]) ||
            xr_rbc_array_ref_family(f->register_kinds[r]) < XR_RBC_ARRAY_FAMILY_COUNT;
    for (size_t pc = 0; pc < f->code_count; ++pc) {
        uint32_t word = f->code[pc], op = word & 255;
        numeric_conversion |= op == XR_RBC_ITOF || op == XR_RBC_FTOI;
        managed |= op >= XR_RBC_STRING_LITERAL && op <= XR_RBC_STRING_CELL_DROP;
        managed |= op == XR_RBC_STRING_LEN || op == XR_RBC_STRING_F64;
        if (op == XR_RBC_CALL) {
            const XrRbcFunction *callee = &module->functions[word >> 16];
            managed |= rbc_managed_kind(callee->result_kind);
            for (uint32_t p = 0; p < callee->parameter_count; ++p)
                managed |= rbc_register_kind(callee, p) != XR_RBC_BITS;
        }
    }
    if (!managed && !range && !f->spill_count && !numeric_conversion && !f->aggregate_value_count) return XR_RBC_OK;
    if (!f->register_kinds && (managed || f->reference_spill_count)) return XR_RBC_INVALID;
    RbcStringGraph g = {.work = {.module = module, .function = f},
        .head = UINT32_MAX, .tail = UINT32_MAX,
        .parameters_only = range || numeric_conversion || f->spill_count != 0 || f->aggregate_value_count != 0};
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
