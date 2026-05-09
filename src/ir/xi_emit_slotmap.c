/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit_slotmap.c - Slot map generation for JIT IC speculation
 *
 * Builds XiSlotMap from the emitter's register assignment, mapping
 * Xi IR value IDs to bytecode register slots with type tags and
 * instruction offsets for IC-guided deoptimization.
 */

#include "xi_emit_internal.h"
#include "../runtime/value/xtype.h"

/* ========== Slot Map Generation ========== */

/* Build an XiSlotMap from the emitter's register assignment.
 * Scans all Xi IR values and records their bytecode register mappings.
 * Returns NULL on allocation failure (non-fatal). */
XR_FUNC XiSlotMap *build_slot_map(EmitCtx *ctx) {
    XiFunc *f = ctx->func;
    XR_DCHECK(f != NULL, "build_slot_map: NULL func");

    /* Count mapped values */
    uint32_t count = 0;
    for (uint32_t i = 0; i < ctx->reg_map_size; i++) {
        if (ctx->reg_map[i] != NO_REG)
            count++;
    }
    if (count == 0)
        return NULL;

    XiSlotMap *map = (XiSlotMap *) xr_calloc(1, sizeof(XiSlotMap));
    if (!map)
        return NULL;

    map->entries = (XiSlotMapEntry *) xr_malloc(count * sizeof(XiSlotMapEntry));
    if (!map->entries) {
        xr_free(map);
        return NULL;
    }
    map->capacity = count;

    /* Fill entries from all blocks' values */
    uint32_t idx = 0;
    for (uint32_t b = 0; b < f->nblocks && idx < count; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues && idx < count; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->id >= ctx->reg_map_size)
                continue;
            uint8_t reg = ctx->reg_map[v->id];
            if (reg == NO_REG)
                continue;

            map->entries[idx].value_id = v->id;
            map->entries[idx].bc_slot = reg;
            /* Derive XR_TAG from Xi IR type */
            uint8_t tag = 5; /* XR_TAG_PTR default */
            if (v->type) {
                switch (v->type->kind) {
                    case XR_KIND_INT:
                        tag = 3;
                        break; /* XR_TAG_I64 */
                    case XR_KIND_FLOAT:
                        tag = 4;
                        break; /* XR_TAG_F64 */
                    case XR_KIND_BOOL:
                        tag = 1;
                        break; /* XR_TAG_BOOL */
                    case XR_KIND_NULL:
                    case XR_KIND_VOID:
                        tag = 0;
                        break; /* XR_TAG_NULL */
                    default:
                        tag = 5;
                        break; /* XR_TAG_PTR */
                }
            }
            map->entries[idx].xr_tag = tag;
            /* bc_pc: prefer per-value IC instruction offset when available
             * (recorded for GETPROP/SETPROP/INVOKE), fallback to block start */
            if (ctx->value_pc && v->id < ctx->reg_map_size && ctx->value_pc[v->id] >= 0) {
                map->entries[idx].bc_pc = (uint32_t) ctx->value_pc[v->id];
            } else {
                map->entries[idx].bc_pc =
                    (blk->id < ctx->block_pc_size && ctx->block_pc[blk->id] >= 0)
                        ? (uint32_t) ctx->block_pc[blk->id]
                        : 0;
            }
            idx++;
        }
    }
    map->count = idx;
    return map;
}
