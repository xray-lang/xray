/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instruction_words.h - Complete physical instruction field projection
 *
 * KEY CONCEPT:
 *   Exact correspondence includes unused operands and both immediate halves.
 *   Field equality alone never grants type, effect or execution permission.
 */
#ifndef XXIR_INSTRUCTION_WORDS_H
#define XXIR_INSTRUCTION_WORDS_H
#include "xxir.h"
enum { XIR_INPUT_WORDS=10 };
static inline uint32_t xir_input_instruction_word(const XrXirInstruction *op,uint32_t word) {
    switch(word) {
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
#endif // XXIR_INSTRUCTION_WORDS_H
