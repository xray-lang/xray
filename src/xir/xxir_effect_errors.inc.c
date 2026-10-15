/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_errors.inc.c - Bounded escaping-error flow on verified CFGs
 *
 * KEY CONCEPT:
 *   Known enum variants and unknown errors are independent facts. Only a
 *   reachable error edge can contribute to an escaping summary. PHIs observe
 *   simultaneous predecessor values; copied immutable errors share filters.
 */
static bool error_bit(const uint64_t *set, uint32_t bit) {
    return (set[bit / 64] & ((uint64_t)1 << (bit % 64))) != 0;
}
static void error_add(uint64_t *set, uint32_t bit) { set[bit / 64] |= (uint64_t)1 << (bit % 64); }
bool xr_xir_effects_error(const XrXirEffects *effects, uint32_t f, XrXirType type, uint32_t variant) {
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    if (!final || f>=final->count) return false;
    if ((uint32_t)type >= XR_XIR_CONSTRUCTED_TYPE_BASE && (uint32_t)type < XR_XIR_CONSTRUCTED_TYPE_LIMIT &&
        (uint32_t)type - XR_XIR_CONSTRUCTED_TYPE_BASE >= final->source_type_count) return false;
    for (uint32_t a = 0; a < final->atom_count; ++a)
        if (final->atoms[a].type == type && final->atoms[a].variant == variant)
            return error_bit(final->errors + (size_t)f * final->words, a + 2);
    return false;
}
bool xr_xir_effects_error_unknown(const XrXirEffects *effects, uint32_t f) {
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    return final && f<final->count && error_bit(final->errors + (size_t)f * final->words, 0);
}
bool xr_xir_effects_error_unidentified(const XrXirEffects *effects, uint32_t f) {
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    return final && f<final->count && error_bit(final->errors + (size_t)f * final->words, 1);
}
typedef struct ErrorInvocationContexts ErrorInvocationContexts;
static uint32_t error_invocation_summary(const ErrorInvocationContexts *contexts,uint32_t n);
enum { ERROR_BLOCK_REACHABLE = 1, ERROR_BLOCK_PENDING = 2 };
/* Typed Cell rows are private to one function interpretation. They are
 * rebuilt after every function or atom-width restart from the current body. */
typedef struct ErrorCellType {
    uint32_t value;
    XrXirType type;
} ErrorCellType;
typedef struct ErrorFlow {
    EffectBodyRequest body;
    EffectBodyFunctionHead head;
    const EffectBodyFunctionHead *function;
    uint32_t function_index,instruction;
    XrXirEffects *effects;
    XrXirCompileContext *remaining;
    EffectTerms *terms;
    uint64_t *states, *work, *edge, *snapshot, *escaping;
    uint8_t *reachable;
    uint32_t *queue,front,back,pending;
    bool scheduling;
    uint32_t *roots, *cells, *slots, *active;
    ErrorCellType *cell_types;
    const EffectBodyMaterial *cell_material;
    uint32_t cell_function,cell_type_count;
    const uint64_t *zero;
    uint8_t *storage;
    size_t storage_capacity;
    size_t stride;
    uint32_t values, cell_count, active_count;
    ErrorInvocationContexts *invocations;
    uint32_t invocation,summary_count;
    bool changed, summary_changed, restart;
} ErrorFlow;
#include "xxir_effect_error_cfg.inc.c"
static XrXirStatus error_invocation_contexts_new(ErrorFlow *flow,ErrorInvocationContexts **output);
static void error_invocation_contexts_free(ErrorInvocationContexts *contexts);
static XrXirStatus error_invocation_functions(ErrorFlow *flow);
static XrXirStatus error_invocation_summaries(ErrorFlow *flow);
static XrXirStatus error_invocation_call(ErrorFlow *flow,const XrXirInstruction *call,
    uint64_t *set,bool *handled);
/* Execution reads consume the sole immutable body. External raw admission
 * and source correspondence remain with their original public callers. */
static XrXirStatus error_type_at(const ErrorFlow *flow,uint32_t value,XrXirType *output) {
    if (!flow) return XR_XIR_BAD_STRUCTURE;
    return effect_body_request_type(&flow->body,flow->function_index,value,output);
}
static XrXirStatus error_operation(const ErrorFlow *flow,uint32_t function,uint32_t instruction,
    XrXirInstruction *storage,const XrXirInstruction **output) {
    if (!flow) return XR_XIR_BAD_STRUCTURE;
    return effect_body_request_instruction(&flow->body,function,instruction,storage,output);
}
static XrXirStatus error_shape(const ErrorFlow *flow,uint32_t function,uint32_t instruction,
    EffectBodyInstructionShape *output) {
    if (!flow) return XR_XIR_BAD_STRUCTURE;
    return effect_body_request_shape(&flow->body,function,instruction,output);
}
/* Missing scalar rows are read-only zero facts, never writable aliases. */
static uint64_t *error_value(ErrorFlow *flow, uint64_t *row, uint32_t value) {
    if (value >= flow->values || flow->slots[value] == UINT32_MAX) return NULL;
    return row + (size_t)flow->slots[value] * flow->effects->outcomes_work.words;
}
static const uint64_t *error_read(const ErrorFlow *flow, const uint64_t *row, uint32_t value) {
    if (value >= flow->values) return NULL;
    return flow->slots[value] == UINT32_MAX ? flow->zero :
        row + (size_t)flow->slots[value] * flow->effects->outcomes_work.words;
}
/* Complete type verification precedes inference. Closed outer containers are
 * not Error atoms, even when an extracted payload has its own error facts. */
static bool error_closed_node_shape(const XrXirTypeNode *node) {
    if (node->kind != XR_XIR_TYPE_NOMINAL &&
        (node->nominal.declaration || node->nominal.arguments || node->nominal.argument_count ||
         node->nominal.fields || node->nominal.field_count)) return false;
    switch (node->kind) {
    case XR_XIR_TYPE_CALLABLE:
        return node->element == XR_XIR_UNIT && xr_xir_callable_flags_valid(node->flags) &&
            ((node->parameter_count != 0) == (node->parameters != NULL));
    case XR_XIR_TYPE_TUPLE:
        return node->element == XR_XIR_UNIT && node->result == XR_XIR_UNIT && !node->flags &&
            node->parameter_count && node->parameters;
    case XR_XIR_TYPE_ARRAY: case XR_XIR_TYPE_NULLABLE:
        return !node->parameters && !node->parameter_count && node->result == XR_XIR_UNIT && !node->flags;
    case XR_XIR_TYPE_ATOMIC:
        return !node->parameters && !node->parameter_count && node->result == XR_XIR_UNIT && !node->flags &&
            (node->element == XR_XIR_I64 || node->element == XR_XIR_F64 || node->element == XR_XIR_BOOL);
    case XR_XIR_TYPE_NOMINAL:
        return node->element == XR_XIR_UNIT && !node->parameters && !node->parameter_count &&
            node->result == XR_XIR_UNIT && !node->flags &&
            ((node->nominal.argument_count != 0) == (node->nominal.arguments != NULL)) &&
            ((node->nominal.field_count != 0) == (node->nominal.fields != NULL));
    default: return false;
    }
}
static bool error_copy_op(XrXirOp op) {
    return op == XR_XIR_COPY || op == XR_XIR_SCALAR_COPY || op == XR_XIR_OWNED_RETAIN ||
        op == XR_XIR_ERROR_ERASE || op == XR_XIR_ERROR_NARROW;
}
enum { ERROR_ROW_ACTIVE = 1, ERROR_ROW_CELL = 2, ERROR_ROW_COPY = 4 };
typedef struct ErrorMembership {
    uint8_t *rows;
    uint32_t *sources;
    XrXirType *cell_types;
    uint32_t zero_copy_count;
} ErrorMembership;
/* One fresh shape read supplies membership and alias classification. These
 * results describe storage, never execution authority or declaration proof. */
static XrXirStatus error_type_active(const ErrorFlow *flow,XrXirType type,bool *active) {
    if (!flow || !active) return XR_XIR_BAD_STRUCTURE;
    switch (type) {
    case XR_XIR_UNIT: case XR_XIR_BOOL: case XR_XIR_I64: case XR_XIR_STRING:
    case XR_XIR_I8: case XR_XIR_I16: case XR_XIR_I32: case XR_XIR_U8:
    case XR_XIR_U16: case XR_XIR_U32: case XR_XIR_U64: case XR_XIR_F32:
    case XR_XIR_F64: case XR_XIR_PANIC_INFO: case XR_XIR_RUNE:
        *active = false;return XR_XIR_OK;
    default: break;
    }
    const XrXirTypes *types = flow->body.domain.types;
    uint32_t id = (uint32_t)type;
    if (!types || !types->nodes || id < XR_XIR_CONSTRUCTED_TYPE_BASE ||
        id >= XR_XIR_CONSTRUCTED_TYPE_LIMIT) { *active = true;return XR_XIR_OK; }
    if (!xir_compile_work(flow->remaining, 1)) return XR_XIR_BUDGET;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    if (!node || node->parameter_span || !error_closed_node_shape(node)) {
        *active = true;return XR_XIR_OK;
    }
    if (node->kind != XR_XIR_TYPE_NOMINAL) { *active = false;return XR_XIR_OK; }
    const XrXirNominalTable *table = types->nominals;
    if (!table || node->nominal.declaration >= table->count ||
        (table->declarations != NULL) == (table->identities != NULL)) {
        *active = true;return XR_XIR_OK;
    }
    if (!xir_compile_work(flow->remaining, 1)) return XR_XIR_BUDGET;
    uint32_t d = node->nominal.declaration;
    uint32_t kind = table->declarations ? table->declarations[d].kind : table->identities[d].kind;
    *active = kind != XR_XIR_NOMINAL_STRUCT && kind != XR_XIR_NOMINAL_CLASS;
    return XR_XIR_OK;
}

static XrXirStatus error_row_active(const ErrorFlow *flow, uint32_t value, bool *active,
    XrXirType *type_output, bool *copy_output, uint32_t *source_output) {
    if (!flow || !flow->function || !active || !type_output || !copy_output || !source_output ||
        (uint64_t)value >= (uint64_t)flow->function->parameter_count + flow->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirType type=XR_XIR_UNIT;bool copy=false;uint32_t source=UINT32_MAX;
    if (value >= flow->function->parameter_count) {
        XrXirInstruction storage;const XrXirInstruction *actual=NULL;
        EffectBodyInstructionFields fields;
        XrXirStatus status=effect_body_request_fields(&flow->body,flow->function_index,
            value-flow->function->parameter_count,&storage,&actual,&fields);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
        XrXirOp op=actual?actual->op:fields.op;type=actual?actual->type:fields.type;
        if (op<=XR_XIR_INVALID || op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        copy=error_copy_op(op);if (copy) source=actual?actual->args[0]:fields.source;
        if (op == XR_XIR_GO || op == XR_XIR_INVOKE_ERROR) {
            *active = true;*type_output=type;*copy_output=copy;*source_output=source;return XR_XIR_OK;
        }
    } else {
        if (!flow->function->parameters) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus read=error_type_at(flow,value,&type);
        if (read!=XR_XIR_OK) return read;
    }
    XrXirStatus status=error_type_active(flow,type,active);
    if (status!=XR_XIR_OK) return status;
    *type_output=type;*copy_output=copy;*source_output=source;return XR_XIR_OK;
}
/* A range reference belongs only to the enclosing membership or CFG walk.
 * Storage identity never supplies Error, ROOT or Cell authority. */
static XrXirStatus error_owned_function(const ErrorFlow *flow,
    const EffectBodyFunction **output) {
    if (!flow || !output || *output || flow->body.instruction!=effect_body_owned_instruction ||
        flow->body.head!=effect_body_owned_head || flow->body.type!=effect_body_owned_type ||
        flow->body.shape!=effect_body_owned_shape)
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyMaterial *body=flow->body.owner;
    if (!xir_compile_context_valid(flow->remaining) || !effect_body_material_shape(body) ||
        body->resources!=flow->remaining->resources || flow->body.read.owner!=flow->remaining ||
        flow->body.read.consume!=effect_body_compile_consume ||
        flow->body.domain.types!=body->domain.types ||
        flow->body.domain.declarations!=body->domain.declarations ||
        flow->body.domain.generics!=body->domain.generics ||
        flow->body.domain.defaults!=body->domain.defaults ||
        flow->body.domain.function_count!=effect_body_count(body) ||
        flow->function_index>=effect_body_count(body)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->remaining,sizeof(*output))) return XR_XIR_BUDGET;
    *output=&body->functions[flow->function_index];return XR_XIR_OK;
}

/* All four canonical callbacks select an owned representation. This is not
 * executable authority; the full current domain and ledger are checked too. */
static bool error_owned_reader(const ErrorFlow *flow) {
    return flow && flow->body.instruction==effect_body_owned_instruction &&
        flow->body.head==effect_body_owned_head && flow->body.type==effect_body_owned_type &&
        flow->body.shape==effect_body_owned_shape;
}
/* The inventory has no lifetime beyond this ErrorFlow function. Its source
 * is the already admitted immutable body, never a previous query or summary. */
static XrXirStatus error_cell_types_prepare(ErrorFlow *flow,const XrXirType *types) {
    if (!error_owned_reader(flow) || !flow->cell_count) return XR_XIR_OK;
    const EffectBodyFunction *function=NULL;
    XrXirStatus status=error_owned_function(flow,&function);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(flow->remaining,sizeof(function)+7)) return XR_XIR_BUDGET;
    if (!types || !flow->cell_types || function->parameter_count!=flow->function->parameter_count ||
        function->instruction_count!=flow->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t c=0;c<flow->cell_count;++c) {
        if (!xir_compile_work(flow->remaining,sizeof(uint32_t)+sizeof(XrXirType)+4)) return XR_XIR_BUDGET;
        uint32_t value=flow->cells[c];
        if (value>=flow->values) return XR_XIR_BAD_STRUCTURE;
        XrXirType type=types[value];
        if (!xr_xir_type_is_cell(flow->body.domain.types,type)) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(flow->remaining,sizeof(*flow->cell_types))) return XR_XIR_BUDGET;
        flow->cell_types[c]=(ErrorCellType){value,type};
    }
    if (!xir_compile_work(flow->remaining,sizeof(flow->cell_material)+
        sizeof(flow->cell_function)+sizeof(flow->cell_type_count))) return XR_XIR_BUDGET;
    flow->cell_material=flow->body.owner;flow->cell_function=flow->function_index;
    flow->cell_type_count=flow->cell_count;return XR_XIR_OK;
}
/* An unchanged type row avoids repeated stream-prefix decoding. Every
 * invalidation still clears and restores the original mutable error facts. */
static XrXirStatus error_cell_types_current(const ErrorFlow *flow) {
    const EffectBodyFunction *function=NULL;
    XrXirStatus status=error_owned_function(flow,&function);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(flow->remaining,sizeof(function)+15)) return XR_XIR_BUDGET;
    if (!flow->cells || !flow->cell_types || flow->cell_types!=(ErrorCellType *)(flow->cells+flow->cell_count) ||
        flow->cell_material!=flow->body.owner ||
        flow->cell_function!=flow->function_index || flow->cell_type_count!=flow->cell_count ||
        function->parameter_count!=flow->function->parameter_count ||
        function->instruction_count!=flow->function->instruction_count ||
        function->parameters!=flow->function->parameters ||
        (uint64_t)flow->values!=(uint64_t)function->parameter_count+function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

/* Membership consumes opcode and physical type; only a real COPY reads its
 * source. Canonical encoding and external ten-word correspondence are kept
 * by their original complete readers, independent of this projection. */
static XrXirStatus error_range_fields(const EffectBodyRead *read,
    const EffectBodyRun *ref,uint32_t instruction,EffectBodyInstructionFields *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,3*sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t opcode=XR_XIR_INVALID,type=XR_XIR_UNIT,source=UINT32_MAX;
    XrXirStatus status=effect_body_run_word(read,ref,instruction,0,&opcode);
    if (status==XR_XIR_OK) status=effect_body_run_word(read,ref,instruction,1,&type);
    if (status!=XR_XIR_OK) return status;
    if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    if (error_copy_op((XrXirOp)opcode)) {
        status=effect_body_run_word(read,ref,instruction,2,&source);
        if (status!=XR_XIR_OK) return status;
    }
    if (!effect_body_read_work(read,sizeof(*output)+2)) return XR_XIR_BUDGET;
    *output=(EffectBodyInstructionFields){(XrXirOp)opcode,(XrXirType)type,source};return XR_XIR_OK;
}

static XrXirStatus error_membership_row(ErrorFlow *flow,uint32_t value,
    const EffectBodyInstructionFields *fields,ErrorMembership *membership) {
    if (!xir_compile_work(flow->remaining,2+(fields?1:0))) return XR_XIR_BUDGET;
    bool active=true,copy=false;XrXirType type=XR_XIR_UNIT;uint32_t source=UINT32_MAX;
    XrXirStatus status=XR_XIR_OK;
    if (fields) {
        if (value<flow->function->parameter_count || !xir_compile_work(flow->remaining,1))
            return value<flow->function->parameter_count?XR_XIR_BAD_STRUCTURE:XR_XIR_BUDGET;
        type=fields->type;copy=error_copy_op(fields->op);if (copy) source=fields->source;
        if (fields->op!=XR_XIR_GO && fields->op!=XR_XIR_INVOKE_ERROR)
            status=error_type_active(flow,type,&active);
    } else status=error_row_active(flow,value,&active,&type,&copy,&source);
    if (status!=XR_XIR_OK) return status;
    bool cell=xr_xir_type_is_cell(flow->body.domain.types,type);
    membership->rows[value]=(uint8_t)((active?ERROR_ROW_ACTIVE:0) |
        (cell?ERROR_ROW_CELL:0) | (copy?ERROR_ROW_COPY:0));
    if (active) ++flow->active_count;
    if (cell) {
        ++flow->cell_count;
        if (membership->cell_types) {
            if (!xir_compile_work(flow->remaining,sizeof(type))) return XR_XIR_BUDGET;
            membership->cell_types[value]=type;
        }
    }
    if (fields && copy && !active) ++membership->zero_copy_count;
    if (copy) {
        if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
        membership->sources[value]=source;
    }
    return XR_XIR_OK;
}

static XrXirStatus error_membership(ErrorFlow *flow,ErrorMembership *membership) {
    bool owned=flow->body.instruction==effect_body_owned_instruction;
    uint32_t prefix=owned?flow->function->parameter_count:flow->values;
    for (uint32_t value=0;value<prefix;++value) {
        XrXirStatus status=error_membership_row(flow,value,NULL,membership);
        if (status!=XR_XIR_OK) return status;
    }
    if (!owned) return XR_XIR_OK;
    const EffectBodyFunction *function=NULL;
    XrXirStatus status=error_owned_function(flow,&function);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(flow->remaining,sizeof(EffectBodyRecordCursor))) return XR_XIR_BUDGET;
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    if (!xir_compile_work(flow->remaining,sizeof(uint32_t))) return XR_XIR_BUDGET;
    for (uint32_t run=0;run<function->run_count;++run) {
        if (!xir_compile_work(flow->remaining,1+sizeof(run))) return XR_XIR_BUDGET;
        const EffectBodyRun *ref;
        status=effect_body_range_ref(&flow->body.read,function,&cursor,&ref);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t i=ref->first;i-ref->first<ref->count;++i) {
            EffectBodyInstructionFields fields;
            status=error_range_fields(&flow->body.read,ref,i,&fields);
            if (status==XR_XIR_OK) status=error_membership_row(flow,
                flow->function->parameter_count+i,&fields,membership);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return cursor.first==function->instruction_count && cursor.run==function->run_count?
        XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static bool error_any(const uint64_t *set, uint32_t words) {
    for (uint32_t w = 0; w < words; ++w) if (set[w]) return true;
    return false;
}
static bool error_join(uint64_t *to, const uint64_t *from, size_t words) {
    bool changed = false;
    for (size_t w = 0; w < words; ++w) {
        uint64_t joined = to[w] | from[w]; changed |= joined != to[w]; to[w] = joined;
    }
    return changed;
}
static void error_type(ErrorFlow *flow, XrXirType type, uint64_t *set) {
    if (xr_xir_type_is_cell(flow->body.domain.types, type)) type = xr_xir_cell_element(flow->body.domain.types, type);
    const XrXirTypeNode *node = xr_xir_type_node(flow->body.domain.types, type);
    if (type == XR_XIR_ERROR || (node && node->kind == XR_XIR_TYPE_TASK)) error_add(set, 1);
    else for (uint32_t a = 0; a < flow->effects->outcomes_work.atom_count; ++a)
        if (flow->effects->outcomes_work.atoms[a].type == type) error_add(set, a + 2);
}
static XrXirStatus error_definition(ErrorFlow *flow,uint32_t value,
    XrXirInstruction *storage,const XrXirInstruction **output) {
    if (!flow || !flow->function || !storage || !output || *output || value>=flow->values)
        return XR_XIR_BAD_STRUCTURE;
    if (value<flow->function->parameter_count) return XR_XIR_OK;
    return error_operation(flow,flow->function_index,value-flow->function->parameter_count,storage,output);
}
/* Completed roots are local derived values, not callable or Cell authority.
 * Forward COPY edges retain the original finite per-visit watchdog. */
static XrXirStatus error_roots(ErrorFlow *flow,const uint8_t *membership,
    const uint32_t *sources) {
    for (uint32_t v=0;v<flow->values;++v) {
        if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
        uint32_t root=v;
        while (membership[root]&ERROR_ROW_COPY) {
            uint32_t target=sources[root];
            if (target>=flow->values) return XR_XIR_BAD_STRUCTURE;
            if (target<v) { root=flow->roots[target];break; }
            root=target;
            if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
        }
        flow->roots[v]=root;
    }
    return XR_XIR_OK;
}
static XrXirStatus error_atom(ErrorFlow *flow, EffectErrorAtom atom, uint64_t *set) {
    XrXirStatus allocation_status = XR_XIR_OK;
    XrXirEffects *effects = flow->effects;
    for (uint32_t a = 0; a < effects->outcomes_work.atom_count; ++a) {
        if (!xir_compile_work(flow->remaining, 1)) return XR_XIR_BUDGET;
        if (effects->outcomes_work.atoms[a].type == atom.type && effects->outcomes_work.atoms[a].variant == atom.variant) {
            error_add(set, a + 2); return XR_XIR_OK;
        }
    }
    if (effects->outcomes_work.atom_count == UINT32_MAX - 2u) return XR_XIR_BUDGET;
    if (effects->outcomes_work.atom_count == effects->outcomes_work.atom_capacity) {
        uint64_t capacity = effects->outcomes_work.atom_capacity ? (uint64_t)effects->outcomes_work.atom_capacity * 2 : 8;
        if (capacity > UINT32_MAX - 2u) capacity = UINT32_MAX - 2u;
        uint64_t bytes = capacity * sizeof(EffectErrorAtom);
        if (bytes > SIZE_MAX ||
            !xir_compile_work(flow->remaining, capacity + effects->outcomes_work.atom_count)) return XR_XIR_BUDGET;
        EffectErrorAtom *atoms = xir_compile_calloc(flow->remaining, (size_t)capacity, sizeof(*atoms), &allocation_status);
        if (!atoms) return allocation_status;

        if (effects->outcomes_work.atom_count) { if (!xir_compile_work(flow->remaining, (size_t)effects->outcomes_work.atom_count * sizeof(*atoms))) { xr_compile_resources_free(atoms); return XR_XIR_BUDGET; } memcpy(atoms, effects->outcomes_work.atoms, (size_t)effects->outcomes_work.atom_count * sizeof(*atoms)); }
        xr_compile_resources_free(effects->outcomes_work.atoms); effects->outcomes_work.atoms = atoms; effects->outcomes_work.atom_capacity = (uint32_t)capacity;
    }
    effects->outcomes_work.atoms[effects->outcomes_work.atom_count++] = atom;
    flow->restart = true;
    return XR_XIR_OK;
}
static XrXirStatus error_call_summary(ErrorFlow *flow, const XrXirInstruction *call,
    const uint64_t *from,uint64_t *set) {
    uint32_t caller=flow->function_index;
    if (error_bit(from, 0)) error_add(set, 0);
    if (error_bit(from, 1)) error_add(set, 1);
    for (uint32_t a = 0; a < flow->effects->outcomes_work.atom_count; ++a) if (error_bit(from, a + 2)) {
        EffectErrorAtom atom = flow->effects->outcomes_work.atoms[a];
        if (!xir_compile_work(flow->remaining, (uint64_t)flow->effects->outcomes_work.atom_count + 1)) return XR_XIR_BUDGET;
        if (atom.variant == XR_XIR_ERROR_SYMBOLIC_VARIANT) {
            uint32_t p = (uint32_t)atom.type - XR_XIR_TYPE_PARAMETER_BASE;
            if (p >= call->type_arguments[1]) return XR_XIR_BAD_TYPE;
            XrXirType type = flow->body.domain.generics[caller].arguments[call->type_arguments[0] + p];
            error_type(flow, type, set); continue;
        }
        if (call->type_arguments[1]) {
            XrXirGeneric arguments = {0}; arguments.argument_count = call->type_arguments[1];
            arguments.arguments = flow->body.domain.generics[caller].arguments + call->type_arguments[0];
            XrXirStatus status = effect_terms_substitute(flow->terms, atom.type, &arguments, &atom.type);
            if (status != XR_XIR_OK) return status;
        }
        XrXirStatus status = error_atom(flow, atom, set);
        if (status != XR_XIR_OK || flow->restart) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus error_call(ErrorFlow *flow, const XrXirInstruction *call, uint64_t *set) {
    bool handled=false;
    XrXirStatus precise=error_invocation_call(flow,call,set,&handled);
    if (precise!=XR_XIR_OK || handled) return precise;
    if (call->op == XR_XIR_CALL_INDIRECT || call->op == XR_XIR_INVOKE_INDIRECT || call->op == XR_XIR_CALL_REQUIREMENT) {
        error_add(set, 0); return XR_XIR_OK;
    }
    XrXirInstruction resolved;
    if(call->op==XR_XIR_CALL_DEFAULT || call->op==XR_XIR_INVOKE_DEFAULT) {
        const XrXirDefaultBinding *binding=NULL;
        const uint32_t *identity=xr_xir_default_identity(call);
        XrXirStatus status=xir_metadata_default_lookup(flow->remaining,&flow->body.domain, identity[0], identity[1], &binding);
        if(status!=XR_XIR_OK) return status;
        if(!binding) return XR_XIR_BAD_STRUCTURE;
        resolved=*call;resolved.immediate=binding->function;call=&resolved;
    }
    const uint64_t *from = flow->effects->outcomes_work.errors + (size_t)call->immediate * flow->effects->outcomes_work.words;
    return error_call_summary(flow,call,from,set);
}

/* Calls, scheduler/host boundaries and writes through other references can
 * change any aliased cell.
 * Immutable values already read from a cell keep their independent facts. */
static XrXirStatus error_cells(ErrorFlow *flow, uint32_t target, const uint64_t *value) {
    bool owned=error_owned_reader(flow);
    if (owned && flow->cell_count) {
        XrXirStatus status=error_cell_types_current(flow);
        if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t c = 0; c < flow->cell_count; ++c) {
        if (!xir_compile_work(flow->remaining, flow->effects->outcomes_work.words + (uint64_t)flow->effects->outcomes_work.atom_count + 1))
            return XR_XIR_BUDGET;
        uint32_t v = flow->cells[c];
        if (v >= flow->values) return XR_XIR_BAD_STRUCTURE;
        XrXirType type=XR_XIR_UNIT;
        if (owned) {
            if (!xir_compile_work(flow->remaining,sizeof(*flow->cell_types)+2)) return XR_XIR_BUDGET;
            if (flow->cell_types[c].value!=v) return XR_XIR_BAD_STRUCTURE;
            type=flow->cell_types[c].type;
        } else {
            XrXirStatus read=error_type_at(flow,v,&type);
            if (read!=XR_XIR_OK) return read;
        }
        if (!xr_xir_type_is_cell(flow->body.domain.types,type)) return XR_XIR_BAD_STRUCTURE;
        uint64_t *set = error_value(flow,flow->work,v);
        if (!set) return XR_XIR_BAD_STRUCTURE;
        { if (!xir_compile_work(flow->remaining, (size_t)flow->effects->outcomes_work.words*sizeof(uint64_t))) { return XR_XIR_BUDGET; } memset(set,0,(size_t)flow->effects->outcomes_work.words*sizeof(uint64_t)); }
        if (target != UINT32_MAX && flow->roots[v] == flow->roots[target])
            { if (!xir_compile_work(flow->remaining, (size_t)flow->effects->outcomes_work.words*sizeof(uint64_t))) { return XR_XIR_BUDGET; } memcpy(set,value,(size_t)flow->effects->outcomes_work.words*sizeof(uint64_t)); }
        else error_type(flow,type,set);
    }
    return XR_XIR_OK;
}
/* Even a locally proven zero transfer retains the original finite step. */
static XrXirStatus error_instruction_begin(ErrorFlow *flow,uint32_t i) {
    if (!flow || !flow->function || i>=flow->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    flow->instruction=i;
    return xir_compile_work(flow->remaining,(uint64_t)flow->effects->outcomes_work.words+
        flow->effects->outcomes_work.atom_count+1) ? XR_XIR_OK : XR_XIR_BUDGET;
}
static XrXirStatus error_instruction(ErrorFlow *flow, uint32_t i,
    const XrXirInstruction *op) {
    if (!op) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus read=error_instruction_begin(flow,i);
    if (read!=XR_XIR_OK) return read;
    uint32_t words = flow->effects->outcomes_work.words;
    uint64_t *out = error_value(flow, flow->work, flow->function->parameter_count + i);
    if (op->op == XR_XIR_PHI) return XR_XIR_OK;
    if (out) { if (!xir_compile_work(flow->remaining, (size_t)words * sizeof(*out))) { return XR_XIR_BUDGET; } memset(out, 0, (size_t)words * sizeof(*out)); }
    if (op->op == XR_XIR_CELL_WRITE) {
        XrXirType cell=XR_XIR_UNIT;
        read=error_type_at(flow,op->args[0],&cell);
        if (read!=XR_XIR_OK) return read;
        const uint64_t *source=xr_xir_cell_payload_operands(flow->body.domain.types,cell,op->op)==1 ?
            flow->zero : error_read(flow,flow->work,op->args[1]);
        { if (!xir_compile_work(flow->remaining, (size_t)words*sizeof(uint64_t))) { return XR_XIR_BUDGET; } memcpy(flow->snapshot,source,(size_t)words*sizeof(uint64_t)); }
        XrXirStatus status = error_cells(flow,op->args[0],flow->snapshot);
        if (status != XR_XIR_OK) return status;
    }
    if (error_copy_op(op->op) || op->op == XR_XIR_CELL_NEW ||
        (op->op == XR_XIR_CELL_READ && !xr_xir_type_is_cell(flow->body.domain.types,op->type)) || op->op == XR_XIR_LOCAL_NEW || op->op == XR_XIR_OWNED_LOCAL_NEW ||
        op->op == XR_XIR_SCALAR_LOCAL_NEW || op->op == XR_XIR_LOCAL_READ ||
        op->op == XR_XIR_OWNED_LOCAL_READ || op->op == XR_XIR_SCALAR_LOCAL_READ)
        { if (out) {
            const uint64_t *source=op->op==XR_XIR_CELL_NEW &&
                !xr_xir_cell_payload_operands(flow->body.domain.types,op->type,op->op) ?
                flow->zero : error_read(flow,flow->work,op->args[0]);
            if (!xir_compile_work(flow->remaining, (size_t)words * sizeof(*out))) return XR_XIR_BUDGET;
            memcpy(out,source,(size_t)words*sizeof(*out));
        } }
    else if (op->op == XR_XIR_LOCAL_WRITE || op->op == XR_XIR_OWNED_LOCAL_WRITE || op->op == XR_XIR_SCALAR_LOCAL_WRITE)
        {
            uint64_t *target = error_value(flow, flow->work, op->args[0]);
            const uint64_t *source = error_read(flow, flow->work, op->args[1]);
            if (target && target != source) {
                if (!xir_compile_work(flow->remaining, (size_t)words * sizeof(*target))) return XR_XIR_BUDGET;
                memcpy(target, source, (size_t)words * sizeof(*target));
            }
        }
    else if (op->op == XR_XIR_CELL_PROJECT) {
        /* The descriptor has no leaf SSA payload or independent value.
         * Existing writes invalidate all other COPY identities. */
        if (!out) return XR_XIR_BAD_STRUCTURE;
        error_type(flow,op->type,out);
    }
    else if (op->op == XR_XIR_ENUM_NEW) {
        if (!out) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t a = 0; a < flow->effects->outcomes_work.atom_count; ++a)
            if (flow->effects->outcomes_work.atoms[a].type == op->type && flow->effects->outcomes_work.atoms[a].variant == (uint32_t)op->immediate)
                error_add(out, a + 2);
    } else if (op->op == XR_XIR_GO) {
        if (!out) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status = error_call(flow, op, out);
        if (status != XR_XIR_OK || flow->restart) return status;
    } else if (op->op == XR_XIR_INVOKE_ERROR) {
        if (!out) return XR_XIR_BAD_STRUCTURE;
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        XrXirInstruction call_storage;const XrXirInstruction *call=NULL;
        read=error_operation(flow,flow->function_index,(uint32_t)op->immediate,&call_storage,&call);
        if (read!=XR_XIR_OK) return read;
        if (call->op == XR_XIR_TASK_AWAIT) {
            if (!xir_compile_work(flow->remaining, (uint64_t)words * sizeof(*out))) return XR_XIR_BUDGET;
            memcpy(out, error_read(flow, flow->work, call->args[0]), (size_t)words * sizeof(*out));
            return XR_XIR_OK;
        }
        uint32_t saved=flow->instruction;flow->instruction=(uint32_t)op->immediate;
        XrXirStatus status=error_call(flow,call,out);flow->instruction=saved;
        if (status!=XR_XIR_OK || flow->restart) return status;
    }
    else if (out) error_type(flow, op->type, out);
    if (op->op == XR_XIR_THROW)
        flow->summary_changed |= error_join(flow->escaping, error_read(flow, flow->work, op->args[0]), words);
    else if (op->op == XR_XIR_CALL_DEFAULT || op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_CALL_REQUIREMENT) {
        uint64_t *temporary = flow->snapshot;
        { if (!xir_compile_work(flow->remaining, (size_t)words * sizeof(*temporary))) { return XR_XIR_BUDGET; } memset(temporary, 0, (size_t)words * sizeof(*temporary)); }
        XrXirStatus status=error_call(flow, op, temporary); if (status!=XR_XIR_OK || flow->restart) return status;
        flow->summary_changed |= error_join(flow->escaping, temporary, words);
    }
    if (op->op == XR_XIR_CALL_DEFAULT || op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_CALL_REQUIREMENT ||
        op->op == XR_XIR_INVOKE_DEFAULT || op->op == XR_XIR_INVOKE || op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_SUSPEND || op->op == XR_XIR_TIMER_AFTER_MS ||
        op->op == XR_XIR_OUTPUT || op->op == XR_XIR_WRITE_STREAM || op->op == XR_XIR_PRINT ||
        op->op == XR_XIR_GO || op->op == XR_XIR_TASK_AWAIT ||
        op->op == XR_XIR_CLEANUP_LEAVE || op->op == XR_XIR_CLEANUP_ERROR || op->op == XR_XIR_CELL_LOCAL_WRITE)
        return error_cells(flow,UINT32_MAX,NULL);
    return XR_XIR_OK;
}
/* Return false only when a recognized filter excludes the complete value set. */
static XrXirStatus error_filter(ErrorFlow *flow,const XrXirInstruction *branch,bool yes,bool *possible) {
    if (!possible) return XR_XIR_BAD_STRUCTURE;
    *possible=true;
    XrXirInstruction test_storage;const XrXirInstruction *test=NULL;
    XrXirStatus status=error_definition(flow,branch->args[0],&test_storage,&test);
    if (status!=XR_XIR_OK) return status;
    if (!test) return XR_XIR_OK;
    uint32_t value;
    XrXirType type = XR_XIR_UNIT;
    int64_t variant = -1;
    if (test->op == XR_XIR_ERROR_IS) { value = test->args[0]; type = (XrXirType)test->immediate; }
    else if (test->op == XR_XIR_EQ_INT || test->op == XR_XIR_NE_INT) {
        XrXirInstruction tag_storage,constant_storage;
        const XrXirInstruction *tag=NULL,*constant=NULL;
        status=error_definition(flow,test->args[0],&tag_storage,&tag);
        if (status==XR_XIR_OK) status=error_definition(flow,test->args[1],&constant_storage,&constant);
        if (status!=XR_XIR_OK) return status;
        if (!tag || !constant) return XR_XIR_OK;
        if (tag->op == XR_XIR_CONST_INT) { const XrXirInstruction *swap = tag; tag = constant; constant = swap; }
        if (tag->op != XR_XIR_ENUM_TAG || constant->op != XR_XIR_CONST_INT) return XR_XIR_OK;
        value = tag->args[0]; variant = constant->immediate;
        if (variant<0) { *possible=test->op==XR_XIR_EQ_INT?!yes:yes;return XR_XIR_OK; }
        status=error_type_at(flow,value,&type);
        if (status!=XR_XIR_OK) return status;
        if (test->op == XR_XIR_NE_INT) yes = !yes;
    } else return XR_XIR_OK;
    uint32_t root = flow->roots[value];
    for (uint32_t slot = 0; slot < flow->active_count; ++slot) {
        uint32_t v = flow->active[slot];
        if (flow->roots[v] != root) continue;
        uint64_t *set = error_value(flow, flow->edge, v);
        for (uint32_t a = 0; a < flow->effects->outcomes_work.atom_count; ++a) {
            if (flow->effects->outcomes_work.atoms[a].variant==XR_XIR_ERROR_SYMBOLIC_VARIANT) continue;
            bool match = flow->effects->outcomes_work.atoms[a].type == type &&
                (variant < 0 || flow->effects->outcomes_work.atoms[a].variant == (uint64_t)variant);
            if (match != yes) set[(a + 2) / 64] &= ~((uint64_t)1 << ((a + 2) % 64));
        }
    }
    *possible=error_any(error_read(flow,flow->edge,value),flow->effects->outcomes_work.words);
    return XR_XIR_OK;
}
static XrXirStatus error_edge(ErrorFlow *flow, uint32_t from, uint32_t to,
    const XrXirInstruction *branch, bool yes) {
    if (!xir_compile_work(flow->remaining, (uint64_t)flow->stride * 3) ||
        !xir_compile_work(flow->remaining, (uint64_t)flow->active_count * ((uint64_t)flow->effects->outcomes_work.atom_count + 1)))
        return XR_XIR_BUDGET;
    /* Both original charges precede metadata access, including extreme probes. */
    const XrXirBlock *block = &flow->function->blocks[to];
    bool has_phi=false;
    if (block->count) {
        EffectBodyInstructionShape shape={0};
        XrXirStatus status=error_shape(flow,flow->function_index,block->first,&shape);
        if (status!=XR_XIR_OK) return status;
        has_phi=shape.op==XR_XIR_PHI;
    }
    bool panic_cells = flow->function->blocks[from].panic == to &&
        flow->function->blocks[from].frontier != block->frontier;
    uint64_t *incoming = flow->work;
    if (branch || has_phi || panic_cells) {
        { if (!xir_compile_work(flow->remaining, flow->stride * sizeof(uint64_t))) { return XR_XIR_BUDGET; } memcpy(flow->edge, flow->work, flow->stride * sizeof(uint64_t)); }
        if (flow->function->blocks[from].panic == to &&
            flow->function->blocks[from].frontier != flow->function->blocks[to].frontier) {
            uint64_t *saved = flow->work;
            flow->work = flow->edge;
            XrXirStatus status = error_cells(flow, UINT32_MAX, NULL);
            flow->work = saved;
            if (status != XR_XIR_OK) return status;
        }
        if (branch) {
            bool possible=false;XrXirStatus status=error_filter(flow,branch,yes,&possible);
            if (status!=XR_XIR_OK || !possible) return status;
        }
        if (has_phi) {
            if (!xir_compile_work(flow->remaining, flow->stride * sizeof(uint64_t))) return XR_XIR_BUDGET;
            memcpy(flow->snapshot, flow->edge, flow->stride * sizeof(uint64_t));
        }
        for (uint32_t i = block->first; i < block->first + block->count; ++i) {
            XrXirInstruction storage;const XrXirInstruction *phi=NULL;
            XrXirStatus read=error_operation(flow,flow->function_index,i,&storage,&phi);
            if (read!=XR_XIR_OK) return read;
            if (phi->op != XR_XIR_PHI) break;
            for (uint32_t p = 0; p < phi->args[1]; p += 2) {
                if (!xir_compile_work(flow->remaining, 1)) return XR_XIR_BUDGET;
                if (flow->function->operands[phi->args[0] + p] != from) continue;
                uint32_t value = flow->function->operands[phi->args[0] + p + 1];
                uint64_t *out = error_value(flow, flow->edge, flow->function->parameter_count + i);
                if (out) {
                    if (!xir_compile_work(flow->remaining, (size_t)flow->effects->outcomes_work.words * sizeof(*out))) return XR_XIR_BUDGET;
                    memcpy(out, error_read(flow, flow->snapshot, value), (size_t)flow->effects->outcomes_work.words * sizeof(*out));
                }
            }
        }
        incoming = flow->edge;
    }
    bool changed = error_join(flow->states + (size_t)to * flow->stride, incoming, flow->stride);
    if (!(flow->reachable[to] & ERROR_BLOCK_REACHABLE)) {
        flow->reachable[to] |= ERROR_BLOCK_REACHABLE;
        changed = true;
    }
    if (changed) {
        if (flow->scheduling) {
            XrXirStatus queued=error_cfg_enqueue(flow,to);
            if (queued!=XR_XIR_OK) return queued;
        } else flow->reachable[to] |= ERROR_BLOCK_PENDING;
        flow->changed = true;
    }
    return XR_XIR_OK;
}
typedef struct ErrorBlockRead {
    XrXirInstruction storage;
    const XrXirInstruction *end;
    bool constant,decoded;
} ErrorBlockRead;

static XrXirStatus error_block_row(ErrorFlow *flow,uint32_t b,uint32_t i,
    const uint8_t *zero_copy,const EffectBodyRun *ref,ErrorBlockRead *row) {
    const XrXirBlock *block=&flow->function->blocks[b];
    XrXirStatus status=block->panic?error_edge(flow,b,block->panic,NULL,true):XR_XIR_OK;
    bool copied_zero=false;
    if (status==XR_XIR_OK && zero_copy && i+1<block->first+block->count) {
        if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
        copied_zero=(zero_copy[i/8]&(UINT32_C(1)<<(i%8)))!=0;
        if (copied_zero && flow->slots[flow->function->parameter_count+i]!=UINT32_MAX)
            return XR_XIR_BAD_STRUCTURE;
    }
    if (status!=XR_XIR_OK) return status;
    if (copied_zero) return error_instruction_begin(flow,i);
    /* Every physical instruction retains its finite step and panic edge.
     * Primitive literal rows have no error slot or Cell transfer. */
    if(ref && i+1<block->first+block->count) {
        if(!xir_compile_work(flow->remaining,2)) return XR_XIR_BUDGET;
        if(!(ref->mask&UINT32_C(3)) &&
            ((ref->base.op==XR_XIR_CONST_BOOL && ref->base.type==XR_XIR_BOOL) ||
             (ref->base.op==XR_XIR_CONST_INT && ref->base.type==XR_XIR_I64) ||
             (ref->base.op==XR_XIR_CONST_FLOAT && ref->base.type==XR_XIR_F64) ||
             (ref->base.op==XR_XIR_CONST_RUNE && ref->base.type==XR_XIR_RUNE))) {
            if(!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
            if(flow->slots[flow->function->parameter_count+i]!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            return error_instruction_begin(flow,i);
        }
    }
    if (ref) {
        if (!xir_compile_work(flow->remaining,2)) return XR_XIR_BUDGET;
        if (!row->constant || !row->decoded) {
            status=effect_body_instruction_ref_value(&flow->body.read,ref,i,&row->storage);
            if (status!=XR_XIR_OK) return status;
            if (!xir_compile_work(flow->remaining,sizeof(row->decoded))) return XR_XIR_BUDGET;
            row->decoded=true;
        }
        if (status==XR_XIR_OK) {
            if (!xir_compile_work(flow->remaining,sizeof(row->end))) return XR_XIR_BUDGET;
            row->end=&row->storage;
        }
    } else status=error_operation(flow,flow->function_index,i,&row->storage,&row->end);
    return status==XR_XIR_OK?error_instruction(flow,i,row->end):status;
}

static XrXirStatus error_block_transfer(ErrorFlow *flow,uint32_t b,
    const uint8_t *zero_copy,ErrorBlockRead *row) {
    const XrXirBlock *block=&flow->function->blocks[b];
    if (flow->body.instruction!=effect_body_owned_instruction) {
        for (uint32_t i=block->first;i<block->first+block->count;++i) {
            XrXirStatus status=error_block_row(flow,b,i,zero_copy,NULL,row);
            if (status!=XR_XIR_OK || flow->restart) return status;
        }
        return XR_XIR_OK;
    }
    const EffectBodyFunction *function=NULL;
    XrXirStatus status=error_owned_function(flow,&function);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(flow->remaining,sizeof(EffectBodyRecordCursor))) return XR_XIR_BUDGET;
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    if (!xir_compile_work(flow->remaining,sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t end=block->first+block->count;
    if (!block->count || block->first>=function->instruction_count ||
        block->count>function->instruction_count-block->first) return XR_XIR_BAD_STRUCTURE;
    while (cursor.first<end) {
        if (!xir_compile_work(flow->remaining,1)) return XR_XIR_BUDGET;
        const EffectBodyRun *ref;
        status=effect_body_range_ref(&flow->body.read,function,&cursor,&ref);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(flow->remaining,2+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
        if (!xir_compile_work(flow->remaining,1+sizeof(row->constant)+sizeof(row->decoded)))
            return XR_XIR_BUDGET;
        row->constant=ref->mask==0;row->decoded=false;
        uint32_t first=ref->first<block->first?block->first:ref->first;
        uint32_t limit=ref->first+ref->count<end?ref->first+ref->count:end;
        for (uint32_t i=first;i<limit;++i) {
            status=error_block_row(flow,b,i,zero_copy,ref,row);
            if (status!=XR_XIR_OK || flow->restart) return status;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus error_block(ErrorFlow *flow, uint32_t b,const uint8_t *zero_copy) {
    const XrXirBlock *block = &flow->function->blocks[b];
    if (!xir_compile_work(flow->remaining, flow->stride)) return XR_XIR_BUDGET;
    { if (!xir_compile_work(flow->remaining, flow->stride * sizeof(uint64_t))) { return XR_XIR_BUDGET; } memcpy(flow->work, flow->states + (size_t)b * flow->stride, flow->stride * sizeof(uint64_t)); }
    ErrorBlockRead row;
    if (!xir_compile_work(flow->remaining,sizeof(row.end))) return XR_XIR_BUDGET;
    row.end=NULL;
    XrXirStatus transferred=error_block_transfer(flow,b,zero_copy,&row);
    if (transferred!=XR_XIR_OK || flow->restart) return transferred;
    const XrXirInstruction *end=row.end;
    if (!end) return XR_XIR_BAD_STRUCTURE;
    uint32_t last=block->first+block->count-1;
    flow->instruction=last;
    if (end->op == XR_XIR_JUMP || end->op == XR_XIR_CLEANUP_REGISTER ||
        end->op == XR_XIR_CLEANUP_LEAVE || end->op == XR_XIR_CLEANUP_ERROR) return error_edge(flow, b, end->targets[0], NULL, true);
    if (end->op == XR_XIR_BRANCH) {
        XrXirStatus status = error_edge(flow, b, end->targets[0], end, true);
        return status == XR_XIR_OK ? error_edge(flow, b, end->targets[1], end, false) : status;
    }
    if (end->op == XR_XIR_INVOKE_DEFAULT || end->op == XR_XIR_INVOKE || end->op == XR_XIR_INVOKE_INDIRECT ||
        end->op == XR_XIR_TASK_AWAIT) {
        XrXirStatus status = error_edge(flow, b, end->targets[0], NULL, true);
        if (status != XR_XIR_OK) return status;
        { if (!xir_compile_work(flow->remaining, (size_t)flow->effects->outcomes_work.words * sizeof(uint64_t))) { return XR_XIR_BUDGET; } memset(flow->snapshot, 0, (size_t)flow->effects->outcomes_work.words * sizeof(uint64_t)); }
        if (end->op == XR_XIR_TASK_AWAIT) {
            if (!xir_compile_work(flow->remaining, (uint64_t)flow->effects->outcomes_work.words * sizeof(uint64_t))) return XR_XIR_BUDGET;
            memcpy(flow->snapshot, error_read(flow, flow->work, end->args[0]),
                (size_t)flow->effects->outcomes_work.words * sizeof(uint64_t));
        } else status=error_call(flow, end, flow->snapshot);
        if (status!=XR_XIR_OK || flow->restart) return status;
        if (error_any(flow->snapshot, flow->effects->outcomes_work.words)) return error_edge(flow, b, end->targets[1], NULL, true);
    }
    return XR_XIR_OK;
}
/* Reusable owned bytes belong to one analysis, never to a cached proof. */
static void error_storage_free(ErrorFlow *flow) {
    xr_compile_resources_free(flow->storage);
    flow->storage = NULL; flow->storage_capacity = 0;
    flow->states = flow->work = flow->edge = flow->snapshot = NULL;
    flow->roots = flow->cells = flow->slots = flow->active = NULL;
    flow->cell_types=NULL;flow->cell_material=NULL;
    flow->cell_function=flow->cell_type_count=0;
    flow->zero = NULL; flow->cell_count = flow->active_count = 0;
    flow->reachable = NULL; flow->escaping = NULL;
    flow->queue=NULL;flow->front=flow->back=flow->pending=0;flow->scheduling=false;
}
static XrXirStatus error_storage_prepare(ErrorFlow *flow, size_t bytes) {
    if (!bytes) return XR_XIR_BAD_STRUCTURE;
    if (bytes > flow->storage_capacity) {
        XrXirStatus status = XR_XIR_OK;
        uint8_t *storage = xir_compile_calloc(flow->remaining, bytes, 1, &status);
        if (!storage) return status;
        xr_compile_resources_free(flow->storage);
        flow->storage = storage; flow->storage_capacity = bytes;
    } else {
        if (!xir_compile_work(flow->remaining, bytes)) return XR_XIR_BUDGET;
        memset(flow->storage, 0, bytes);
    }
    return XR_XIR_OK;
}
static XrXirStatus error_function(ErrorFlow *flow, uint32_t f) {
    flow->scheduling=false;flow->front=flow->back=flow->pending=0;
    flow->function_index=f;
    XrXirStatus head_status=effect_body_request_head(&flow->body,f,&flow->head);
    if (head_status!=XR_XIR_OK) return head_status;
    flow->function=&flow->head;
    if (!xir_compile_work(flow->remaining,sizeof(flow->cell_types)+sizeof(flow->cell_material)+
        sizeof(flow->cell_function)+sizeof(flow->cell_type_count))) return XR_XIR_BUDGET;
    flow->cell_types=NULL;flow->cell_material=NULL;flow->cell_function=flow->cell_type_count=0;
    flow->cells = NULL; flow->cell_count = flow->active_count = 0;
    if (flow->function->parameter_count > UINT32_MAX - flow->function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    flow->values = flow->function->parameter_count + flow->function->instruction_count;
    uint64_t words = flow->effects->outcomes_work.words, rows = (uint64_t)flow->function->block_count + 3;
    uint64_t minimum_extra = (uint64_t)flow->values * 2 * sizeof(uint32_t) +
        flow->function->block_count + (flow->function->block_count>1?
            (uint64_t)flow->function->block_count*sizeof(uint32_t):0);
    /* At least one writable words row and a separate zero vector are required
     * even when no SSA value can carry error facts. Reject resource-impossible
     * metadata before reading it, without charging a fictitious allocation. */
    if (!words || minimum_extra > SIZE_MAX ||
        words > (SIZE_MAX - minimum_extra) / sizeof(uint64_t) / (rows + 1)) return XR_XIR_BUDGET;
    uint64_t minimum = words * (rows + 1) * sizeof(uint64_t) + minimum_extra;
    if (minimum > flow->storage_capacity) {
        if (!xir_compile_context_valid(flow->remaining)) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus admitted = xir_compile_resource_status(xr_compile_resources_admit(
            flow->remaining->resources, (size_t)minimum, minimum));
        if (admitted != XR_XIR_OK) return admitted;
    }
    /* Only byte capacity is local. Every request rebuilds every actual SSA
     * classification; no fact survives a function or atom-width restart. */
    uint8_t inline_membership[1024], *heap_membership = NULL;
    uint32_t inline_sources[1024], *sources = inline_sources;
    uint8_t *membership = inline_membership;
    XrXirStatus status = XR_XIR_OK;
    /* Representation dispatch is not authority. Every real SSA rebuilds
     * membership through the current immutable body's canonical ranges. */
    if (!xir_compile_work(flow->remaining,sizeof(XrXirType *)+4)) return XR_XIR_BUDGET;
    XrXirType inline_cell_types[1024],*cell_types=error_owned_reader(flow)?inline_cell_types:NULL;
    if (flow->values > sizeof(inline_membership)) {
        uint64_t temporary_bytes=(uint64_t)flow->values*(sizeof(*sources)+sizeof(*membership)+
            (cell_types?sizeof(*cell_types):0));
        if (temporary_bytes>SIZE_MAX) return XR_XIR_BUDGET;
        heap_membership = xir_compile_alloc(flow->remaining,(size_t)temporary_bytes,&status);
        if (!heap_membership) return status;
        sources=(uint32_t *)heap_membership;
        if (cell_types) {
            if (!xir_compile_work(flow->remaining,sizeof(cell_types))) { status=XR_XIR_BUDGET;goto membership_done; }
            cell_types=(XrXirType *)(heap_membership+(size_t)flow->values*sizeof(*sources));
            membership=(uint8_t *)(cell_types+flow->values);
        } else membership=heap_membership+(size_t)flow->values*sizeof(*sources);
    }
    if (!xir_compile_work(flow->remaining,sizeof(ErrorMembership))) { status=XR_XIR_BUDGET;goto membership_done; }
    ErrorMembership inventory={membership,sources,cell_types,0};
    status=error_membership(flow,&inventory);
    if (status!=XR_XIR_OK) goto membership_done;
    uint64_t stride = (uint64_t)(flow->active_count ? flow->active_count : 1) * words;
    uint64_t copy_bytes=inventory.zero_copy_count ? ((uint64_t)flow->function->instruction_count+7)/8 : 0;
    uint64_t cell_bytes=error_owned_reader(flow)?(uint64_t)flow->cell_count*sizeof(ErrorCellType):0;
    uint64_t extra = ((uint64_t)flow->values * 2 + flow->active_count + flow->cell_count) *
        sizeof(uint32_t) + cell_bytes + flow->function->block_count + copy_bytes;
    uint64_t zero_bytes = words * sizeof(uint64_t);
    if (extra > SIZE_MAX - zero_bytes ||
        stride > (SIZE_MAX - zero_bytes - extra) / sizeof(uint64_t) / rows) { status = XR_XIR_BUDGET;goto membership_done; }
    uint64_t bytes = stride * rows * sizeof(uint64_t) + zero_bytes + extra;
    uint64_t queue_offset=bytes,queue_bytes=flow->function->block_count>1?
        (uint64_t)flow->function->block_count*sizeof(uint32_t):0;
    if (queue_bytes) {
        if (bytes>SIZE_MAX-3) { status=XR_XIR_BUDGET;goto membership_done; }
        queue_offset=(bytes+3)&~UINT64_C(3);
        if (queue_bytes>SIZE_MAX-queue_offset) { status=XR_XIR_BUDGET;goto membership_done; }
        bytes=queue_offset+queue_bytes;
    }
    if (!xir_compile_work(flow->remaining, stride * rows + words + flow->values + flow->function->block_count))
        { status = XR_XIR_BUDGET;goto membership_done; }
    flow->stride = (size_t)stride;
    status = error_storage_prepare(flow, (size_t)bytes);
    if (status == XR_XIR_OK) {
        size_t state_bytes = (size_t)(stride * rows) * sizeof(uint64_t);
        flow->states = (uint64_t *)flow->storage;
        flow->zero = (const uint64_t *)(flow->storage + state_bytes);
        flow->roots = (uint32_t *)(flow->storage + state_bytes + (size_t)zero_bytes);
        flow->slots = flow->roots + flow->values;
        flow->active = flow->slots + flow->values;
        flow->cells = flow->active + flow->active_count;
        if (cell_bytes) {
            if (!xir_compile_work(flow->remaining,sizeof(flow->cell_types))) { status=XR_XIR_BUDGET;goto membership_done; }
            flow->cell_types=(ErrorCellType *)(flow->cells+flow->cell_count);
        }
        flow->reachable = (uint8_t *)(flow->cells + flow->cell_count)+(size_t)cell_bytes;
        flow->queue=queue_bytes?(uint32_t *)(flow->storage+(size_t)queue_offset):NULL;
        /* Charge the temporary byte scan and all map, inverse and Cell writes. */
        if (!xir_compile_work(flow->remaining, (uint64_t)flow->values * 2 + flow->active_count + flow->cell_count))
            { status = XR_XIR_BUDGET;goto membership_done; }
        uint8_t *zero_copy=copy_bytes ? flow->reachable+flow->function->block_count : NULL;
        uint32_t cell = 0, active = 0;
        for (uint32_t v = 0; v < flow->values; ++v) {
            uint8_t classified = membership[v];
            if (classified & ERROR_ROW_ACTIVE) {
                if (active >= flow->active_count) { status = XR_XIR_BAD_STRUCTURE;goto membership_done; }
                flow->slots[v] = active; flow->active[active++] = v;
            } else flow->slots[v] = UINT32_MAX;
            if (zero_copy && (classified&ERROR_ROW_COPY) && !(classified&ERROR_ROW_ACTIVE)) {
                if (v<flow->function->parameter_count) { status=XR_XIR_BAD_STRUCTURE;goto membership_done; }
                if (!xir_compile_work(flow->remaining,1)) { status=XR_XIR_BUDGET;goto membership_done; }
                uint32_t i=v-flow->function->parameter_count;
                zero_copy[i/8]|=(uint8_t)(UINT32_C(1)<<(i%8));
            }
            if (classified & ERROR_ROW_CELL) {
                if (cell >= flow->cell_count) { status = XR_XIR_BAD_STRUCTURE;goto membership_done; }
                flow->cells[cell++] = v;
            }
        }
        if (cell != flow->cell_count || active != flow->active_count) { status = XR_XIR_BAD_STRUCTURE;goto membership_done; }
        status=error_cell_types_prepare(flow,cell_types);
        if (status!=XR_XIR_OK) goto membership_done;
        flow->work = flow->states + (size_t)flow->function->block_count * flow->stride;
        flow->edge = flow->work + flow->stride; flow->snapshot = flow->edge + flow->stride;
        uint32_t summary=flow->invocations?error_invocation_summary(flow->invocations,flow->invocation):f;
        if (summary>=(flow->invocations?flow->summary_count:flow->effects->count)) { status = XR_XIR_BAD_STRUCTURE;goto membership_done; }
        flow->escaping = flow->effects->outcomes_work.errors + (size_t)summary * flow->effects->outcomes_work.words;
        status = error_roots(flow,membership,sources);
        xr_compile_resources_free(heap_membership);heap_membership = NULL;
        flow->reachable[0] = ERROR_BLOCK_REACHABLE;
        for (uint32_t p = 0; p < flow->function->parameter_count && status == XR_XIR_OK; ++p) {
            if (!xir_compile_work(flow->remaining, flow->effects->outcomes_work.atom_count + 1)) status = XR_XIR_BUDGET;
            else {
                uint64_t *set = error_value(flow, flow->states, p);
                if (set) error_type(flow, flow->function->parameters[p], set);
            }
        }
        if (status==XR_XIR_OK) status=error_cfg_solve(flow,zero_copy);
    }
membership_done:
    xr_compile_resources_free(heap_membership);
    return status;
}
static uint32_t error_variant_count(const XrXirTypes *types, uint32_t t) {
    uint32_t d = types->nodes[t].nominal.declaration;
    return types->nominals->declarations ? types->nominals->declarations[d].variant_count :
        types->nominals->identities[d].variant_count;
}
static XrXirStatus effect_errors_seed(const XirMetadataDomain *module, XrXirEffects *effects, XrXirCompileContext *remaining) {
    XrXirStatus allocation_status = XR_XIR_OK;
    uint32_t parameters=0;
    for (uint32_t f=0;module->generics && f<module->function_count;++f) {
        if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
        if (module->generics[f].parameter_count>parameters) parameters=module->generics[f].parameter_count;
    }
    uint64_t atoms = parameters;
    for (uint32_t t = 0; module->types && t < module->types->count; ++t) {
        if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
        if (xr_xir_type_is_enum(module->types, (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + t)))
            atoms += error_variant_count(module->types, t);
    }
    if (atoms > UINT32_MAX-2u) return XR_XIR_BUDGET;
    effects->outcomes_work.source_type_count = module->types ? module->types->count : 0;
    effects->outcomes_work.error_rows=effects->count;
    effects->outcomes_work.atom_count = (uint32_t)atoms; effects->outcomes_work.atom_capacity = (uint32_t)atoms; effects->outcomes_work.words = (uint32_t)((atoms + 65) / 64);
    uint64_t bytes = atoms * sizeof(EffectErrorAtom) + (uint64_t)effects->count * effects->outcomes_work.words * sizeof(uint64_t);
    if ((bytes > SIZE_MAX) || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    if (atoms) effects->outcomes_work.atoms = xir_compile_calloc(remaining, (size_t)atoms, sizeof(EffectErrorAtom), &allocation_status);
    effects->outcomes_work.errors = xir_compile_calloc(remaining, (size_t)effects->count * effects->outcomes_work.words, sizeof(uint64_t), &allocation_status);
    if ((atoms && !effects->outcomes_work.atoms) || !effects->outcomes_work.errors) return allocation_status;

    uint32_t at = 0;
    for (uint32_t t = 0; module->types && t < module->types->count; ++t) {
        XrXirType type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + t);
        if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
        if (!xr_xir_type_is_enum(module->types, type)) continue;
        uint32_t count = error_variant_count(module->types, t);
        if (!xir_compile_work(remaining, count)) return XR_XIR_BUDGET;
        for (uint32_t v = 0; v < count; ++v) effects->outcomes_work.atoms[at++] = (EffectErrorAtom){type, v};
    }
    if (!xir_compile_work(remaining,parameters)) return XR_XIR_BUDGET;
    for (uint32_t p=0;p<parameters;++p)
        effects->outcomes_work.atoms[at++]=(EffectErrorAtom){(XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p),XR_XIR_ERROR_SYMBOLIC_VARIANT};
    return XR_XIR_OK;
}
static XrXirStatus error_summary_resize(ErrorFlow *flow) {
    XrXirEffects *effects=flow->effects;XrXirCompileContext *remaining=flow->remaining;
    XrXirStatus allocation_status = XR_XIR_OK;
    uint32_t words = (uint32_t)(((uint64_t)effects->outcomes_work.atom_count + 65) / 64);
    if (words == effects->outcomes_work.words) return XR_XIR_OK;
    uint32_t summaries=flow->invocations?flow->summary_count:effects->count;
    uint64_t count = (uint64_t)summaries * words;
    if (count > SIZE_MAX / sizeof(uint64_t) ||(count * sizeof(uint64_t) > SIZE_MAX) ||
        !xir_compile_work(remaining, count)) return XR_XIR_BUDGET;
    uint64_t *errors = xir_compile_calloc(remaining, (size_t)count, sizeof(*errors), &allocation_status);
    if (!errors) return allocation_status;

    for (uint32_t f = 0; f < summaries; ++f)
        { if (!xir_compile_work(remaining, (size_t)effects->outcomes_work.words * sizeof(*errors))) { xr_compile_resources_free(errors); return XR_XIR_BUDGET; } memcpy(errors + (size_t)f * words, effects->outcomes_work.errors + (size_t)f * effects->outcomes_work.words,
            (size_t)effects->outcomes_work.words * sizeof(*errors)); }
    xr_compile_resources_free(effects->outcomes_work.errors); effects->outcomes_work.errors = errors; effects->outcomes_work.words = words;
    return XR_XIR_OK;
}
/* A child task's errors affect GO values and await paths, but cannot grant
 * parent execution authority. Keep these dependencies outside authority heads. */
static XrXirStatus error_graph_prepare(EffectGraph *graph, XrXirCompileContext *remaining) {
    if (!xir_compile_context_valid(remaining) || !graph || !effect_body_material_shape(graph->material) ||
        graph->material->resources!=remaining->resources) return XR_XIR_BAD_STRUCTURE;
    uint32_t functions=effect_body_count(graph->material),count=0;
    for (uint32_t f = 0; f < functions; ++f) {
        EffectBodyRequest body=effect_body_owned_request(remaining,graph->material);
        EffectBodyFunctionDimensions dimensions={0};
        XrXirStatus dimension_status=effect_body_request_dimensions(&body,f,&dimensions);
        if (dimension_status!=XR_XIR_OK) return dimension_status;
        const EffectBodyFunctionDimensions *function=&dimensions;
        const EffectPhysicalFunction *physical=NULL;
        XrXirStatus status=effect_graph_owned_physical_function(remaining,graph,f,&physical);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t r=0;r<physical->count;++r) {
            if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=physical->records[r];
            if (!(record.mask&EFFECT_PHYSICAL_GO)) continue;
            if (record.value<function->parameter_count || record.value>=physical->values) return XR_XIR_BAD_STRUCTURE;
            EffectBodyInstructionShape shape={0};
            status=effect_body_request_shape(&body,f,record.value-function->parameter_count,&shape);
            if (status!=XR_XIR_OK) return status;
            if (shape.op!=XR_XIR_GO) return XR_XIR_BAD_STRUCTURE;
            if (count == UINT32_MAX) return XR_XIR_BUDGET;
            ++count;
        }
    }
    if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
    graph->error_edge_count = count;
    if (!count) return XR_XIR_OK;
    uint64_t bytes = (uint64_t)functions * sizeof(*graph->error_heads) +
        (uint64_t)count * sizeof(*graph->error_edges);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    graph->error_heads = xir_compile_calloc(remaining, functions, sizeof(*graph->error_heads), &status);
    graph->error_edges = xir_compile_calloc(remaining, count, sizeof(*graph->error_edges), &status);
    if (!graph->error_heads || !graph->error_edges) return status;
    if (!xir_compile_work(remaining, functions)) return XR_XIR_BUDGET;
    for (uint32_t f = 0; f < functions; ++f) graph->error_heads[f] = UINT32_MAX;
    uint32_t at = 0;
    for (uint32_t f = 0; f < functions; ++f) {
        EffectBodyRequest body=effect_body_owned_request(remaining,graph->material);
        EffectBodyFunctionDimensions dimensions={0};
        XrXirStatus dimension_status=effect_body_request_dimensions(&body,f,&dimensions);
        if (dimension_status!=XR_XIR_OK) return dimension_status;
        const EffectBodyFunctionDimensions *function=&dimensions;
        const EffectPhysicalFunction *physical=NULL;
        status=effect_graph_owned_physical_function(remaining,graph,f,&physical);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t r=0;r<physical->count;++r) {
            if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=physical->records[r];
            if (!(record.mask&EFFECT_PHYSICAL_GO)) continue;
            if (record.value<function->parameter_count || record.value>=physical->values)
                return XR_XIR_BAD_STRUCTURE;
            uint32_t i=record.value-function->parameter_count;
            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            status=effect_body_request_instruction(&body,f,i,&storage,&op);
            if (status!=XR_XIR_OK) return status;
            if (op->op != XR_XIR_GO) return XR_XIR_BAD_STRUCTURE;
            uint32_t callee = (uint32_t)op->immediate;
            if (callee >= functions || at >= count) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(remaining, 2)) return XR_XIR_BUDGET;
            graph->error_edges[at] = (EffectEdge){f, graph->error_heads[callee], i, false};
            graph->error_heads[callee] = at++;
        }
    }
    return at == count ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
/* Empty escaping is an exact equation only for these known op semantics.
 * Every visit rebuilds this proof; new operations conservatively interpret CFG. */
static inline XrXirStatus error_empty_escaping(const XrXirFunction *function,
    const XrXirCompileContext *remaining, bool *empty) {
    *empty = false;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
        XrXirOp op = function->instructions[i].op;
        if (op <= XR_XIR_INVALID || op >= XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        if (!effect_physical_error_empty_opcode(op)) return XR_XIR_OK;
    }
    *empty = true;
    return XR_XIR_OK;
}
static XrXirStatus error_empty_escaping_read(const EffectBodyRequest *body,uint32_t function,
    const XrXirCompileContext *remaining,bool *empty) {
    if (!body || !empty) return XR_XIR_BAD_STRUCTURE;
    *empty=false;EffectBodyFunctionDimensions head={0};
    XrXirStatus status=effect_body_request_dimensions(body,function,&head);
    for (uint32_t i=0;status==XR_XIR_OK && i<head.instruction_count;++i) {
        if (!xir_compile_work(remaining,1)) return XR_XIR_BUDGET;
        EffectBodyInstructionShape shape={0};status=effect_body_request_shape(body,function,i,&shape);
        if (status!=XR_XIR_OK) return status;
        if (shape.op<=XR_XIR_INVALID || shape.op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        if (!effect_physical_error_empty_opcode(shape.op)) return XR_XIR_OK;
    }
    if (status==XR_XIR_OK) *empty=true;
    return status;
}
/* Only the inference generation that built this complete inventory can
 * consume the sparse locator. Existing nonempty summaries still force CFG. */
static XrXirStatus error_empty_escaping_graph(uint32_t index,
    const EffectGraph *graph,const XrXirCompileContext *work,bool *empty) {
    if (!empty) return XR_XIR_BAD_STRUCTURE;
    *empty=false;const EffectPhysicalFunction *physical=NULL;
    XrXirStatus status=effect_graph_owned_physical_function(work,graph,index,&physical);
    if (status!=XR_XIR_OK) return status;
    EffectBodyRequest body=effect_body_owned_request(work,graph->material);
    EffectBodyFunctionDimensions dimensions={0};
    status=effect_body_request_dimensions(&body,index,&dimensions);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionDimensions *function=&dimensions;
    for (uint32_t r=0;r<physical->count;++r) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        EffectPhysicalRecord record=physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_ERROR_ESCAPE)) continue;
        if (record.value<function->parameter_count || record.value>=physical->values)
            return XR_XIR_BAD_STRUCTURE;
        EffectBodyInstructionShape shape={0};
        status=effect_body_request_shape(&body,index,record.value-function->parameter_count,&shape);
        if (status!=XR_XIR_OK) return status;
        XrXirOp op=shape.op;
        if (op<=XR_XIR_INVALID || op>=XR_XIR_OP_COUNT || effect_physical_error_empty_opcode(op))
            return XR_XIR_BAD_STRUCTURE;
        return XR_XIR_OK;
    }
    *empty=true;return XR_XIR_OK;
}
/* Callees with no outstanding dependencies run first. Cycles retain original
 * function order and the existing fair FIFO reaches their least fixed point.
 * Ordering is rebuilt on every atom-width restart, never a cached summary. */
static XrXirStatus error_queue_prepare(EffectGraph *graph, ErrorFlow *flow) {
    if (!graph || !flow || !flow->body.owner || !flow->effects ||
        !xir_compile_context_valid(flow->remaining)) return XR_XIR_BAD_STRUCTURE;
    uint32_t count = flow->effects->count;
    if (!count || count != flow->body.domain.function_count || !graph->heads ||
        !graph->queue || !graph->queued || (graph->edge_count && !graph->edges) ||
        (graph->error_edge_count && (!graph->error_heads || !graph->error_edges)))
        return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes = (uint64_t)count * sizeof(uint32_t);
    if (bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    uint32_t *degree = NULL;
    bool temporary = !flow->storage || flow->storage_capacity < (size_t)bytes;
    if (temporary) {
        degree = xir_compile_calloc(flow->remaining, count, sizeof(*degree), &status);
        if (!degree) return status;
    } else {
        /* All CFG views are dead here. The next interpretation fully rebinds
         * them after preparing storage; escaping belongs to separate facts. */
        if (!xir_compile_work(flow->remaining, 10)) return XR_XIR_BUDGET;
        flow->states = flow->work = flow->edge = flow->snapshot = NULL;
        flow->reachable = NULL;
        flow->roots = flow->cells = flow->slots = flow->active = NULL;
        flow->zero = NULL;
        if (!xir_compile_work(flow->remaining, bytes)) return XR_XIR_BUDGET;
        degree = (uint32_t *)flow->storage;
        memset(degree, 0, (size_t)bytes);
    }
    const uint32_t *heads[2] = {graph->heads, graph->error_heads};
    const EffectEdge *edges[2] = {graph->edges, graph->error_edges};
    uint32_t counts[2] = {graph->edge_count, graph->error_edge_count};
    if (!xir_compile_work(flow->remaining, count)) { status = XR_XIR_BUDGET; goto done; }
    for (uint32_t f = 0; f < count; ++f) graph->queued[f] = 0;
    for (uint32_t list = 0; list < 2; ++list) {
        if (!heads[list]) continue;
        uint32_t seen = 0;
        for (uint32_t f = 0; f < count; ++f) {
            if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
            uint32_t e = heads[list][f];
            while (e != UINT32_MAX) {
                if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
                if (e >= counts[list] || seen >= counts[list]) { status = XR_XIR_BAD_STRUCTURE; goto done; }
                EffectEdge edge = edges[list][e];
                if (edge.caller >= count || (edge.next != UINT32_MAX && edge.next >= e)) {
                    status = XR_XIR_BAD_STRUCTURE; goto done;
                }
                if (degree[edge.caller] == UINT32_MAX) { status = XR_XIR_BUDGET; goto done; }
                if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
                ++degree[edge.caller]; ++seen; e = edge.next;
            }
        }
        if (seen != counts[list]) { status = XR_XIR_BAD_STRUCTURE; goto done; }
    }
    uint32_t front = 0, back = 0;
    for (uint32_t f = 0; f < count; ++f) {
        if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
        if (degree[f]) continue;
        if (!xir_compile_work(flow->remaining, 2)) { status = XR_XIR_BUDGET; goto done; }
        graph->queue[back++] = f; graph->queued[f] = 1;
    }
    while (front < back) {
        if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
        uint32_t f = graph->queue[front++];
        for (uint32_t list = 0; list < 2; ++list) {
            if (!heads[list]) continue;
            if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
            uint32_t e = heads[list][f];
            while (e != UINT32_MAX) {
                if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
                if (e >= counts[list]) { status = XR_XIR_BAD_STRUCTURE; goto done; }
                EffectEdge edge = edges[list][e];
                if (edge.caller >= count || !degree[edge.caller] ||
                    (edge.next != UINT32_MAX && edge.next >= e)) { status = XR_XIR_BAD_STRUCTURE; goto done; }
                if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
                --degree[edge.caller];
                if (!degree[edge.caller]) {
                    if (graph->queued[edge.caller] || back >= count) { status = XR_XIR_BAD_STRUCTURE; goto done; }
                    if (!xir_compile_work(flow->remaining, 2)) { status = XR_XIR_BUDGET; goto done; }
                    graph->queue[back++] = edge.caller; graph->queued[edge.caller] = 1;
                }
                e = edge.next;
            }
        }
    }
    for (uint32_t f = 0; f < count; ++f) {
        if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; goto done; }
        if (graph->queued[f]) continue;
        if (back >= count) { status = XR_XIR_BAD_STRUCTURE; goto done; }
        if (!xir_compile_work(flow->remaining, 2)) { status = XR_XIR_BUDGET; goto done; }
        graph->queue[back++] = f; graph->queued[f] = 1;
    }
    if (back != count) status = XR_XIR_BAD_STRUCTURE;
done:
    if (temporary) xr_compile_resources_free(degree);
    return status;
}
/* Summaries grow monotonically. Only reverse dependants need fresh local CFG
 * interpretation; new atom identities still restart every function. */
static XrXirStatus error_functions(XrXirEffects *effects, EffectGraph *graph, ErrorFlow *flow) {
    XrXirStatus status = XR_XIR_OK;
    uint32_t front = 0, back = 0, pending = 0;
    do {
        status = error_queue_prepare(graph, flow);
        if (status != XR_XIR_OK) return status;
        front = back = 0; pending = effects->count; flow->restart = false;
        while (pending && status == XR_XIR_OK && !flow->restart) {
            if (!xir_compile_work(flow->remaining, 2)) return XR_XIR_BUDGET;
            uint32_t f = graph->queue[front];
            front = front + 1 == effects->count ? 0 : front + 1;
            --pending; graph->queued[f] = 0; flow->summary_changed = false;
            bool empty = false;
            status = error_empty_escaping_graph(f,graph,flow->remaining,&empty);
            if (status == XR_XIR_OK && empty) {
                /* Preserve an existing nonempty summary rather than washing it. */
                if (!xir_compile_work(flow->remaining, effects->outcomes_work.words)) status = XR_XIR_BUDGET;
                else if (error_any(effects->outcomes_work.errors + (size_t)f * effects->outcomes_work.words, effects->outcomes_work.words)) empty = false;
            }
            if (status == XR_XIR_OK && !empty) status = error_function(flow, f);
            if (status != XR_XIR_OK || flow->restart || !flow->summary_changed) continue;
            for (uint32_t list = 0; list < 2 && status == XR_XIR_OK; ++list) {
                const uint32_t *heads = list ? graph->error_heads : graph->heads;
                const EffectEdge *edges = list ? graph->error_edges : graph->edges;
                if (!heads) continue;
                for (uint32_t e = heads[f]; e != UINT32_MAX; e = edges[e].next) {
                    if (!xir_compile_work(flow->remaining, 1)) { status = XR_XIR_BUDGET; break; }
                    uint32_t caller = edges[e].caller;
                    if (caller >= flow->body.domain.function_count) { status = XR_XIR_BAD_STRUCTURE; break; }
                    if (graph->queued[caller]) continue;
                    if (!xir_compile_work(flow->remaining, 2)) { status = XR_XIR_BUDGET; break; }
                    graph->queue[back] = caller;
                    back = back + 1 == effects->count ? 0 : back + 1;
                    graph->queued[caller] = 1; ++pending;
                }
            }
        }
        if (status == XR_XIR_OK && flow->restart) status = error_summary_resize(flow);
    } while (status == XR_XIR_OK && flow->restart);
    return status;
}
