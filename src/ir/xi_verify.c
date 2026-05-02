/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_verify.c - IR verification pass for Xi IR
 *
 * Validates structural and semantic invariants of SSA functions
 * produced by xi_lower.c and transformed by xi_opt.c.
 */

#include "xi_verify.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ========== Error Reporting ========== */

typedef struct {
    char *buf;
    int size;
    bool failed;
} VerifyCtx;

static void verr(VerifyCtx *ctx, const char *fmt, ...) {
    if (ctx->failed) return;  /* report first error only */
    ctx->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->buf, (size_t)ctx->size, fmt, ap);
    va_end(ap);
}

/* ========== Individual Checks ========== */

/* Check 1: function-level invariants */
static void verify_func(VerifyCtx *ctx, const XiFunc *f) {
    if (!f->name) {
        verr(ctx, "function has NULL name");
        return;
    }
    if (f->nblocks == 0) {
        verr(ctx, "func '%s': no blocks", f->name);
        return;
    }
    if (!f->entry) {
        verr(ctx, "func '%s': NULL entry block", f->name);
        return;
    }
    if (f->entry != f->blocks[0]) {
        verr(ctx, "func '%s': entry != blocks[0]", f->name);
        return;
    }
}

/* Check 2: entry block must have 0 predecessors */
static void verify_entry(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;
    if (f->entry->npreds != 0) {
        verr(ctx, "func '%s': entry block b%u has %u predecessors (expected 0)",
             f->name, f->entry->id, f->entry->npreds);
    }
}

/* Check 3: block-level invariants */
static void verify_block(VerifyCtx *ctx, const XiFunc *f, const XiBlock *blk) {
    if (ctx->failed) return;
    XR_DCHECK(blk != NULL, "verify_block: NULL block");

    /* Block must belong to this function */
    if (blk->func != f) {
        verr(ctx, "func '%s': block b%u has wrong func pointer",
             f->name, blk->id);
        return;
    }

    /* Check terminator consistency */
    switch (blk->kind) {
        case XI_BLOCK_PLAIN:
            if (!blk->succs[0] && blk->nvalues > 0) {
                /* Plain block with no successor and values is suspicious
                 * but may be valid (dead code). Skip. */
            }
            break;
        case XI_BLOCK_IF:
            if (!blk->control) {
                verr(ctx, "func '%s': IF block b%u has NULL control",
                     f->name, blk->id);
                return;
            }
            if (!blk->succs[0] || !blk->succs[1]) {
                verr(ctx, "func '%s': IF block b%u missing successor(s)",
                     f->name, blk->id);
                return;
            }
            break;
        case XI_BLOCK_RETURN:
            /* control may be NULL for void returns */
            break;
        case XI_BLOCK_UNREACHABLE:
            break;
        default:
            verr(ctx, "func '%s': block b%u has invalid kind %u",
                 f->name, blk->id, blk->kind);
            return;
    }
}

/* Check 4: value-level invariants */
static void verify_value(VerifyCtx *ctx, const XiFunc *f,
                          const XiBlock *blk, const XiValue *v) {
    if (ctx->failed) return;
    XR_DCHECK(v != NULL, "verify_value: NULL value");

    /* Type must be non-NULL */
    if (!v->type) {
        verr(ctx, "func '%s': value v%u in b%u has NULL type",
             f->name, v->id, blk->id);
        return;
    }

    /* Op must be in valid range */
    if (v->op >= XI_OP_COUNT) {
        verr(ctx, "func '%s': value v%u in b%u has invalid op %u",
             f->name, v->id, blk->id, v->op);
        return;
    }

    /* Block back-pointer must match */
    if (v->block != blk) {
        verr(ctx, "func '%s': value v%u claims block b%u but is in b%u",
             f->name, v->id,
             v->block ? v->block->id : 9999, blk->id);
        return;
    }

    /* Args array consistency */
    if (v->nargs > 0 && !v->args) {
        verr(ctx, "func '%s': value v%u in b%u has %u args but NULL args ptr",
             f->name, v->id, blk->id, v->nargs);
        return;
    }

    /* Each arg should be a plausible value (non-NULL, has type).
     * Exception: CLOSURE_NEW args may be NULL for upvalue-chain captures
     * that have no local SSA value (source is parent's upvalue, not a reg). */
    for (uint16_t a = 0; a < v->nargs; a++) {
        if (!v->args[a]) {
            if (v->op == XI_CLOSURE_NEW)
                continue;  /* NULL capture arg is valid */
            verr(ctx, "func '%s': value v%u in b%u arg[%u] is NULL",
                 f->name, v->id, blk->id, a);
            return;
        }
        if (!v->args[a]->type) {
            verr(ctx, "func '%s': value v%u in b%u arg[%u] (v%u) has NULL type",
                 f->name, v->id, blk->id, a, v->args[a]->id);
            return;
        }
    }
}

/* Check 5: phi node invariants */
static void verify_phi(VerifyCtx *ctx, const XiFunc *f,
                        const XiBlock *blk, const XiPhi *phi) {
    if (ctx->failed) return;
    XR_DCHECK(phi != NULL, "verify_phi: NULL phi");

    /* Phi must have op == XI_PHI */
    if (phi->value.op != XI_PHI) {
        verr(ctx, "func '%s': phi in b%u has op %u (expected XI_PHI=%u)",
             f->name, blk->id, phi->value.op, XI_PHI);
        return;
    }

    /* Phi type must be non-NULL */
    if (!phi->value.type) {
        verr(ctx, "func '%s': phi v%u in b%u has NULL type",
             f->name, phi->value.id, blk->id);
        return;
    }

    /* Phi arg count must match predecessor count */
    if (phi->value.nargs != blk->npreds) {
        verr(ctx, "func '%s': phi v%u in b%u has %u args but block has %u preds",
             f->name, phi->value.id, blk->id,
             phi->value.nargs, blk->npreds);
        return;
    }

    /* Each phi arg must be non-NULL with valid type */
    for (uint16_t a = 0; a < phi->value.nargs; a++) {
        if (!phi->value.args[a]) {
            verr(ctx, "func '%s': phi v%u in b%u arg[%u] is NULL",
                 f->name, phi->value.id, blk->id, a);
            return;
        }
    }
}

/* Check 6: CFG edge consistency — each succ lists blk as a pred */
static void verify_cfg_edges(VerifyCtx *ctx, const XiFunc *f,
                              const XiBlock *blk) {
    if (ctx->failed) return;

    for (int s = 0; s < 2; s++) {
        XiBlock *succ = blk->succs[s];
        if (!succ) continue;

        bool found = false;
        for (uint16_t p = 0; p < succ->npreds; p++) {
            if (succ->preds[p] == blk) {
                found = true;
                break;
            }
        }
        if (!found) {
            verr(ctx, "func '%s': b%u has successor b%u but is not in its pred list",
                 f->name, blk->id, succ->id);
            return;
        }
    }
}

/* Check 7: unique value IDs within function */
static void verify_unique_ids(VerifyCtx *ctx, const XiFunc *f) {
    if (ctx->failed) return;

    /* Use a simple O(n) scan; for typical function sizes this is fine. */
    uint32_t max_id = f->next_value_id;

    /* Track seen IDs with a bitmap if small enough, else skip. */
    if (max_id > 10000) return;  /* skip for very large functions */

    /* Stack-allocate a bitset. Max ~1.2 KB for 10000 IDs. */
    uint8_t seen[1250];
    memset(seen, 0, sizeof(seen));

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            uint32_t vid = blk->values[i]->id;
            if (vid >= max_id) {
                verr(ctx, "func '%s': value v%u >= next_value_id %u",
                     f->name, vid, max_id);
                return;
            }
            uint32_t byte = vid / 8;
            uint8_t bit = (uint8_t)(1 << (vid & 7));
            if (byte < sizeof(seen)) {
                if (seen[byte] & bit) {
                    verr(ctx, "func '%s': duplicate value ID v%u",
                         f->name, vid);
                    return;
                }
                seen[byte] |= bit;
            }
        }

        /* Also check phi IDs */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            uint32_t vid = phi->value.id;
            if (vid >= max_id) {
                verr(ctx, "func '%s': phi v%u >= next_value_id %u",
                     f->name, vid, max_id);
                return;
            }
            uint32_t byte = vid / 8;
            uint8_t bit = (uint8_t)(1 << (vid & 7));
            if (byte < sizeof(seen)) {
                if (seen[byte] & bit) {
                    verr(ctx, "func '%s': duplicate phi ID v%u",
                         f->name, vid);
                    return;
                }
                seen[byte] |= bit;
            }
        }
    }
}

/* ========== Public API ========== */

XR_FUNC bool xi_verify(const XiFunc *f, char *errbuf, int errbuf_size) {
    XR_DCHECK(errbuf != NULL, "xi_verify: NULL errbuf");
    XR_DCHECK(errbuf_size > 0, "xi_verify: errbuf_size <= 0");

    if (!f) {
        snprintf(errbuf, (size_t)errbuf_size, "NULL function pointer");
        return false;
    }

    VerifyCtx ctx = { .buf = errbuf, .size = errbuf_size, .failed = false };
    errbuf[0] = '\0';

    /* Function-level */
    verify_func(&ctx, f);
    if (ctx.failed) return false;

    /* Entry block */
    verify_entry(&ctx, f);
    if (ctx.failed) return false;

    /* Per-block checks */
    for (uint32_t b = 0; b < f->nblocks && !ctx.failed; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) {
            verr(&ctx, "func '%s': blocks[%u] is NULL", f->name, b);
            break;
        }

        verify_block(&ctx, f, blk);
        if (ctx.failed) break;

        /* Values in this block */
        for (uint32_t i = 0; i < blk->nvalues && !ctx.failed; i++) {
            if (!blk->values[i]) {
                verr(&ctx, "func '%s': b%u values[%u] is NULL",
                     f->name, blk->id, i);
                break;
            }
            verify_value(&ctx, f, blk, blk->values[i]);
        }

        /* Phi nodes */
        for (XiPhi *phi = blk->phis; phi && !ctx.failed; phi = phi->next) {
            verify_phi(&ctx, f, blk, phi);
        }

        /* CFG edges */
        verify_cfg_edges(&ctx, f, blk);
    }

    /* Unique IDs */
    if (!ctx.failed) {
        verify_unique_ids(&ctx, f);
    }

    return !ctx.failed;
}
