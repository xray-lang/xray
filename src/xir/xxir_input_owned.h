/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_input_owned.h - Independent physical function owner and ordinal reads
 *
 * KEY CONCEPT:
 *   A physical owner stores one initialized instruction per numeric ordinal.
 *   Every consumer admits current types, metadata and execution roles separately.
 */
#ifndef XXIR_INPUT_OWNED_H
#define XXIR_INPUT_OWNED_H
#include "xxir_instruction_words.h"
#include <limits.h>

typedef struct XirOwnedInputFunction XirOwnedInputFunction;
/* A synchronous physical borrow requires the caller's live strong owner.
 * It conveys no opcode, type, CFG or semantic admission result. */
typedef struct XirInputInstructionSpan {
    const XrXirInstruction *rows;
    uint32_t count;
} XirInputInstructionSpan;
typedef struct XirInputFunctionHead {
    const char *name;
    uint32_t name_length;
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result;
    const XrXirBlock *blocks;
    uint32_t block_count,instruction_count;
    const uint32_t *operands;
    uint32_t operand_count;
} XirInputFunctionHead;
/* This readable external span is borrowed only during complete receiving.
 * It is never retained, used as a digest or accepted as semantic authority. */
typedef struct XirInputFunctionReceive {
    XirInputFunctionHead head;
    const XrXirInstruction *instructions;
} XirInputFunctionReceive;
typedef struct XirInputWork {
    const void *owner;
    bool (*consume)(const void *,uint64_t);
} XirInputWork;
XR_FUNC XrXirStatus xir_input_function_instruction_span(const XirInputWork *,
    const XirOwnedInputFunction *,XirInputInstructionSpan *);
/* These bounds describe physical reads, never semantic or execution admission.
 * The query pays for fixed metadata and finite integer-width arithmetic. */
typedef struct XirInputReadCosts {
    uint64_t head,row,type;
    uint32_t blocks,instructions,operands;
} XirInputReadCosts;
#define XIR_INPUT_READ_COST_QUERY_UNITS \
    (sizeof(XirInputReadCosts)+3*sizeof(uint32_t)+sizeof(void *))
XR_FUNC XrXirStatus xir_input_function_read_costs(const XirInputWork *,
    const XirOwnedInputFunction *,XirInputReadCosts *);
/* The visitor receives actual initialized storage synchronously. Each row is
 * directly located and delivered immediately; the actual instruction count
 * closes the whole function. No shape result or data borrow survives the call. */
typedef struct XirInputFunctionVisitor {
    void *owner;
    XrXirStatus (*head)(void *,const XirInputFunctionHead *,uint32_t);
    XrXirStatus (*row)(void *,uint32_t,const XrXirInstruction *);
} XirInputFunctionVisitor;
XR_FUNC XrXirStatus xir_input_function_visit(const XirInputWork *,
    const XirOwnedInputFunction *,const XirInputFunctionVisitor *);
/* This current projection contains only the two requested physical words.
 * Every complete receiving still proves all ten fields independently. */
typedef struct XirInputInstructionShape { XrXirOp op;XrXirType type; } XirInputInstructionShape;
XR_FUNC XrXirStatus xir_input_instruction_shape(const XirInputWork *,
    const XirOwnedInputFunction *,uint32_t,XirInputInstructionShape *);
XR_FUNC XrXirStatus xir_input_function_value_type(const XirInputWork *,
    const XirOwnedInputFunction *,uint32_t,XrXirType *);
typedef struct XirInputPc { uint32_t function,instruction; } XirInputPc;

/* The physical factory owns every name, parameter, block, operand and record.
 * Complete semantic/current-source receiving and final proof are separate.
 * Failure leaves output empty and retains no generating context/stack pointer. */
XR_FUNC XrXirStatus xir_input_function_new(const XrXirCompileContext *,
    const XirInputFunctionReceive *,XirOwnedInputFunction **);
XR_FUNC XrXirStatus xir_input_function_clone(const XrXirCompileContext *,
    const XirOwnedInputFunction *,XirOwnedInputFunction **);
XR_FUNC XrXirStatus xir_input_function_raw_match(const XrXirCompileContext *,
    const XrXirFunction *,const XirOwnedInputFunction *);
XR_FUNC void xir_input_function_free(XirOwnedInputFunction *);
/* Every actual allocation retains this live ledger. Equality rejects a
 * foreign request and conveys no physical or semantic admission. */
XR_FUNC XrCompileResources *xir_input_function_resources(const XirOwnedInputFunction *);
/* Readable owner and work callback require caller exclusion and lifetime.
 * Every full Artifact entry calls shape on actual current storage. Neither
 * shape nor any projection grants types, layout, effects or execution rights. */
XR_FUNC XrXirStatus xir_input_function_shape(const XirInputWork *,const XirOwnedInputFunction *);
XR_FUNC XrXirStatus xir_input_function_head_shape(const XirInputWork *,const XirInputFunctionHead *);
XR_FUNC XrXirStatus xir_input_function_head(const XirInputWork *,
    const XirOwnedInputFunction *,XirInputFunctionHead *);
/* This pointer names true owned immutable metadata, not a stack snapshot. */
XR_FUNC XrXirStatus xir_input_function_head_ref(const XirInputWork *,
    const XirOwnedInputFunction *,const XirInputFunctionHead **);
/* This projection proves readable allocation extents only. Complete block,
 * opcode, type and execution admission remain separate verifier duties. */
XR_FUNC XrXirStatus xir_input_function_receive_read(const XirInputWork *,
    const XirOwnedInputFunction *,XirInputFunctionReceive *);
/* Random reads directly locate actual initialized instructions. The
 * complete current-shape entry and physical ordinal bounds are independent
 * responsibilities; no verified bit or owner-address permission is stored. */
XR_FUNC XrXirStatus xir_input_instruction(const XirInputWork *,const XirOwnedInputFunction *,
    uint32_t,XrXirInstruction *);
/* A row lends actual owned storage during this synchronous call only.
 * Numeric ordinals remain the keys; no pointer-derived ordinal exists. */
XR_FUNC XrXirStatus xir_input_instruction_ref(const XirInputWork *,const XirOwnedInputFunction *,
    uint32_t,XrXirInstruction *,const XrXirInstruction **);
#endif // XXIR_INPUT_OWNED_H
