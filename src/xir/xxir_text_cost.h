/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_text_cost.h - Bounded deterministic decimal conversion reserves
 *
 * KEY CONCEPT:
 *   IEEE exponent bounds prepay the same finite bigint work before formatting.
 */
#ifndef XXIR_TEXT_COST_H
#define XXIR_TEXT_COST_H
#include "xxir_value.h"
#include "xxir_float.h"
typedef struct XirTextCost { uint64_t work,frame; } XirTextCost;
static inline XirTextCost xir_text_cost(const XrXirValue *value) {
    XirTextCost cost={256,512};
    uint32_t bits=xr_xir_float_bits((XrXirType)value->type);
    if (!bits) return cost;
    unsigned fraction=bits==32?23:52;uint64_t raw=(uint64_t)value->payload;
    uint64_t maximum=bits==32?255:2047,exponent=(raw>>fraction)&maximum;
    uint64_t magnitude=raw&((UINT64_C(1)<<(bits-1))-1);
    cost.work=1024;
    if (magnitude && exponent!=maximum) {
        int bias=bits==32?127:1023;
        int power=exponent?(int)exponent-bias:1-bias-(int)fraction;
        uint64_t distance=(uint64_t)(power<0?-power:power);
        /* At most 17 digits and 9 subtractions per digit fit this reserve.
         * Normalization pays full bigint copies and scans for each exponent. */
        cost.work=UINT64_C(131072)+(distance+2)*UINT64_C(2048);cost.frame=16384;
    }
    return cost;
}
#endif // XXIR_TEXT_COST_H
