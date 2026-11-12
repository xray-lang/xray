/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_aggregate_transfer.inc.c - Definite aggregate initialization on real CFG edges
 */
static bool rbc_aggregate_transfer(RbcStringAdmission *s, uint32_t index) {
    XrRbcAggregateAccess access = s->function->aggregate_accesses[index];
    for (uint32_t n = 0; n < access.count; ++n) {
        XrRbcArrayLocation operand = s->function->aggregate_operands[access.first + n];
        if ((operand.spill ? s->spills[operand.index].state : s->state[operand.index]) != RBC_BITS) return false;
    }
    if (access.operation == XR_RBC_AGGREGATE_NEW) {
        s->aggregates[access.destination] = 1;
        return true;
    }
    if (!s->aggregates[access.source]) return false;
    if (access.operation == XR_RBC_AGGREGATE_COPY) {
        s->aggregates[access.destination] = 1;
    } else if (access.result.spill) {
        s->spills[access.result.index] = (RbcSpillState){0, RBC_BITS};
    } else {
        if (!rbc_bits_destination(s, access.result.index)) return false;
        s->state[access.result.index] = RBC_BITS;
    }
    return true;
}
