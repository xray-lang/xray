/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instruction_words_internal.h - Canonical physical instruction value core
 *
 * KEY CONCEPT:
 *   Data projections preserve all ten words and grant no semantic admission.
 */
#ifndef XXIR_INSTRUCTION_WORDS_INTERNAL_H
#define XXIR_INSTRUCTION_WORDS_INTERNAL_H
#include "xxir.h"
#include <string.h>
static inline uint32_t xir_instruction_word(const XrXirInstruction *op,uint32_t word) {
    switch (word) {
    case 0:return (uint32_t)op->op;
    case 1:return op->type;
    case 2:return op->args[0];case 3:return op->args[1];
    case 4:return op->targets[0];case 5:return op->targets[1];
    case 6:return (uint32_t)(uint64_t)op->immediate;
    case 7:return (uint32_t)((uint64_t)op->immediate>>32);
    case 8:return op->type_arguments[0];case 9:return op->type_arguments[1];
    default:return 0;
    }
}

/* The canonical run has already bounded every selected column. Physical
 * field assignments consume only their own mask-selected delta slots, including
 * unused fields and both unsigned halves of the signed immediate. */
static inline XrXirInstruction xir_instruction_words_value(const XrXirInstruction *base,const uint32_t *words,
    uint32_t mask,const uint32_t *delta,uint32_t offset) {
    XrXirInstruction value;
    value.op=(XrXirOp)((base?xir_instruction_word(base,0):words[0])+((mask&UINT32_C(1))?delta[0]*offset:0));
    value.type=(base?xir_instruction_word(base,1):words[1])+((mask&UINT32_C(2))?delta[1]*offset:0);
    value.args[0]=(base?xir_instruction_word(base,2):words[2])+((mask&UINT32_C(4))?delta[2]*offset:0);
    value.args[1]=(base?xir_instruction_word(base,3):words[3])+((mask&UINT32_C(8))?delta[3]*offset:0);
    value.targets[0]=(base?xir_instruction_word(base,4):words[4])+((mask&UINT32_C(16))?delta[4]*offset:0);
    value.targets[1]=(base?xir_instruction_word(base,5):words[5])+((mask&UINT32_C(32))?delta[5]*offset:0);
    uint64_t immediate=(base?xir_instruction_word(base,6):words[6])+((mask&UINT32_C(64))?delta[6]*offset:0);
    immediate|=(uint64_t)((base?xir_instruction_word(base,7):words[7])+((mask&UINT32_C(128))?delta[7]*offset:0))<<32;
    memcpy(&value.immediate,&immediate,sizeof(immediate));
    value.type_arguments[0]=(base?xir_instruction_word(base,8):words[8])+((mask&UINT32_C(256))?delta[8]*offset:0);
    value.type_arguments[1]=(base?xir_instruction_word(base,9):words[9])+((mask&UINT32_C(512))?delta[9]*offset:0);
    return value;
}

#endif // XXIR_INSTRUCTION_WORDS_INTERNAL_H
