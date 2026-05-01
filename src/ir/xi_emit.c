/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit.c - Xi IR to VM bytecode emitter
 *
 * Translates typed SSA IR (XiFunc) into register-based bytecode
 * targeting the existing Xray VM (XrProto / xchunk.h format).
 */

#include "xi_emit.h"
#include "xi_analysis.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_names.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/object/xstring.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xmethod.h"
#include "../runtime/xexec_state.h"
#include "../frontend/parser/xast_nodes.h"
#include <string.h>

/* ========== Emit Context ========== */

#define MAX_REGS 256
#define NO_REG   255

typedef struct {
    XiFunc *func;
    XrProto *proto;
    XrayIsolate *isolate;      /* for string interning; may be NULL */
    XiEmitStatus status;

    /* Register allocation: value_id -> register number */
    uint8_t *reg_map;        /* [next_value_id] */
    uint32_t reg_map_size;
    uint8_t next_reg;        /* next free register */
    uint8_t max_reg;         /* high-water mark */

    /* Free register stack for register recycling */
    uint8_t free_regs[MAX_REGS];
    uint16_t nfree;          /* count of free registers on the stack */

    /* Liveness: per-value last-use tracking (value_id -> last-use ordinal) */
    uint32_t *last_use;      /* [next_value_id], 0 = unused/dead */
    uint32_t current_ordinal;/* monotonic instruction counter */

    /* Line number tracking for debug info */
    int current_line;        /* line of the value being emitted */

    /* Block linearization */
    XiBlock **rpo_order;     /* blocks in RPO order */
    uint32_t rpo_count;

    /* Jump patching: block_id -> start PC */
    int *block_pc;           /* [next_block_id], -1 = not yet emitted */
    uint32_t block_pc_size;

    /* Pending jump patches: instructions that need target PCs */
    struct {
        int pc;              /* instruction PC to patch */
        uint32_t target_bid; /* target block ID */
    } *patches;
    uint32_t npatch;
    uint32_t patch_cap;

    /* OP_TRY patches: absolute target PC patching (catch + finally) */
    struct {
        int pc;              /* OP_TRY instruction PC */
        uint32_t target_bid; /* catch block ID */
        uint32_t finally_bid;/* finally block ID (0 if none) */
    } *try_patches;
    uint32_t ntry_patch;
    uint32_t try_patch_cap;

    /* Track which value IDs have been wrapped in a cell (OP_CELL_NEW).
     * Prevents double-wrapping when multiple closures capture the same
     * mutable variable. */
    bool *cell_wrapped;      /* [next_value_id] */

    /* Comparison-branch fusion: if the block control is a comparison with
     * no other consumers, skip emitting OP_CMP_* and instead emit the
     * branch-form opcode (OP_LT/LE/EQ) directly in the terminator. */
    XiValue *fused_cmp;
} EmitCtx;

/* ========== Helpers ========== */

static void emit_error(EmitCtx *ctx, XiEmitStatus s) {
    if (ctx->status == XI_EMIT_OK)
        ctx->status = s;
}

static int current_pc(EmitCtx *ctx) {
    return PROTO_CODE_COUNT(ctx->proto);
}

static void emit_inst(EmitCtx *ctx, XrInstruction inst) {
    xr_vm_proto_write(ctx->proto, inst, ctx->current_line);
}

/* Return a register to the free pool for reuse. */
static void free_reg(EmitCtx *ctx, uint8_t reg) {
    if (reg == NO_REG) return;
    if (ctx->nfree < MAX_REGS) {
        ctx->free_regs[ctx->nfree++] = reg;
    }
}

/* Get register for a value. Assigns one if not yet mapped.
 * Uses free register stack before allocating new ones. */
static uint8_t reg_of(EmitCtx *ctx, const XiValue *v) {
    XR_DCHECK(v != NULL, "reg_of: NULL value");

    if (v->id >= ctx->reg_map_size) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return 0;
    }

    if (ctx->reg_map[v->id] == NO_REG) {
        /* Try recycled register first */
        if (ctx->nfree > 0) {
            ctx->reg_map[v->id] = ctx->free_regs[--ctx->nfree];
        } else {
            if (ctx->next_reg >= MAX_REGS - 1) {
                emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
                return 0;
            }
            ctx->reg_map[v->id] = ctx->next_reg++;
            if (ctx->next_reg > ctx->max_reg)
                ctx->max_reg = ctx->next_reg;
        }
    }
    return ctx->reg_map[v->id];
}

/* Like reg_of but never uses the free list — always allocates from next_reg.
 * Call instructions place args at dst+1..dst+nargs; a recycled low register
 * for dst could overlap with live source registers and cause clobber bugs. */
static uint8_t alloc_reg_fresh(EmitCtx *ctx, const XiValue *v) {
    XR_DCHECK(v != NULL, "alloc_reg_fresh: NULL value");
    if (v->id >= ctx->reg_map_size) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return 0;
    }
    if (ctx->reg_map[v->id] == NO_REG) {
        if (ctx->next_reg >= MAX_REGS - 1) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return 0;
        }
        ctx->reg_map[v->id] = ctx->next_reg++;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
    }
    return ctx->reg_map[v->id];
}

/* Release registers of input args whose last use is at the current ordinal.
 * Called AFTER emitting an instruction that reads these args. */
static void try_free_args(EmitCtx *ctx, const XiValue *v) {
    for (uint16_t i = 0; i < v->nargs; i++) {
        const XiValue *arg = v->args[i];
        if (!arg || arg->id >= ctx->reg_map_size) continue;
        /* Free register if this is the last use of arg */
        if (ctx->last_use[arg->id] == ctx->current_ordinal) {
            uint8_t r = ctx->reg_map[arg->id];
            ctx->reg_map[arg->id] = NO_REG;
            free_reg(ctx, r);
        }
    }
}

/* Add a pending jump patch. */
static void add_patch(EmitCtx *ctx, int pc, uint32_t target_bid) {
    if (ctx->npatch >= ctx->patch_cap) {
        uint32_t new_cap = ctx->patch_cap ? ctx->patch_cap * 2 : 16;
        void *tmp = xr_realloc(ctx->patches, new_cap * sizeof(ctx->patches[0]));
        if (!tmp) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        ctx->patches = tmp;
        ctx->patch_cap = new_cap;
    }
    ctx->patches[ctx->npatch].pc = pc;
    ctx->patches[ctx->npatch].target_bid = target_bid;
    ctx->npatch++;
}

/* Add a pending OP_TRY patch (catch + finally absolute PC targets). */
static void add_try_patch(EmitCtx *ctx, int pc, uint32_t catch_bid,
                          uint32_t finally_bid) {
    if (ctx->ntry_patch >= ctx->try_patch_cap) {
        uint32_t new_cap = ctx->try_patch_cap ? ctx->try_patch_cap * 2 : 4;
        void *tmp = xr_realloc(ctx->try_patches, new_cap * sizeof(ctx->try_patches[0]));
        if (!tmp) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        ctx->try_patches = tmp;
        ctx->try_patch_cap = new_cap;
    }
    ctx->try_patches[ctx->ntry_patch].pc = pc;
    ctx->try_patches[ctx->ntry_patch].target_bid = catch_bid;
    ctx->try_patches[ctx->ntry_patch].finally_bid = finally_bid;
    ctx->ntry_patch++;
}

/* Add constant to pool, return index. */
static int add_const_int(EmitCtx *ctx, int64_t val) {
    XrValue xv = xr_make_int_val(val, XR_TAG_I64);
    int idx = xr_vm_proto_add_constant(ctx->proto, xv);
    if (idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

static int add_const_float(EmitCtx *ctx, double val) {
    XrValue xv = xr_make_float_val(val, XR_TAG_F64);
    int idx = xr_vm_proto_add_constant(ctx->proto, xv);
    if (idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

static int add_const_string(EmitCtx *ctx, const char *str) {
    XrValue xv;
    if (ctx->isolate && str) {
        XrString *xs = xr_compile_time_intern(ctx->isolate, str, strlen(str));
        xv = xr_string_value(xs);
    } else {
        /* No isolate: raw C string pointers are not valid GC objects,
         * so xr_make_ptr_val would read garbage GC header bytes.
         * Use null placeholder — callers without isolate only check
         * instruction sequences, not constant pool values. */
        xv = xr_null();
    }
    int idx = xr_vm_proto_add_constant(ctx->proto, xv);
    if (idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
    }
    return idx;
}

/* Add a method name to the proto's local symbol table.  Returns the local
 * symbol index suitable for OP_INVOKE's B field.  Requires an isolate
 * (for the global symbol table); returns -1 on error. */
static int add_symbol(EmitCtx *ctx, const char *name) {
    if (!ctx->isolate || !name) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return -1;
    }
    XrSymbolTable *st = (XrSymbolTable *)xr_isolate_get_symbol_table(ctx->isolate);
    XR_DCHECK(st != NULL, "isolate must have a symbol table");
    SymbolId global = xr_symbol_register_in_table(st, name);
    int local = xr_proto_add_symbol(ctx->proto, (int32_t)global);
    if (local > MAXARG_B) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
        return -1;
    }
    return local;
}

/* ========== Last-Use Computation ========== */

/* Pre-compute last-use ordinals for register recycling.
 * Walks all blocks in RPO, assigning each value a monotonic ordinal.
 * For each arg reference, updates last_use[arg_id] = max ordinal.
 * Also accounts for block terminators that reference values. */
static void compute_last_use(EmitCtx *ctx) {
    uint32_t ord = 1;
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk) continue;

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            /* Record ordinal for this value */
            /* Update last-use of all args referenced by this value */
            for (uint16_t a = 0; a < v->nargs; a++) {
                XiValue *arg = v->args[a];
                if (arg && arg->id < ctx->reg_map_size)
                    ctx->last_use[arg->id] = ord;
            }
            ord++;
        }

        /* Terminator references: control value and phi args in successors */
        if (blk->control && blk->control->id < ctx->reg_map_size)
            ctx->last_use[blk->control->id] = ord;

        /* Phi args from this block's successors reference values too */
        for (int s = 0; s < 2; s++) {
            XiBlock *succ = blk->succs[s];
            if (!succ) continue;
            int pred_idx = -1;
            for (uint16_t p = 0; p < succ->npreds; p++) {
                if (succ->preds[p] == blk) { pred_idx = (int)p; break; }
            }
            if (pred_idx < 0) continue;
            for (XiPhi *phi = succ->phis; phi; phi = phi->next) {
                if ((uint16_t)pred_idx < phi->value.nargs) {
                    XiValue *src = phi->value.args[pred_idx];
                    if (src && src->id < ctx->reg_map_size)
                        ctx->last_use[src->id] = ord;
                }
            }
        }
        ord++;  /* account for terminator */
    }

    /* Phi registers must never be freed: they are referenced by
     * emit_phi_moves from any predecessor, which is not captured
     * by the ordinal-based last-use tracking above. */
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk) continue;
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (phi->value.id < ctx->reg_map_size)
                ctx->last_use[phi->value.id] = UINT32_MAX;
        }
    }

    /* Loop-invariant liveness: values defined outside a loop but used inside
     * must stay live for the entire loop — the single RPO walk above only
     * records one ordinal per use, but the VM re-executes loop blocks.
     *
     * Algorithm: detect back edges (succ.rpo <= block.rpo).  For each back
     * edge target (loop header), any value whose def-block RPO < header RPO
     * and whose last_use falls inside the loop range must be pinned. */
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk) continue;
        for (int s = 0; s < 2; s++) {
            XiBlock *succ = blk->succs[s];
            if (!succ || succ->rpo == 0) continue;
            if (succ->rpo > blk->rpo) continue;   /* not a back edge */

            /* Back edge: blk → succ.  Loop spans RPO [succ->rpo, blk->rpo].
             * Pin every value defined before the loop that is used inside. */
            uint32_t loop_lo = succ->rpo;
            uint32_t loop_hi = blk->rpo;
            for (uint32_t lr = loop_lo; lr <= loop_hi; lr++) {
                XiBlock *lb = ctx->rpo_order[lr];
                if (!lb) continue;
                for (uint32_t i = 0; i < lb->nvalues; i++) {
                    XiValue *v = lb->values[i];
                    for (uint16_t a = 0; a < v->nargs; a++) {
                        XiValue *arg = v->args[a];
                        if (!arg || arg->id >= ctx->reg_map_size) continue;
                        if (!arg->block) continue;
                        if (arg->block->rpo > 0 && arg->block->rpo < loop_lo)
                            ctx->last_use[arg->id] = UINT32_MAX;
                    }
                }
            }
        }
    }
}

/* ========== Register Allocation ========== */

/* Params get R[0..nparams-1], phis pre-assigned, last-use computed. */
static void alloc_registers(EmitCtx *ctx) {
    XiFunc *f = ctx->func;

    /* Assign parameter registers by scanning entry block for XI_PARAM ops.
     * This is robust whether f->params is populated or not. */
    XiBlock *entry = f->entry;
    if (entry) {
        for (uint32_t i = 0; i < entry->nvalues; i++) {
            XiValue *v = entry->values[i];
            if (v->op == XI_PARAM) {
                uint16_t pidx = (uint16_t)v->aux_int;
                if (v->id < ctx->reg_map_size && pidx < MAX_REGS) {
                    ctx->reg_map[v->id] = (uint8_t)pidx;
                    if (pidx + 1 > ctx->next_reg) {
                        ctx->next_reg = (uint8_t)(pidx + 1);
                        ctx->max_reg = ctx->next_reg;
                    }
                }
            }
        }
    }

    /* Pre-assign phi registers to avoid conflicts with phi moves.
     * Phis get their own registers before instruction values. */
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            (void)reg_of(ctx, &phi->value);
            if (ctx->status != XI_EMIT_OK) return;
        }
    }
}

/* ========== Phi Elimination ========== */

/* Emit MOVE instructions for phi nodes when transitioning from
 * predecessor 'pred' to successor 'succ'. */
static void emit_phi_moves(EmitCtx *ctx, XiBlock *pred, XiBlock *succ) {
    /* Find which predecessor index 'pred' is in succ->preds */
    int pred_idx = -1;
    for (uint16_t p = 0; p < succ->npreds; p++) {
        if (succ->preds[p] == pred) { pred_idx = (int)p; break; }
    }
    if (pred_idx < 0) return;

    for (XiPhi *phi = succ->phis; phi; phi = phi->next) {
        if ((uint16_t)pred_idx >= phi->value.nargs) continue;
        XiValue *src = phi->value.args[pred_idx];
        if (!src) continue;

        uint8_t dst_reg = reg_of(ctx, &phi->value);
        uint8_t src_reg = reg_of(ctx, src);
        if (ctx->status != XI_EMIT_OK) return;

        if (dst_reg != src_reg) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst_reg, src_reg, 0));
        }
    }
}

/* ========== Class Emission Helpers ========== */

/* Convert an AST literal to XrValue for field default values. */
static XrValue ast_field_default_to_value(EmitCtx *ctx, AstNode *init) {
    if (!init) return xr_null();
    if (init->type == AST_LITERAL_INT)
        return xr_int(init->as.literal.raw_value.int_val);
    if (init->type == AST_LITERAL_FLOAT)
        return xr_float(init->as.literal.raw_value.float_val);
    if (init->type == AST_LITERAL_TRUE)  return xr_bool(true);
    if (init->type == AST_LITERAL_FALSE) return xr_bool(false);
    if (init->type == AST_LITERAL_STRING && ctx->isolate) {
        const char *s = init->as.literal.raw_value.string_val;
        if (s) {
            XrString *xs = xr_compile_time_intern(ctx->isolate, s, strlen(s));
            if (xs) return xr_string_value(xs);
        }
    }
    return xr_null();
}

/* Populate instance and static fields on the descriptor from AST. */
static bool emit_class_collect_fields(EmitCtx *ctx, ClassDeclNode *cd,
                                       XrClassDescriptor *desc) {
    /* Instance fields */
    uint32_t fc = 0;
    for (int i = 0; i < cd->field_count; i++)
        if (cd->fields[i]->type == AST_FIELD_DECL
            && !cd->fields[i]->as.field_decl.is_static)
            fc++;
    if (fc > 0) {
        desc->instance_fields = (XrFieldDescriptorEntry *)xr_calloc(
            fc, sizeof(XrFieldDescriptorEntry));
        if (!desc->instance_fields) return false;
        desc->instance_field_count = fc;
        uint32_t idx = 0;
        for (int i = 0; i < cd->field_count; i++) {
            if (cd->fields[i]->type != AST_FIELD_DECL) continue;
            FieldDeclNode *f = &cd->fields[i]->as.field_decl;
            if (f->is_static) continue;
            desc->instance_fields[idx].name = strdup(f->name);
            desc->instance_fields[idx].type_name =
                f->field_type ? xr_type_to_string(f->field_type) : NULL;
            desc->instance_fields[idx].default_value =
                ast_field_default_to_value(ctx, f->initializer);
            if (f->is_private) desc->instance_fields[idx].flags |= XR_FIELD_PRIVATE;
            if (f->is_final)   desc->instance_fields[idx].flags |= XR_FIELD_FINAL;
            idx++;
        }
    }
    /* Static fields */
    uint32_t sfc = 0;
    for (int i = 0; i < cd->field_count; i++)
        if (cd->fields[i]->type == AST_FIELD_DECL
            && cd->fields[i]->as.field_decl.is_static)
            sfc++;
    if (sfc > 0) {
        desc->static_fields = (XrFieldDescriptorEntry *)xr_calloc(
            sfc, sizeof(XrFieldDescriptorEntry));
        if (!desc->static_fields) return false;
        desc->static_field_count = sfc;
        uint32_t idx = 0;
        for (int i = 0; i < cd->field_count; i++) {
            if (cd->fields[i]->type != AST_FIELD_DECL) continue;
            FieldDeclNode *f = &cd->fields[i]->as.field_decl;
            if (!f->is_static) continue;
            desc->static_fields[idx].name = strdup(f->name);
            desc->static_fields[idx].flags = XR_FIELD_STATIC;
            desc->static_fields[idx].default_value = xr_null();
            idx++;
        }
    }
    return true;
}

/* Emit a child XiFunc as a sub-proto and return the proto index.
 * Also populates upvalue descriptors on the child proto. */
static int emit_method_proto(EmitCtx *ctx, uint16_t child_func_idx) {
    XR_DCHECK(child_func_idx < ctx->func->nchildren, "child index out of bounds");
    XiFunc *child = ctx->func->children[child_func_idx];
    XrProto *child_proto = NULL;
    XiEmitStatus cst = xi_emit(child, ctx->isolate, &child_proto);
    if (cst != XI_EMIT_OK || !child_proto) return -1;
    child_proto->shared_offset = ctx->proto->shared_offset;

    for (uint16_t ui = 0; ui < child->ncaptures; ui++) {
        XiCapture *cap = &child->captures[ui];
        uint8_t uv_idx = 0;
        if (cap->source == XI_CAPTURE_SRC_REG) {
            XR_DCHECK(cap->value != NULL,
                      "SRC_REG capture must have parent SSA value");
            uv_idx = reg_of(ctx, cap->value);
            if (ctx->status != XI_EMIT_OK) return -1;
        } else {
            uv_idx = cap->index;
        }
        xr_vm_proto_add_upvalue(child_proto, uv_idx,
                                 0, 0, 0, cap->source, cap->type);
    }
    return xr_vm_proto_add_proto(ctx->proto, child_proto);
}

/* Build XrClassDescriptor from AST, emit child method protos,
 * and generate OP_CLASS_CREATE_FROM_DESCRIPTOR. */
static void emit_class_create(EmitCtx *ctx, XiValue *v,
                               XiClassData *cdata, uint8_t dst) {
    (void)v;
    ClassDeclNode *cd = &cdata->ast->as.class_decl;

    XrClassDescriptor *desc = (XrClassDescriptor *)xr_calloc(
        1, sizeof(XrClassDescriptor));
    if (!desc) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }

    desc->class_name = strdup(cd->name);
    desc->super_name = (cd->super_name && !cd->super_module)
                       ? strdup(cd->super_name) : NULL;
    desc->super_global_index = -1;
    desc->descriptor_version = XR_CLASS_DESCRIPTOR_VERSION;
    desc->clinit_proto_index = -1;
    if (cd->is_abstract) desc->flags |= XR_CLASS_ABSTRACT;
    if (cd->is_final)    desc->flags |= XR_CLASS_FINAL;

    if (!emit_class_collect_fields(ctx, cd, desc)) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    /* Instance methods: fill descriptor entries and emit sub-protos */
    if (cdata->ninst > 0) {
        desc->instance_methods = (XrMethodDescriptorEntry *)xr_calloc(
            cdata->ninst, sizeof(XrMethodDescriptorEntry));
        if (!desc->instance_methods) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        desc->instance_method_count = cdata->ninst;
        uint16_t mi = 0;
        for (int i = 0; i < cd->method_count && mi < cdata->ninst; i++) {
            if (cd->methods[i]->type != AST_METHOD_DECL) continue;
            MethodDeclNode *m = &cd->methods[i]->as.method_decl;
            if (m->is_static || m->is_static_constructor) continue;
            desc->instance_methods[mi].name = strdup(m->name);
            desc->instance_methods[mi].param_count = m->param_count;
            if (m->is_constructor || strcmp(m->name, "constructor") == 0
                || strcmp(m->name, "init") == 0)
                desc->instance_methods[mi].flags |= XMETHOD_FLAG_CONSTRUCTOR;
            if (m->is_private)  desc->instance_methods[mi].flags |= XMETHOD_FLAG_PRIVATE;
            if (m->is_abstract) desc->instance_methods[mi].flags |= XMETHOD_FLAG_ABSTRACT;
            if (m->is_final)    desc->instance_methods[mi].flags |= XMETHOD_FLAG_FINAL;
            XR_DCHECK(cdata->child_idx != NULL, "child_idx must be set");
            int pi = emit_method_proto(ctx, cdata->child_idx[mi]);
            if (pi < 0) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            desc->instance_methods[mi].closure_index = (uint32_t)pi;
            mi++;
        }
    }

    /* Static methods */
    if (cdata->nstat > 0) {
        desc->static_methods = (XrMethodDescriptorEntry *)xr_calloc(
            cdata->nstat, sizeof(XrMethodDescriptorEntry));
        if (!desc->static_methods) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        desc->static_method_count = cdata->nstat;
        uint16_t mi = 0, off = cdata->ninst;
        for (int i = 0; i < cd->method_count && mi < cdata->nstat; i++) {
            if (cd->methods[i]->type != AST_METHOD_DECL) continue;
            MethodDeclNode *m = &cd->methods[i]->as.method_decl;
            if (!m->is_static || m->is_static_constructor) continue;
            desc->static_methods[mi].name = strdup(m->name);
            desc->static_methods[mi].param_count = m->param_count;
            desc->static_methods[mi].flags = XMETHOD_FLAG_STATIC;
            XR_DCHECK(cdata->child_idx != NULL, "child_idx must be set");
            int pi = emit_method_proto(ctx, cdata->child_idx[off + mi]);
            if (pi < 0) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            desc->static_methods[mi].closure_index = (uint32_t)pi;
            mi++;
        }
    }

    /* Add descriptor to constant pool and emit bytecode */
    XrValue desc_val = XR_FROM_PTR(desc);
    int desc_idx = xr_vm_proto_add_constant(ctx->proto, desc_val);
    if (desc_idx < 0 || desc_idx > MAXARG_Bx) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
        return;
    }
    emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
    emit_inst(ctx, CREATE_ABx(OP_CLASS_CREATE_FROM_DESCRIPTOR, dst, desc_idx));
}

/* ========== Instruction Selection ========== */

static void emit_value(EmitCtx *ctx, XiValue *v) {
    if (ctx->status != XI_EMIT_OK) return;

    /* Skip comparison that was absorbed into the block terminator */
    if (v == ctx->fused_cmp) return;

    /* Call-like ops place args at dst+1..dst+nargs.  A recycled low register
     * for dst could overlap with live source arg registers (clobber bug).
     * Force fresh allocation so dst > all previously allocated registers. */
    bool call_like = (v->op == XI_CALL || v->op == XI_CALL_METHOD
                      || v->op == XI_GO || v->op == XI_MULTI_RET);
    uint8_t dst = call_like ? alloc_reg_fresh(ctx, v) : reg_of(ctx, v);
    if (ctx->status != XI_EMIT_OK) return;

    switch (v->op) {
        case XI_CONST: {
            struct XrType *ty = v->type;
            if (!ty) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }

            switch (ty->kind) {
                case XR_KIND_INT: {
                    int64_t val = v->aux_int;
                    if (val >= LOADI_MIN && val <= LOADI_MAX) {
                        emit_inst(ctx, CREATE_AsBx(OP_LOADI, dst, (int)val));
                    } else {
                        int ki = add_const_int(ctx, val);
                        if (ctx->status != XI_EMIT_OK) return;
                        emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
                    }
                    break;
                }
                case XR_KIND_FLOAT: {
                    double fval;
                    memcpy(&fval, &v->aux_int, sizeof(double));
                    int sv = (int)fval;
                    if ((double)sv == fval && sv >= LOADI_MIN && sv <= LOADI_MAX) {
                        emit_inst(ctx, CREATE_AsBx(OP_LOADF, dst, sv));
                    } else {
                        int ki = add_const_float(ctx, fval);
                        if (ctx->status != XI_EMIT_OK) return;
                        emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
                    }
                    break;
                }
                case XR_KIND_BOOL:
                    if (v->aux_int)
                        emit_inst(ctx, CREATE_ABC(OP_LOADTRUE, dst, 0, 0));
                    else
                        emit_inst(ctx, CREATE_ABC(OP_LOADFALSE, dst, 0, 0));
                    break;
                case XR_KIND_NULL:
                    emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
                    break;
                case XR_KIND_STRING: {
                    const char *s = (const char *)v->aux;
                    int ki = add_const_string(ctx, s);
                    if (ctx->status != XI_EMIT_OK) return;
                    emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
                    break;
                }
                default: {
                    /* Generic pointer constant (enum type, etc.) → LOADK */
                    void *ptr = v->aux;
                    if (ptr) {
                        XrValue xv = XR_FROM_PTR(ptr);
                        int ki = xr_vm_proto_add_constant(ctx->proto, xv);
                        if (ki > MAXARG_Bx) {
                            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_CONSTS);
                            return;
                        }
                        emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, ki));
                    } else {
                        emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
                    }
                    break;
                }
            }
            break;
        }

        case XI_PARAM:
            /* Params already in registers; no-op. */
            break;

        case XI_COPY: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            if (dst != src)
                emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
            break;
        }

        /* Binary arithmetic — with instruction fusion for constant operands.
         * ADDI/SUBI/MULI use signed 8-bit immediate (int8_t, -128..127).
         * ADDK/SUBK/MULK/DIVK use constant pool index. */
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
        case XI_BAND: case XI_BOR: case XI_BXOR: case XI_SHL: case XI_SHR: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }

            XiValue *lhs = v->args[0];
            XiValue *rhs = v->args[1];

            /* Try fused immediate form: OP_ADDI/SUBI/MULI with signed 8-bit C */
            bool rhs_is_small_int = (rhs->op == XI_CONST && rhs->type &&
                                     rhs->type->kind == XR_KIND_INT &&
                                     rhs->aux_int >= -128 && rhs->aux_int <= 127);
            bool lhs_is_small_int = (lhs->op == XI_CONST && lhs->type &&
                                     lhs->type->kind == XR_KIND_INT &&
                                     lhs->aux_int >= -128 && lhs->aux_int <= 127);

            if (rhs_is_small_int &&
                (v->op == XI_ADD || v->op == XI_SUB || v->op == XI_MUL)) {
                uint8_t b = reg_of(ctx, lhs);
                if (ctx->status != XI_EMIT_OK) return;
                int8_t imm = (int8_t)rhs->aux_int;
                OpCode fused = v->op == XI_ADD ? OP_ADDI :
                               v->op == XI_SUB ? OP_SUBI : OP_MULI;
                emit_inst(ctx, CREATE_ABC(fused, dst, b, (uint8_t)imm));
                break;
            }

            /* ADD is commutative: try swapping if lhs is the constant */
            if (lhs_is_small_int && v->op == XI_ADD) {
                uint8_t b = reg_of(ctx, rhs);
                if (ctx->status != XI_EMIT_OK) return;
                int8_t imm = (int8_t)lhs->aux_int;
                emit_inst(ctx, CREATE_ABC(OP_ADDI, dst, b, (uint8_t)imm));
                break;
            }
            /* MUL is commutative: try swapping if lhs is the constant */
            if (lhs_is_small_int && v->op == XI_MUL) {
                uint8_t b = reg_of(ctx, rhs);
                if (ctx->status != XI_EMIT_OK) return;
                int8_t imm = (int8_t)lhs->aux_int;
                emit_inst(ctx, CREATE_ABC(OP_MULI, dst, b, (uint8_t)imm));
                break;
            }

            /* Try constant-pool form: ADDK/SUBK/MULK/DIVK/MODK for larger constants */
            bool rhs_is_const_num = (rhs->op == XI_CONST && rhs->type &&
                (rhs->type->kind == XR_KIND_INT || rhs->type->kind == XR_KIND_FLOAT));
            bool lhs_is_const_num = (lhs->op == XI_CONST && lhs->type &&
                (lhs->type->kind == XR_KIND_INT || lhs->type->kind == XR_KIND_FLOAT));
            if (rhs_is_const_num && !rhs_is_small_int &&
                (v->op == XI_ADD || v->op == XI_SUB ||
                 v->op == XI_MUL || v->op == XI_DIV || v->op == XI_MOD)) {
                uint8_t b = reg_of(ctx, lhs);
                if (ctx->status != XI_EMIT_OK) return;
                int ki;
                if (rhs->type->kind == XR_KIND_INT) {
                    ki = add_const_int(ctx, rhs->aux_int);
                } else {
                    double fval;
                    memcpy(&fval, &rhs->aux_int, sizeof(double));
                    ki = add_const_float(ctx, fval);
                }
                if (ctx->status != XI_EMIT_OK) return;
                OpCode kop = v->op == XI_ADD ? OP_ADDK :
                             v->op == XI_SUB ? OP_SUBK :
                             v->op == XI_MUL ? OP_MULK :
                             v->op == XI_DIV ? OP_DIVK : OP_MODK;
                emit_inst(ctx, CREATE_ABC(kop, dst, b, (uint8_t)ki));
                break;
            }
            /* Commutative constant-pool: swap lhs constant for ADD/MUL */
            if (lhs_is_const_num && !lhs_is_small_int &&
                (v->op == XI_ADD || v->op == XI_MUL)) {
                uint8_t b = reg_of(ctx, rhs);
                if (ctx->status != XI_EMIT_OK) return;
                int ki;
                if (lhs->type->kind == XR_KIND_INT) {
                    ki = add_const_int(ctx, lhs->aux_int);
                } else {
                    double fval;
                    memcpy(&fval, &lhs->aux_int, sizeof(double));
                    ki = add_const_float(ctx, fval);
                }
                if (ctx->status != XI_EMIT_OK) return;
                OpCode kop = v->op == XI_ADD ? OP_ADDK : OP_MULK;
                emit_inst(ctx, CREATE_ABC(kop, dst, b, (uint8_t)ki));
                break;
            }

            /* Generic register-register form */
            uint8_t b = reg_of(ctx, lhs);
            uint8_t c = reg_of(ctx, rhs);
            if (ctx->status != XI_EMIT_OK) return;

            OpCode op;
            switch (v->op) {
                case XI_ADD:  op = OP_ADD;  break;
                case XI_SUB:  op = OP_SUB;  break;
                case XI_MUL:  op = OP_MUL;  break;
                case XI_DIV:  op = OP_DIV;  break;
                case XI_MOD:  op = OP_MOD;  break;
                case XI_BAND: op = OP_BAND; break;
                case XI_BOR:  op = OP_BOR;  break;
                case XI_BXOR: op = OP_BXOR; break;
                case XI_SHL:  op = OP_SHL;  break;
                case XI_SHR:  op = OP_SHR;  break;
                default: op = OP_NOP; break;
            }
            emit_inst(ctx, CREATE_ABC(op, dst, b, c));
            break;
        }

        /* Unary ops */
        case XI_NEG:
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            emit_inst(ctx, CREATE_ABC(OP_UNM, dst, reg_of(ctx, v->args[0]), 0));
            break;
        case XI_NOT:
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            emit_inst(ctx, CREATE_ABC(OP_NOT, dst, reg_of(ctx, v->args[0]), 0));
            break;
        case XI_BNOT:
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            emit_inst(ctx, CREATE_ABC(OP_BNOT, dst, reg_of(ctx, v->args[0]), 0));
            break;

        /* Comparison ops -> CMP_* (produce bool in register) */
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE:
        case XI_GT: case XI_GE: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t b = reg_of(ctx, v->args[0]);
            uint8_t c = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;

            switch (v->op) {
                case XI_EQ: emit_inst(ctx, CREATE_ABC(OP_CMP_EQ, dst, b, c)); break;
                case XI_NE: emit_inst(ctx, CREATE_ABC(OP_CMP_NE, dst, b, c)); break;
                case XI_LT: emit_inst(ctx, CREATE_ABC(OP_CMP_LT, dst, b, c)); break;
                case XI_LE: emit_inst(ctx, CREATE_ABC(OP_CMP_LE, dst, b, c)); break;
                /* GT/GE: swap args */
                case XI_GT: emit_inst(ctx, CREATE_ABC(OP_CMP_LT, dst, c, b)); break;
                case XI_GE: emit_inst(ctx, CREATE_ABC(OP_CMP_LE, dst, c, b)); break;
                default: break;
            }
            break;
        }

        /* Type conversion */
        case XI_CONVERT: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            struct XrType *target = v->type;
            if (!target) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            switch (target->kind) {
                case XR_KIND_INT:   emit_inst(ctx, CREATE_ABC(OP_TOINT, dst, src, 0)); break;
                case XR_KIND_FLOAT: emit_inst(ctx, CREATE_ABC(OP_TOFLOAT, dst, src, 0)); break;
                case XR_KIND_STRING:emit_inst(ctx, CREATE_ABC(OP_TOSTRING, dst, src, 0)); break;
                case XR_KIND_BOOL:  emit_inst(ctx, CREATE_ABC(OP_TOBOOL, dst, src, 0)); break;
                default: emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP); return;
            }
            break;
        }

        /* Memory: field access */
        case XI_LOAD_FIELD: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t obj = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            /* aux = property name string (set by lower_member_access) */
            const char *prop = (const char *)v->aux;
            if (prop) {
                int sym = add_symbol(ctx, prop);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABC(OP_GETPROP, dst, obj, (uint8_t)sym));
            } else {
                /* Fallback: numeric field index from aux_int */
                emit_inst(ctx, CREATE_ABC(OP_GETFIELD, dst, obj, (uint8_t)v->aux_int));
            }
            break;
        }
        case XI_STORE_FIELD: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t obj = reg_of(ctx, v->args[0]);
            uint8_t val = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
            const char *prop = (const char *)v->aux;
            if (prop) {
                int sym = add_symbol(ctx, prop);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABC(OP_SETPROP, obj, (uint8_t)sym, val));
            } else {
                emit_inst(ctx, CREATE_ABC(OP_SETFIELD, obj, (uint8_t)v->aux_int, val));
            }
            break;
        }

        /* Indexing */
        case XI_INDEX_GET: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t obj = reg_of(ctx, v->args[0]);
            uint8_t key = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_INDEX_GET, dst, obj, key));
            break;
        }
        case XI_INDEX_SET: {
            if (v->nargs < 3) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t obj = reg_of(ctx, v->args[0]);
            uint8_t key = reg_of(ctx, v->args[1]);
            uint8_t val = reg_of(ctx, v->args[2]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_INDEX_SET, obj, key, val));
            break;
        }

        /* Print */
        case XI_PRINT: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            int flags = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABC(OP_PRINT, src, (uint8_t)(flags & 1),
                                      (uint8_t)((flags >> 1) & 0xFF)));
            break;
        }

        /* Function calls */
        case XI_CALL: {
            /* args[0]=callee, args[1..n]=params
             * aux_int bits 0-7: flags (1=self_call)
             * aux_int bits 8-15: nresults (0 means 1) */
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t nargs = (uint8_t)(v->nargs - 1);
            bool self_call = ((v->aux_int & 0xFF) == 1);
            int nresults = (int)((v->aux_int >> 8) & 0xFF);
            if (nresults == 0) nresults = 1;

            /* Account for arg and result registers in maxstacksize */
            {
                int span = nargs + 1;
                if (nresults > span) span = nresults;
                uint8_t call_top = (uint8_t)(dst + span);
                if (call_top > ctx->max_reg) ctx->max_reg = call_top;
            }

            /* Reserve result registers so the allocator won't reuse them */
            if (nresults > 1 && ctx->next_reg < (uint8_t)(dst + nresults)) {
                ctx->next_reg = (uint8_t)(dst + nresults);
                if (ctx->next_reg > ctx->max_reg)
                    ctx->max_reg = ctx->next_reg;
            }

            if (self_call) {
                /* Recursive self-call: OP_CALLSELF uses frame->closure,
                 * no callee register needed. Use dst as base. */
                for (uint16_t a = 1; a < v->nargs; a++) {
                    uint8_t arg_reg = reg_of(ctx, v->args[a]);
                    if (ctx->status != XI_EMIT_OK) return;
                    uint8_t target = (uint8_t)(dst + a);
                    if (arg_reg != target) {
                        emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
                    }
                }
                emit_inst(ctx, CREATE_ABC(OP_CALLSELF, dst, nargs,
                                          (uint8_t)nresults));
            } else {
                /* Use dst as call base: OP_CALL writes result to the base
                 * register, so using the callee's original register would
                 * clobber the closure (breaks nested calls to the same fn).
                 * dst is always beyond all source registers, so copying
                 * callee->dst and args->dst+1..dst+n is safe. */
                uint8_t callee = reg_of(ctx, v->args[0]);
                if (ctx->status != XI_EMIT_OK) return;
                if (callee != dst) {
                    emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));
                }
                for (uint16_t a = 1; a < v->nargs; a++) {
                    uint8_t arg_reg = reg_of(ctx, v->args[a]);
                    if (ctx->status != XI_EMIT_OK) return;
                    uint8_t target = (uint8_t)(dst + a);
                    if (arg_reg != target) {
                        emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
                    }
                }
                emit_inst(ctx, CREATE_ABC(OP_CALL, dst, nargs,
                                          (uint8_t)nresults));
            }
            break;
        }

        /* Extract i-th result from a multi-return call.
         * The call wrote results to consecutive registers starting at
         * the call's dst; result index i is at call_dst + i. */
        case XI_EXTRACT: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t base = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            int idx = (int)v->aux_int;
            uint8_t src = (uint8_t)(base + idx);
            if (dst != src)
                emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
            break;
        }

        /* Multi-value return: place args in consecutive registers.
         * The block terminator reads base=reg_of(this) + nargs to emit
         * OP_RETURN base, nret. */
        case XI_MULTI_RET: {
            uint8_t top = (uint8_t)(dst + v->nargs);
            if (top > ctx->max_reg) ctx->max_reg = top;
            for (uint16_t a = 0; a < v->nargs; a++) {
                uint8_t arg_reg = reg_of(ctx, v->args[a]);
                if (ctx->status != XI_EMIT_OK) return;
                uint8_t target = (uint8_t)(dst + a);
                if (arg_reg != target)
                    emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }
            break;
        }

        /* Containers */
        case XI_ARRAY_NEW: {
            uint8_t cap = 0;
            if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
                cap = (uint8_t)v->args[0]->aux_int;
            }
            emit_inst(ctx, CREATE_ABC(OP_NEWARRAY, dst, cap, 0));
            break;
        }
        case XI_MAP_NEW: {
            uint8_t cap = 0;
            if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
                cap = (uint8_t)v->args[0]->aux_int;
            }
            uint8_t c_field = (uint8_t)(v->aux_int & 0xFF);
            emit_inst(ctx, CREATE_ABC(OP_NEWMAP, dst, cap, c_field));
            break;
        }

        /* Throw */
        case XI_THROW: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_THROW, src, 0, 0));
            break;
        }

        /* Box/Unbox */
        case XI_BOX: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            /* Use BOX_I64 for ints, BOX_F64 for floats; default to move */
            struct XrType *sty = v->args[0]->type;
            if (sty && sty->kind == XR_KIND_FLOAT)
                emit_inst(ctx, CREATE_ABC(OP_BOX_F64, dst, src, 0));
            else
                emit_inst(ctx, CREATE_ABC(OP_BOX_I64, dst, src, 0));
            break;
        }
        case XI_UNBOX: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            struct XrType *dty = v->type;
            if (dty && dty->kind == XR_KIND_FLOAT)
                emit_inst(ctx, CREATE_ABC(OP_UNBOX_F64, dst, src, 0));
            else
                emit_inst(ctx, CREATE_ABC(OP_UNBOX_I64, dst, src, 0));
            break;
        }

        /* Null check */
        case XI_ISNULL: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_ISNULL_SET, dst, src, 0));
            break;
        }

        /* Coroutine */
        case XI_GO: {
            /* OP_GO: R[A]=task result, R[B]=closure, args at R[B+1..B+C].
             * Use dst for both A and B — VM reads base[B] before writing
             * base[A]. dst is fresh (call_like), so args can safely be
             * placed at dst+1..dst+nargs without clobbering live regs. */
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t nargs = (uint8_t)(v->nargs - 1);

            /* Account for arg registers in maxstacksize */
            {
                uint8_t call_top = (uint8_t)(dst + nargs + 1);
                if (call_top > ctx->max_reg) ctx->max_reg = call_top;
            }

            /* Move callee to dst */
            uint8_t callee = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            if (callee != dst)
                emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));

            /* Move args to dst+1..dst+nargs */
            for (uint16_t a = 1; a < v->nargs; a++) {
                uint8_t arg_reg = reg_of(ctx, v->args[a]);
                if (ctx->status != XI_EMIT_OK) return;
                uint8_t target = (uint8_t)(dst + a);
                if (arg_reg != target)
                    emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }

            emit_inst(ctx, CREATE_ABC(OP_GO, dst, dst, nargs));
            break;
        }
        case XI_AWAIT: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t task = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            /* aux_int flags: bit0=is_any, bit1=is_all, bit2=is_any_success */
            int flags = (int)v->aux_int;
            bool is_any  = (flags & 1) != 0;
            bool is_all  = (flags & 2) != 0;
            bool is_any_success = (flags & 4) != 0;
            if (is_any_success) {
                emit_inst(ctx, CREATE_ABC(OP_AWAIT_ANY, dst, task, 1));
            } else if (is_any) {
                emit_inst(ctx, CREATE_ABC(OP_AWAIT_ANY, dst, task, 0));
            } else if (is_all) {
                emit_inst(ctx, CREATE_ABx(OP_AWAIT_ALL, dst, task));
            } else if (v->nargs >= 2) {
                /* Has timeout argument */
                uint8_t timeout = reg_of(ctx, v->args[1]);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABC(OP_AWAIT_TIMEOUT, dst, task, timeout));
            } else {
                emit_inst(ctx, CREATE_ABC(OP_AWAIT, dst, task, 0));
            }
            break;
        }
        case XI_YIELD:
            emit_inst(ctx, CREATE_ABC(OP_YIELD, 0, 0, 0));
            break;
        case XI_CHAN_NEW: {
            uint8_t buf = 0;
            if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
                buf = (uint8_t)v->args[0]->aux_int;
            }
            emit_inst(ctx, CREATE_ABx(OP_CHAN_NEW, dst, buf));
            break;
        }
        case XI_CHAN_SEND: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t ch = reg_of(ctx, v->args[0]);
            uint8_t val = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_CHAN_SEND, 0, ch, val));
            break;
        }
        case XI_CHAN_RECV: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t ch = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_CHAN_RECV, dst, ch, 0));
            break;
        }

        /* Iteration */
        case XI_RANGE: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t start = reg_of(ctx, v->args[0]);
            uint8_t end = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_NEWRANGE, dst, start, end));
            break;
        }

        /* Slice: OP_SLICE expects start at R[C], end at R[C+1] (consecutive). */
        case XI_SLICE: {
            if (v->nargs < 3) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src    = reg_of(ctx, v->args[0]);
            uint8_t lo_src = reg_of(ctx, v->args[1]);
            uint8_t hi_src = reg_of(ctx, v->args[2]);
            if (ctx->status != XI_EMIT_OK) return;
            /* Allocate consecutive temp pair for start/end */
            if (ctx->next_reg + 2 >= MAX_REGS) {
                emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS); return;
            }
            uint8_t lo_slot = ctx->next_reg;
            ctx->next_reg += 2;
            if (ctx->next_reg > ctx->max_reg)
                ctx->max_reg = ctx->next_reg;
            emit_inst(ctx, CREATE_ABC(OP_MOVE, lo_slot, lo_src, 0));
            emit_inst(ctx, CREATE_ABC(OP_MOVE, (uint8_t)(lo_slot + 1), hi_src, 0));
            emit_inst(ctx, CREATE_ABC(OP_SLICE, dst, src, lo_slot));
            break;
        }

        /* Closure creation: recursively emit child XiFunc, register sub-proto,
         * then emit OP_CLOSURE(A, Bx=proto_index).  If the child has
         * captures, populate upvalue descriptors on the child proto using
         * the parent's (current) register map to resolve SRC_REG indices. */
        case XI_CLOSURE_NEW: {
            XiFunc *child_func = (XiFunc *)v->aux;
            XR_DCHECK(child_func != NULL, "closure child func must not be NULL");

            XrProto *child_proto = NULL;
            XiEmitStatus child_st = xi_emit(child_func, ctx->isolate, &child_proto);
            if (child_st != XI_EMIT_OK || !child_proto) {
                emit_error(ctx, child_st != XI_EMIT_OK
                           ? child_st : XI_EMIT_ERR_INTERNAL);
                return;
            }
            /* Child inherits parent's shared_offset so nested
             * GETSHARED/SETSHARED resolves to the same region. */
            child_proto->shared_offset = ctx->proto->shared_offset;

            /* Populate upvalue descriptors on child proto from captures */
            for (uint16_t ci = 0; ci < child_func->ncaptures; ci++) {
                XiCapture *cap = &child_func->captures[ci];
                uint8_t uv_index = 0;
                if (cap->source == XI_CAPTURE_SRC_REG) {
                    XR_DCHECK(cap->value != NULL,
                              "SRC_REG capture must have parent SSA value");
                    uv_index = reg_of(ctx, cap->value);
                    if (ctx->status != XI_EMIT_OK) return;
                } else {
                    uv_index = cap->index;
                }
                xr_vm_proto_add_upvalue(child_proto, uv_index,
                                         0, 0, 0, cap->source, cap->type);
            }

            /* For captures that need cell indirection (mutable captures),
             * wrap the parent register value in a heap cell before the
             * closure instruction reads it.  OP_CELL_NEW replaces the
             * register content with a cell pointer in-place.
             * Only emit once per value — skip if already cell-wrapped. */
            for (uint16_t ci = 0; ci < child_func->ncaptures; ci++) {
                XiCapture *cap = &child_func->captures[ci];
                if (cap->needs_cell && cap->source == XI_CAPTURE_SRC_REG) {
                    uint8_t reg = reg_of(ctx, cap->value);
                    if (ctx->status != XI_EMIT_OK) return;
                    if (!ctx->cell_wrapped[cap->value->id]) {
                        emit_inst(ctx, CREATE_ABC(OP_CELL_NEW, reg, 0, 0));
                        ctx->cell_wrapped[cap->value->id] = true;
                    }
                }
            }

            int proto_idx = xr_vm_proto_add_proto(ctx->proto, child_proto);
            emit_inst(ctx, CREATE_ABx(OP_CLOSURE, dst, proto_idx));
            break;
        }

        /* Upvalue access */
        case XI_LOAD_UPVAL: {
            int upval_idx = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABC(OP_UPVAL_GET, dst, (uint8_t)upval_idx, 0));

            /* If this capture uses cell indirection, dereference the cell
             * to get the actual value.  The upvalue holds a cell pointer. */
            if (upval_idx < (int)ctx->func->ncaptures &&
                ctx->func->captures[upval_idx].needs_cell) {
                emit_inst(ctx, CREATE_ABC(OP_CELL_GET, dst, dst, 0));
            }
            break;
        }
        case XI_STORE_UPVAL: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t val = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            int upval_idx = (int)v->aux_int;

            /* Get cell reference from upvalue, then store value into cell.
             * OP_UPVAL_GET(tmp, idx) loads the cell pointer;
             * OP_CELL_SET(tmp, val) writes the new value into it. */
            uint8_t cell_reg = dst;  /* reuse dst as temporary */
            emit_inst(ctx, CREATE_ABC(OP_UPVAL_GET, cell_reg,
                                       (uint8_t)upval_idx, 0));
            emit_inst(ctx, CREATE_ABC(OP_CELL_SET, cell_reg, val, 0));
            break;
        }

        /* Shared (module-level) variable access */
        case XI_GET_SHARED: {
            int shared_idx = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABx(OP_GETSHARED, dst, shared_idx));
            break;
        }
        case XI_SET_SHARED: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t val = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            int shared_idx = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABx(OP_SETSHARED, val, shared_idx));
            break;
        }

        /* Method call: args[0]=receiver, args[1..n]=params, aux=method name
         *
         * OP_INVOKE calling convention:
         *   R[A]   = return value position
         *   R[A+1] = receiver (this)
         *   R[A+2..A+1+C] = user arguments
         *   B = method symbol, C = user arg count (excluding this) */
        case XI_CALL_METHOD: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t recv = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            uint8_t nargs = (uint8_t)(v->nargs - 1);

            /* Account for: dst, dst+1 (recv), dst+2..dst+1+nargs */
            {
                uint8_t call_top = (uint8_t)(dst + nargs + 2);
                if (call_top > ctx->max_reg) ctx->max_reg = call_top;
            }

            /* R[dst+1] = receiver */
            if (recv != (uint8_t)(dst + 1))
                emit_inst(ctx, CREATE_ABC(OP_MOVE, dst + 1, recv, 0));

            /* R[dst+2...] = user arguments */
            for (uint16_t a = 1; a < v->nargs; a++) {
                uint8_t arg_reg = reg_of(ctx, v->args[a]);
                if (ctx->status != XI_EMIT_OK) return;
                uint8_t target = (uint8_t)(dst + 1 + a);
                if (arg_reg != target)
                    emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }

            const char *method_name = (const char *)v->aux;
            bool is_super = (v->aux_int == 1);
            if (is_super) {
                /* OP_SUPERINVOKE B = constant pool index (string) */
                int ci = add_const_string(ctx, method_name);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABC(OP_SUPERINVOKE, dst, (uint8_t)ci, nargs));
            } else {
                int sym = add_symbol(ctx, method_name);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABC(OP_INVOKE, dst, (uint8_t)sym, nargs));
            }
            break;
        }

        /* Builtin call: aux_int=builtin_id or aux=name string */
        case XI_CALL_BUILTIN: {
            /* Name-based dispatch (aux is a string identifier) */
            const char *bname = (const char *)v->aux;
            if (bname && strcmp(bname, "Bytes") == 0) {
                /* Bytes(n [, fill]) → OP_BYTES_NEW A B 0
                 * VM expects: R[A]=result, R[A+1..A+B]=args (consecutive).
                 * Allocate a consecutive block: base=result, base+1..=args. */
                uint8_t nargs = (uint8_t)v->nargs;
                if (ctx->next_reg + 1 + nargs >= MAX_REGS) {
                    emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS); return;
                }
                uint8_t base = ctx->next_reg;
                ctx->next_reg += 1 + nargs;
                if (ctx->next_reg > ctx->max_reg)
                    ctx->max_reg = ctx->next_reg;
                for (uint16_t a = 0; a < nargs; a++) {
                    uint8_t arg_r = reg_of(ctx, v->args[a]);
                    if (ctx->status != XI_EMIT_OK) return;
                    emit_inst(ctx, CREATE_ABC(OP_MOVE, (uint8_t)(base + 1 + a), arg_r, 0));
                }
                emit_inst(ctx, CREATE_ABC(OP_BYTES_NEW, base, nargs, 0));
                /* Move result to the SSA-assigned dst register */
                if (dst != base)
                    emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
                break;
            }
            /* Numeric builtin_id dispatch */
            int builtin_id = (int)v->aux_int;
            if (builtin_id == 0 && !bname) {
                /* cancelled() */
                emit_inst(ctx, CREATE_ABC(OP_CANCELLED, dst, 0, 0));
            } else if (!bname) {
                /* Generic: INVOKE_BUILTIN A=base, B=builtin_idx, C=nargs */
                if (v->nargs > 0) {
                    uint8_t base = reg_of(ctx, v->args[0]);
                    if (ctx->status != XI_EMIT_OK) return;
                    emit_inst(ctx, CREATE_ABC(OP_INVOKE_BUILTIN,
                                              base, (uint8_t)builtin_id,
                                              (uint8_t)v->nargs));
                    if (dst != base)
                        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
                }
            }
            break;
        }

        /* String concatenation: STRBUF_NEW + STRBUF_APPEND*n + STRBUF_FINISH */
        case XI_STR_CONCAT: {
            emit_inst(ctx, CREATE_ABC(OP_STRBUF_NEW, dst, 0, 0));
            for (uint16_t a = 0; a < v->nargs; a++) {
                uint8_t part = reg_of(ctx, v->args[a]);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABC(OP_STRBUF_APPEND, dst, part, 0));
            }
            emit_inst(ctx, CREATE_ABC(OP_STRBUF_FINISH, dst, 0, 0));
            break;
        }

        /* Object allocation: aux=class/type name */
        case XI_ALLOC: {
            /* Emit as NEWMAP with field capacity from aux or args[0] */
            uint8_t cap = 0;
            if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
                cap = (uint8_t)v->args[0]->aux_int;
            }
            emit_inst(ctx, CREATE_ABC(OP_NEWMAP, dst, cap, 0));
            break;
        }

        /* Set creation: B = (elem_tid << 2) | flags */
        case XI_SET_NEW: {
            uint8_t b_field = (uint8_t)(v->aux_int & 0xFF);
            emit_inst(ctx, CREATE_ABC(OP_NEWSET, dst, b_field, 0));
            break;
        }

        /* Defer: args[0]=callee; OP_DEFER A=callee_reg B=nargs */
        case XI_DEFER: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t callee = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            uint8_t nargs = (uint8_t)(v->nargs > 1 ? v->nargs - 1 : 0);
            emit_inst(ctx, CREATE_ABC(OP_DEFER, callee, nargs, 0));
            break;
        }

        /* Structured concurrency scope enter/exit */
        case XI_SCOPE_ENTER:
            emit_inst(ctx, CREATE_ABC(OP_SCOPE_ENTER, (uint8_t)v->aux_int, 0, 0));
            break;
        case XI_SCOPE_EXIT:
            emit_inst(ctx, CREATE_ABC(OP_SCOPE_EXIT, (uint8_t)v->aux_int, dst, 0));
            break;

        /* Type check: IS A B C — R[A] = (R[B] is Type[C]) */
        case XI_IS: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            int type_id = (int)v->aux_int;
            emit_inst(ctx, CREATE_ABC(OP_IS, dst, src, (uint8_t)type_id));
            break;
        }

        /* Type cast: runtime typeof check.
         * Unsafe (as):  type mismatch -> throw TypeError
         * Safe   (as?): type mismatch -> load null
         * aux=(void*)XrType*, aux_int: 0=unsafe, 1=safe */
        case XI_AS: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            bool is_safe = (v->aux_int == 1);
            struct XrType *target = (struct XrType *)v->aux;

            /* Map compile-time XrType kind to runtime XrTypeId */
            int tid = -1;
            if (target) {
                switch (target->kind) {
                    case XR_KIND_INT:    tid = XR_TID_INT;    break;
                    case XR_KIND_FLOAT:  tid = XR_TID_FLOAT;  break;
                    case XR_KIND_STRING: tid = XR_TID_STRING; break;
                    case XR_KIND_BOOL:   tid = XR_TID_BOOL;   break;
                    case XR_KIND_JSON:   tid = XR_TID_JSON;   break;
                    case XR_KIND_OBJECT: tid = XR_TID_JSON;   break;
                    case XR_KIND_ARRAY:  tid = XR_TID_ARRAY;  break;
                    default:             tid = -1;            break;
                }
            }

            /* Unknown target type: degenerate to a move */
            if (tid < 0) {
                if (dst != src)
                    emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
                break;
            }

            /* Move source to dst first so the mismatch path can
             * overwrite dst with null (safe) or we throw (unsafe). */
            if (dst != src)
                emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));

            /* OP_TYPEOF tmp, dst, 0 */
            uint8_t tmp = ctx->next_reg++;
            if (tmp >= ctx->max_reg) ctx->max_reg = (uint8_t)(tmp + 1);
            emit_inst(ctx, CREATE_ABC(OP_TYPEOF, tmp, dst, 0));

            /* OP_EQK tmp, K[tid], 1
             *   match:    (1!=1)=false -> don't skip -> execute JMP to ok
             *   mismatch: (0!=1)=true  -> skip JMP   -> fall into error */
            int tid_k = add_const_int(ctx, tid);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_EQK, tmp, (uint8_t)tid_k, 1));
            int ok_jmp_pc = current_pc(ctx);
            emit_inst(ctx, CREATE_sJ(OP_JMP, 0));  /* placeholder */

            if (is_safe) {
                /* Mismatch: load null, then jump past ok */
                emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
                int end_jmp_pc = current_pc(ctx);
                emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
                /* Ok target: next instruction */
                int ok_target = current_pc(ctx);
                XrInstruction *ok_inst = PROTO_CODE_PTR(ctx->proto, ok_jmp_pc);
                *ok_inst = CREATE_sJ(OP_JMP, ok_target - (ok_jmp_pc + 1));
                /* End target: same as ok target (both resolve here) */
                XrInstruction *end_inst = PROTO_CODE_PTR(ctx->proto, end_jmp_pc);
                *end_inst = CREATE_sJ(OP_JMP, ok_target - (end_jmp_pc + 1));
            } else {
                /* Mismatch: throw TypeError */
                const char *tname = target ? xr_type_to_string(target) : "unknown";
                char err_buf[128];
                snprintf(err_buf, sizeof(err_buf),
                         "Type cast failed: expected %s", tname);
                int err_k = add_const_string(ctx, err_buf);
                if (ctx->status != XI_EMIT_OK) return;
                uint8_t err_reg = ctx->next_reg++;
                if (err_reg >= ctx->max_reg) ctx->max_reg = (uint8_t)(err_reg + 1);
                emit_inst(ctx, CREATE_ABx(OP_LOADK, err_reg, err_k));
                emit_inst(ctx, CREATE_ABC(OP_THROW, err_reg, 0, 0));
                /* Ok target: after the throw */
                int ok_target = current_pc(ctx);
                XrInstruction *ok_inst = PROTO_CODE_PTR(ctx->proto, ok_jmp_pc);
                *ok_inst = CREATE_sJ(OP_JMP, ok_target - (ok_jmp_pc + 1));
            }
            break;
        }

        /* Iteration protocol: desugar to method calls.
         * XI_ITER_NEW(coll) → base = coll.iterator() via OP_INVOKE
         * XI_ITER_VALID(iter) → base = iter.hasNext() via OP_INVOKE
         * XI_ITER_NEXT(iter)  → base = iter.next() via OP_INVOKE */
        case XI_ITER_NEW: case XI_ITER_VALID: case XI_ITER_NEXT: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t obj = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;

            const char *method =
                v->op == XI_ITER_NEW   ? "iterator" :
                v->op == XI_ITER_VALID ? "hasNext"  : "next";
            int sym = add_symbol(ctx, method);
            if (ctx->status != XI_EMIT_OK) return;

            /* OP_INVOKE calling convention:
             *   R[base]   = return value
             *   R[base+1] = receiver (this)
             *   B = local symbol index, C = nargs (excl. receiver) */
            uint8_t base = dst;  /* reuse dst as invoke base */
            if (obj != base + 1)
                emit_inst(ctx, CREATE_ABC(OP_MOVE, base + 1, obj, 0));
            emit_inst(ctx, CREATE_ABC(OP_INVOKE, base, (uint8_t)sym, 1));
            break;
        }

        /* Class creation: build XrClassDescriptor from AST, emit child
         * method protos, then OP_CLASS_CREATE_FROM_DESCRIPTOR. */
        case XI_CLASS_CREATE: {
            XiClassData *cdata = (XiClassData *)v->aux;
            XR_DCHECK(cdata != NULL && cdata->ast != NULL,
                      "XI_CLASS_CREATE: missing class data");
            emit_class_create(ctx, v, cdata, dst);
            break;
        }

        /* Exception handling:
         * XI_TRY  → OP_TRY (catch PC patched after emission)
         *           OP_NOP (finally placeholder — 0 if no finally)
         * XI_CATCH → OP_CATCH A=dst_reg
         * XI_FINALLY → OP_FINALLY
         * XI_END_TRY → OP_END_TRY */
        case XI_TRY: {
            XiBlock *catch_blk = (XiBlock *)v->aux;
            XR_DCHECK(catch_blk != NULL, "XI_TRY: missing catch block");
            int try_pc = current_pc(ctx);
            emit_inst(ctx, CREATE_ABx(OP_TRY, 0, 0));  /* patched later */
            emit_inst(ctx, CREATE_ABx(OP_NOP, 0, 0));   /* finally placeholder */

            /* Find the finally block: scan for XI_FINALLY in the function.
             * Only needed when aux_int=1 (has finally). */
            uint32_t fin_bid = 0;
            if (v->aux_int) {
                XiFunc *fn = ctx->func;
                for (uint32_t fbi = 0; fbi < fn->nblocks && fin_bid == 0; fbi++) {
                    const XiBlock *fb = fn->blocks[fbi];
                    if (!fb) continue;
                    for (uint32_t fvi = 0; fvi < fb->nvalues; fvi++) {
                        if (fb->values[fvi] && fb->values[fvi]->op == XI_FINALLY) {
                            fin_bid = fb->id;
                            break;
                        }
                    }
                }
            }
            add_try_patch(ctx, try_pc, catch_blk->id, fin_bid);
            break;
        }
        case XI_CATCH:
            emit_inst(ctx, CREATE_ABC(OP_CATCH, dst, 0, 0));
            break;
        case XI_FINALLY:
            emit_inst(ctx, CREATE_ABC(OP_FINALLY, 0, 0, 0));
            break;
        case XI_END_TRY:
            emit_inst(ctx, CREATE_ABC(OP_END_TRY, 0, 0, 0));
            break;

        /* Builtin assert family:
         * XI_ASSERT    → OP_ASSERT  A=cond, B=loc_const, C=invert_flag
         * XI_ASSERT_EQ → OP_ASSERT_EQ A=actual, B=expected, C=loc_const
         * XI_ASSERT_NE → OP_ASSERT_NE A=actual, B=unexpected, C=loc_const */
        case XI_ASSERT: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t cond = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            int loc_k = add_const_string(ctx, (const char *)v->aux);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_ASSERT, cond, (uint8_t)loc_k,
                                       (uint8_t)v->aux_int));
            break;
        }
        case XI_ASSERT_EQ: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t actual = reg_of(ctx, v->args[0]);
            uint8_t expected = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
            int loc_k = add_const_string(ctx, (const char *)v->aux);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_ASSERT_EQ, actual, expected,
                                       (uint8_t)loc_k));
            break;
        }
        case XI_ASSERT_NE: {
            if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t actual = reg_of(ctx, v->args[0]);
            uint8_t unexpected = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
            int loc_k = add_const_string(ctx, (const char *)v->aux);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_ASSERT_NE, actual, unexpected,
                                       (uint8_t)loc_k));
            break;
        }

        /* typeof(x) → OP_TYPEOF; typename(x) → OP_TYPENAME (aux_int=1) */
        case XI_TYPEOF: {
            if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
            uint8_t src = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            uint8_t tyop = v->aux_int == 1 ? OP_TYPENAME : OP_TYPEOF;
            emit_inst(ctx, CREATE_ABC(tyop, dst, src, 0));
            break;
        }

        /* Runtime global variable lookup → OP_GETBUILTIN A=dst, Bx=global_index */
        case XI_GET_BUILTIN:
            emit_inst(ctx, CREATE_ABx(OP_GETBUILTIN, dst, (int)v->aux_int));
            break;

        /* AOT-only: cross-module import reference.  VM handles imports via
         * OP_IMPORT/OP_GETPROP so this op should never appear in the VM path.
         * Emit LOADNULL as a safe fallback. */
        case XI_IMPORT_REF:
            emit_inst(ctx, CREATE_ABx(OP_LOADNULL, dst, 0));
            break;

        default:
            emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
            return;
    }
}

/* ========== Block Emission ========== */

/* Check if a comparison value is only used as the block control (no other
 * consumers).  If so, we can fuse it into the branch-form opcode. */
static bool can_fuse_cmp(XiBlock *blk, XiValue *ctrl) {
    XR_DCHECK(ctrl != NULL, "ctrl must not be NULL");
    uint16_t op = ctrl->op;
    if (op < XI_EQ || op > XI_GE || ctrl->nargs < 2) return false;
    /* Ensure no other value in this block uses the comparison result */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (v == ctrl) continue;
        for (uint16_t a = 0; a < v->nargs; a++) {
            if (v->args[a] == ctrl) return false;
        }
    }
    return true;
}

static void emit_block(EmitCtx *ctx, XiBlock *blk, XiBlock *next_blk) {
    if (ctx->status != XI_EMIT_OK) return;

    /* Record block start PC */
    XR_DCHECK(blk->id < ctx->block_pc_size, "block_id out of range");
    ctx->block_pc[blk->id] = current_pc(ctx);

    /* Detect fuseable comparison for IF blocks */
    ctx->fused_cmp = NULL;
    if (blk->kind == XI_BLOCK_IF && blk->control)
        if (can_fuse_cmp(blk, blk->control))
            ctx->fused_cmp = blk->control;

    /* Emit instruction values with register recycling */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        ctx->current_ordinal++;
        ctx->current_line = (int)blk->values[i]->line;
        emit_value(ctx, blk->values[i]);
        if (ctx->status != XI_EMIT_OK) return;
        /* Recycle registers of args whose last use was this instruction.
         * Skip recycling for the fused comparison's args — they are still
         * needed by the branch-form opcode in the terminator. */
        if (ctx->last_use && blk->values[i] != ctx->fused_cmp)
            try_free_args(ctx, blk->values[i]);
    }

    ctx->current_ordinal++;  /* ordinal for terminator */
    /* Emit terminator */
    switch (blk->kind) {
        case XI_BLOCK_RETURN:
            if (blk->control && blk->control->op == XI_MULTI_RET) {
                /* Multi-value return: values in consecutive regs */
                uint8_t base = reg_of(ctx, blk->control);
                if (ctx->status != XI_EMIT_OK) return;
                uint8_t nret = (uint8_t)blk->control->nargs;
                emit_inst(ctx, CREATE_ABC(OP_RETURN, base, nret, 0));
            } else if (blk->control) {
                uint8_t r = reg_of(ctx, blk->control);
                if (ctx->status != XI_EMIT_OK) return;
                /* If the return value was cell-wrapped for closure capture,
                 * dereference the cell to return the actual value. */
                if (blk->control->id < ctx->reg_map_size &&
                    ctx->cell_wrapped[blk->control->id]) {
                    emit_inst(ctx, CREATE_ABC(OP_CELL_GET, r, r, 0));
                }
                emit_inst(ctx, CREATE_ABC(OP_RETURN1, r, 0, 0));
            } else {
                emit_inst(ctx, CREATE_ABC(OP_RETURN0, 0, 0, 0));
            }
            break;

        case XI_BLOCK_PLAIN: {
            XiBlock *succ = blk->succs[0];
            if (!succ) break;
            /* Emit phi moves for successor */
            emit_phi_moves(ctx, blk, succ);
            if (ctx->status != XI_EMIT_OK) return;
            /* Jump to successor (skip if it's the next block in RPO) */
            if (succ != next_blk) {
                int jmp_pc = current_pc(ctx);
                emit_inst(ctx, CREATE_sJ(OP_JMP, 0));  /* placeholder */
                add_patch(ctx, jmp_pc, succ->id);
            }
            break;
        }

        case XI_BLOCK_IF: {
            if (!blk->control) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }

            XiBlock *then_b = blk->succs[0];
            XiBlock *else_b = blk->succs[1];
            XR_DCHECK(then_b && else_b, "IF block missing successor");

            if (ctx->fused_cmp) {
                /* Fused comparison-branch: emit OP_LT/LE/EQ etc. directly.
                 * Saves one instruction vs OP_CMP_* + OP_TEST. */
                XiValue *cmp = ctx->fused_cmp;
                XiValue *lhs = cmp->args[0];
                XiValue *rhs = cmp->args[1];
                uint8_t a = reg_of(ctx, lhs);
                uint8_t b = reg_of(ctx, rhs);
                if (ctx->status != XI_EMIT_OK) return;

                /* Determine branch-form opcode and sense.  GT/GE swap args. */
                OpCode branch_op;
                int k = 0;
                bool swap = false;
                switch (cmp->op) {
                    case XI_LT: branch_op = OP_LT; break;
                    case XI_LE: branch_op = OP_LE; break;
                    case XI_GT: branch_op = OP_LT; swap = true; break;
                    case XI_GE: branch_op = OP_LE; swap = true; break;
                    case XI_EQ: branch_op = OP_EQ; break;
                    case XI_NE: branch_op = OP_EQ; k = 1; break;
                    default:    branch_op = OP_EQ; break;
                }
                if (swap) { uint8_t t = a; a = b; b = t; }

                /* Try immediate form (OP_LTI/LEI/EQI) when RHS is small int */
                bool is_imm = false;
                XiValue *imm_arg = swap ? lhs : rhs;   /* the "B" operand */
                XiValue *reg_arg = swap ? rhs : lhs;    /* the "A" operand */
                if (imm_arg->op == XI_CONST && imm_arg->type &&
                    imm_arg->type->kind == XR_KIND_INT &&
                    imm_arg->aux_int >= -128 && imm_arg->aux_int <= 127 &&
                    (branch_op == OP_LT || branch_op == OP_LE || branch_op == OP_EQ)) {
                    OpCode imm_op = branch_op == OP_LT ? OP_LTI :
                                   branch_op == OP_LE ? OP_LEI : OP_EQI;
                    uint8_t ra = reg_of(ctx, reg_arg);
                    if (ctx->status != XI_EMIT_OK) return;
                    int8_t imm = (int8_t)imm_arg->aux_int;
                    emit_inst(ctx, CREATE_ABC(imm_op, ra, (uint8_t)imm, (uint8_t)k));
                    is_imm = true;
                }

                if (!is_imm) {
                    emit_inst(ctx, CREATE_ABC(branch_op, a, b, (uint8_t)k));
                }
            } else {
                /* Non-fused path: TEST cond, skip next if cond is false */
                uint8_t cond = reg_of(ctx, blk->control);
                if (ctx->status != XI_EMIT_OK) return;
                emit_inst(ctx, CREATE_ABC(OP_TEST, cond, 0, 0));
            }

            /* JMP -> else block */
            int else_jmp_pc = current_pc(ctx);
            emit_inst(ctx, CREATE_sJ(OP_JMP, 0));  /* placeholder */
            add_patch(ctx, else_jmp_pc, else_b->id);

            /* Phi moves for then path */
            emit_phi_moves(ctx, blk, then_b);
            if (ctx->status != XI_EMIT_OK) return;

            /* Jump to then if not fallthrough */
            if (then_b != next_blk) {
                int then_jmp_pc = current_pc(ctx);
                emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
                add_patch(ctx, then_jmp_pc, then_b->id);
            }
            break;
        }

        case XI_BLOCK_UNREACHABLE:
            /* Emit a NOP as placeholder */
            emit_inst(ctx, CREATE_ABC(OP_NOP, 0, 0, 0));
            break;

        default:
            break;
    }
}

/* ========== Jump Patching ========== */

static void patch_jumps(EmitCtx *ctx) {
    for (uint32_t i = 0; i < ctx->npatch; i++) {
        int pc = ctx->patches[i].pc;
        uint32_t bid = ctx->patches[i].target_bid;

        XR_DCHECK(bid < ctx->block_pc_size, "patch: bad block id");
        int target_pc = ctx->block_pc[bid];

        if (target_pc < 0) {
            /* Unreachable target — should not happen in valid IR.
             * Point to current PC as fallback. */
            target_pc = pc + 1;
        }

        /* sJ = target_pc - (pc + 1) */
        int offset = target_pc - (pc + 1);
        XrInstruction *inst = PROTO_CODE_PTR(ctx->proto, pc);
        *inst = CREATE_sJ(OP_JMP, offset);
    }

    /* Patch OP_TRY instructions: Bx = absolute catch PC;
     * Patch the following NOP with finally PC (Bx). */
    for (uint32_t i = 0; i < ctx->ntry_patch; i++) {
        int pc = ctx->try_patches[i].pc;
        uint32_t bid = ctx->try_patches[i].target_bid;
        uint32_t fbid = ctx->try_patches[i].finally_bid;
        XR_DCHECK(bid < ctx->block_pc_size, "try_patch: bad catch block id");
        int target_pc = ctx->block_pc[bid];
        if (target_pc < 0) target_pc = pc + 1;
        XrInstruction *inst = PROTO_CODE_PTR(ctx->proto, pc);
        *inst = CREATE_ABx(OP_TRY, 0, target_pc);

        /* Patch NOP at pc+1 with finally absolute PC */
        if (fbid > 0 && fbid < ctx->block_pc_size) {
            int fin_pc = ctx->block_pc[fbid];
            if (fin_pc >= 0) {
                XrInstruction *fin_inst = PROTO_CODE_PTR(ctx->proto, pc + 1);
                *fin_inst = CREATE_ABx(OP_NOP, 0, fin_pc);
            }
        }
    }
}

/* ========== Slot Map Generation ========== */

/* Build an XiSlotMap from the emitter's register assignment.
 * Scans all Xi IR values and records their bytecode register mappings.
 * Returns NULL on allocation failure (non-fatal). */
static XiSlotMap *build_slot_map(EmitCtx *ctx) {
    XiFunc *f = ctx->func;
    XR_DCHECK(f != NULL, "build_slot_map: NULL func");

    /* Count mapped values */
    uint32_t count = 0;
    for (uint32_t i = 0; i < ctx->reg_map_size; i++) {
        if (ctx->reg_map[i] != NO_REG)
            count++;
    }
    if (count == 0) return NULL;

    XiSlotMap *map = (XiSlotMap *)xr_calloc(1, sizeof(XiSlotMap));
    if (!map) return NULL;

    map->entries = (XiSlotMapEntry *)xr_malloc(count * sizeof(XiSlotMapEntry));
    if (!map->entries) {
        xr_free(map);
        return NULL;
    }
    map->capacity = count;

    /* Fill entries from all blocks' values */
    uint32_t idx = 0;
    for (uint32_t b = 0; b < f->nblocks && idx < count; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;
        for (uint32_t vi = 0; vi < blk->nvalues && idx < count; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->id >= ctx->reg_map_size) continue;
            uint8_t reg = ctx->reg_map[v->id];
            if (reg == NO_REG) continue;

            map->entries[idx].value_id = v->id;
            map->entries[idx].bc_slot = reg;
            /* Derive XR_TAG from Xi IR type */
            uint8_t tag = 5; /* XR_TAG_PTR default */
            if (v->type) {
                switch (v->type->kind) {
                    case XR_KIND_INT:   tag = 3; break; /* XR_TAG_I64 */
                    case XR_KIND_FLOAT: tag = 4; break; /* XR_TAG_F64 */
                    case XR_KIND_BOOL:  tag = 1; break; /* XR_TAG_BOOL */
                    case XR_KIND_NULL:
                    case XR_KIND_VOID:  tag = 0; break; /* XR_TAG_NULL */
                    default:            tag = 5; break; /* XR_TAG_PTR */
                }
            }
            map->entries[idx].xr_tag = tag;
            /* bc_pc: use block's start PC if available */
            map->entries[idx].bc_pc = (blk->id < ctx->block_pc_size &&
                                       ctx->block_pc[blk->id] >= 0)
                                          ? (uint32_t)ctx->block_pc[blk->id]
                                          : 0;
            idx++;
        }
    }
    map->count = idx;
    return map;
}

/* ========== Public API ========== */

XR_FUNC XiEmitStatus xi_emit(XiFunc *f, struct XrayIsolate *isolate,
                              struct XrProto **out_proto) {
    XR_DCHECK(f != NULL, "xi_emit: NULL func");
    XR_DCHECK(out_proto != NULL, "xi_emit: NULL out_proto");
    *out_proto = NULL;

    /* Run prerequisite analyses */
    uint32_t rpo_count = xi_compute_rpo(f);
    if (rpo_count == 0) return XI_EMIT_ERR_INTERNAL;

    /* Exception handler blocks are unreachable via normal CFG edges.
     * Scan for XI_TRY ops and assign RPO numbers to catch targets and
     * all transitively reachable blocks (catch body, finally, merge). */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (v->op == XI_TRY && v->aux) {
                XiBlock *catch_blk = (XiBlock *)v->aux;
                /* BFS from catch block to assign RPO to all reachable
                 * exception-related blocks (catch, finally, merge). */
                XiBlock *queue[64];
                int qhead = 0, qtail = 0;
                if (catch_blk->rpo == 0) {
                    catch_blk->rpo = ++rpo_count;
                    queue[qtail++] = catch_blk;
                }
                while (qhead < qtail && qtail < 64) {
                    XiBlock *cur = queue[qhead++];
                    for (int s = 0; s < 2; s++) {
                        XiBlock *succ = cur->succs[s];
                        if (succ && succ->rpo == 0) {
                            succ->rpo = ++rpo_count;
                            queue[qtail++] = succ;
                        }
                    }
                }
            }
        }
    }

    /* Build RPO order array */
    XiBlock **rpo_order = (XiBlock **)xr_calloc(rpo_count + 1,
                                                  sizeof(XiBlock *));
    if (!rpo_order) return XI_EMIT_ERR_INTERNAL;

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (blk->rpo > 0 && blk->rpo <= rpo_count)
            rpo_order[blk->rpo] = blk;
    }

    /* Initialize context */
    EmitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = f;
    ctx.isolate = isolate;
    ctx.proto = xr_vm_proto_new();
    if (!ctx.proto) { xr_free(rpo_order); return XI_EMIT_ERR_INTERNAL; }

    /* Allocate shared variable slots in the isolate's shared_array
     * BEFORE emitting blocks, so child protos can inherit the offset. */
    if (isolate && f->nshared > 0) {
        ctx.proto->shared_offset = isolate->vm.shared.count;
        int total = isolate->vm.shared.count + f->nshared;
        isolate->vm.shared.count = total;
        xr_shared_array_ensure(&isolate->vm.shared, total - 1);
    }

    ctx.rpo_order = rpo_order;
    ctx.rpo_count = rpo_count;

    /* Allocate register map */
    ctx.reg_map_size = f->next_value_id;
    ctx.reg_map = (uint8_t *)xr_malloc(ctx.reg_map_size);
    if (!ctx.reg_map) {
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    memset(ctx.reg_map, NO_REG, ctx.reg_map_size);

    /* Allocate block PC map */
    ctx.block_pc_size = f->next_block_id;
    ctx.block_pc = (int *)xr_malloc(ctx.block_pc_size * sizeof(int));
    if (!ctx.block_pc) {
        xr_free(ctx.reg_map);
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    for (uint32_t i = 0; i < ctx.block_pc_size; i++)
        ctx.block_pc[i] = -1;

    /* Allocate last-use ordinal map for register recycling */
    ctx.last_use = (uint32_t *)xr_calloc(ctx.reg_map_size, sizeof(uint32_t));
    if (!ctx.last_use) {
        xr_free(ctx.block_pc);
        xr_free(ctx.reg_map);
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }
    compute_last_use(&ctx);

    /* Allocate cell-wrapped tracker for dedup of OP_CELL_NEW */
    ctx.cell_wrapped = (bool *)xr_calloc(ctx.reg_map_size, sizeof(bool));
    if (!ctx.cell_wrapped) {
        xr_free(ctx.last_use);
        xr_free(ctx.block_pc);
        xr_free(ctx.reg_map);
        xr_vm_proto_free(ctx.proto);
        xr_free(rpo_order);
        return XI_EMIT_ERR_INTERNAL;
    }

    alloc_registers(&ctx);
    if (ctx.status != XI_EMIT_OK) goto cleanup;

    /* Emit blocks in RPO order */
    for (uint32_t r = 1; r <= rpo_count; r++) {
        XiBlock *blk = rpo_order[r];
        if (!blk) continue;
        XiBlock *next_blk = (r + 1 <= rpo_count) ? rpo_order[r + 1] : NULL;

        /* Before emitting block, emit phi moves from IF predecessors.
         * For PLAIN blocks, phi moves are emitted by the predecessor.
         * For IF predecessors, phi moves for the else path need to be
         * emitted at the else block's start. */
        emit_block(&ctx, blk, next_blk);
        if (ctx.status != XI_EMIT_OK) goto cleanup;
    }

    /* Phase 3: Patch jump targets */
    patch_jumps(&ctx);

    /* Finalize proto metadata */
    ctx.proto->maxstacksize = ctx.max_reg;
    /* Use f->nparams if set by lowerer; otherwise count XI_PARAM ops.
     * Vararg-only functions (e.g. fn(...nums)) have nparams=0 legitimately,
     * so only fall back to counting when nparams==0 and NOT vararg. */
    if (f->nparams > 0 || f->is_vararg) {
        ctx.proto->numparams = f->nparams;
    } else if (f->entry) {
        uint16_t pc = 0;
        for (uint32_t i = 0; i < f->entry->nvalues; i++) {
            if (f->entry->values[i]->op == XI_PARAM) pc++;
        }
        ctx.proto->numparams = pc;
    }
    ctx.proto->is_vararg = f->is_vararg;
    ctx.proto->entry_type = f->entry_type;
    ctx.proto->min_params = f->min_params;

    /* Build slot map for JIT (non-fatal if allocation fails) */
    XiSlotMap *slot_map = build_slot_map(&ctx);
    if (slot_map)
        ctx.proto->xi_slot_map = slot_map;

cleanup:;
    XiEmitStatus result = ctx.status;
    if (result == XI_EMIT_OK) {
        *out_proto = ctx.proto;
    } else {
        xr_vm_proto_free(ctx.proto);
    }
    xr_free(ctx.cell_wrapped);
    xr_free(ctx.last_use);
    xr_free(ctx.reg_map);
    xr_free(ctx.block_pc);
    xr_free(ctx.patches);
    xr_free(ctx.try_patches);
    xr_free(rpo_order);
    return result;
}

XR_FUNC void xi_emit_attach_ir(struct XrProto *proto, XiFunc *ir) {
    XR_DCHECK(proto != NULL, "xi_emit_attach_ir: NULL proto");
    XR_DCHECK(proto->xi_func == NULL, "xi_emit_attach_ir: proto already has xi_func");
    proto->xi_func = ir;
}

XR_FUNC const char *xi_emit_status_str(XiEmitStatus s) {
    switch (s) {
        case XI_EMIT_OK:                return "OK";
        case XI_EMIT_ERR_TOO_MANY_REGS: return "too many registers (>255)";
        case XI_EMIT_ERR_TOO_MANY_CONSTS: return "constant pool overflow";
        case XI_EMIT_ERR_UNSUPPORTED_OP:  return "unsupported Xi IR operation";
        case XI_EMIT_ERR_INTERNAL:        return "internal emitter error";
    }
    return "unknown error";
}
