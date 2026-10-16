/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_integer_sqrt.inc.c - Rounded integer square roots with scalar XIR
 *
 * KEY CONCEPT: Exact restoring division rounds a binary64 significand once.
 */
typedef struct SourceSqrtLoop {
    SourceValue root, remainder, index;
    uint32_t head, branch, operands[3];
} SourceSqrtLoop;

static bool source_sqrt_loop_open(SourceContext *ctx,SourceValue zero,SourceValue first,
    SourceValue bound,SourceSqrtLoop *loop) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    uint32_t origin=body->current_block.identity;
    loop->head=body->block_count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{loop->head},0,{0}},NULL) ||
        !begin_block(ctx)) return false;
    SourceValue *values[]={&loop->root,&loop->remainder,&loop->index};
    for (unsigned i=0;i<3;++i) {
        SourceValue seed=i==2 ? first : zero;
        SourceValue inputs[]={{origin,XR_XIR_UNIT},seed,{0,XR_XIR_UNIT},seed};
        loop->operands[i]=body->operand_count;
        if (!source_recipe_group(ctx,(XrXirInstruction){XR_XIR_PHI,seed.type,{0},{0},0,{0}},inputs,4,values[i])) return false;
    }
    SourceValue condition;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,
        {loop->index.id,bound.id},{0},0,{0}},&condition)) return false;
    loop->branch=body->count;
    uint32_t entry=body->block_count;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{condition.id},{entry,0},0,{0}},NULL) &&
        begin_block(ctx);
}

static bool source_sqrt_loop_close(SourceContext *ctx,SourceSqrtLoop *loop,
    SourceValue root,SourceValue remainder,SourceValue one) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    SourceValue next;
    if (!source_bits_binary(ctx,XR_XIR_ADD_INT,loop->index,one,&next)) return false;
    uint32_t origin=body->current_block.identity,exit=body->block_count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{loop->head},0,{0}},NULL) ||
        !begin_block(ctx) || !source_work_units(ctx,NULL,6)) return false;
    SourceValue values[]={root,remainder,next};
    for (unsigned i=0;i<3;++i) {
        uint32_t offset=loop->operands[i];
        if (offset>body->operand_count || body->operand_count-offset<4)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"square root loop operands are inconsistent");
        body->operands[offset+2]=origin;
        body->operands[offset+3]=values[i].id;
    }
    body->recipes[loop->branch].instruction.targets[1]=exit;
    return true;
}

static bool source_sqrt_bit(SourceContext *ctx,SourceValue bits,SourceValue index,
    SourceValue zero,SourceValue one,SourceValue *value) {
    SourceValue lower,upper,limit,mask,safe,negative,inside;
    /* Mask the count before evaluating the shift. Invalid indices still yield
     * zero, without relying on a branch to suppress a host shift by 64. */
    return source_bits_constant(ctx,XR_XIR_I64,0,&lower) &&
        source_bits_constant(ctx,XR_XIR_I64,64,&upper) &&
        source_bits_constant(ctx,XR_XIR_I64,63,&mask) &&
        source_bits_binary(ctx,XR_XIR_AND_INT,index,mask,&safe) &&
        source_bits_binary(ctx,XR_XIR_SHR_INT,bits,safe,&limit) &&
        source_bits_binary(ctx,XR_XIR_AND_INT,limit,one,&limit) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{index.id,lower.id},{0},0,{0}},&negative) &&
        source_integer_pick(ctx,negative,zero,limit,&limit) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{index.id,upper.id},{0},0,{0}},&inside) &&
        source_integer_pick(ctx,inside,limit,zero,value);
}

static bool source_sqrt_pair(SourceContext *ctx,SourceValue bits,SourceValue index,
    SourceValue scale,SourceValue zero,SourceValue one,SourceValue *pair) {
    SourceValue top,two,position,next,high,low;
    return source_bits_constant(ctx,XR_XIR_I64,52,&top) &&
        source_bits_constant(ctx,XR_XIR_I64,2,&two) &&
        source_bits_binary(ctx,XR_XIR_SUB_INT,top,index,&position) &&
        source_bits_binary(ctx,XR_XIR_MUL_INT,position,two,&position) &&
        source_bits_binary(ctx,XR_XIR_SUB_INT,position,scale,&position) &&
        source_bits_constant(ctx,XR_XIR_I64,1,&next) &&
        source_bits_binary(ctx,XR_XIR_ADD_INT,position,next,&next) &&
        source_sqrt_bit(ctx,bits,next,zero,one,&high) &&
        source_sqrt_bit(ctx,bits,position,zero,one,&low) &&
        source_bits_constant(ctx,XR_XIR_I64,1,&next) &&
        source_bits_binary(ctx,XR_XIR_SHL_INT,high,next,&high) &&
        source_bits_binary(ctx,XR_XIR_OR_INT,high,low,pair);
}

static bool source_sqrt_restore(SourceContext *ctx,SourceSqrtLoop *loop,SourceValue pair,
    SourceValue one,SourceValue *root,SourceValue *remainder) {
    SourceValue two,count,trial,shifted,difference,accepted,candidate;
    return source_bits_constant(ctx,XR_XIR_I64,2,&two) &&
        source_bits_binary(ctx,XR_XIR_SHL_INT,loop->remainder,two,&shifted) &&
        source_bits_binary(ctx,XR_XIR_OR_INT,shifted,pair,&shifted) &&
        source_bits_binary(ctx,XR_XIR_SHL_INT,loop->root,two,&trial) &&
        source_bits_binary(ctx,XR_XIR_OR_INT,trial,one,&trial) &&
        source_bits_binary(ctx,XR_XIR_SUB_INT,shifted,trial,&difference) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_GE_INT,XR_XIR_BOOL,{shifted.id,trial.id},{0},0,{0}},&accepted) &&
        source_integer_pick(ctx,accepted,difference,shifted,remainder) &&
        source_bits_constant(ctx,XR_XIR_I64,1,&count) &&
        source_bits_binary(ctx,XR_XIR_SHL_INT,loop->root,count,&candidate) &&
        source_bits_binary(ctx,XR_XIR_OR_INT,candidate,one,&trial) &&
        source_integer_pick(ctx,accepted,trial,candidate,root);
}

static bool source_sqrt_positive(SourceContext *ctx,SourceValue input,SourceValue *value) {
    SourceValue bits,leading,top,two,exponent,scale,zero,first,one,bound,pair,root,remainder;
    /* The public conversion rounds the input to binary64 first, including MAX
     * rounding to 2^63. Its unsigned integer representation remains exact. */
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_F64,{input.id},{0},0,{0}},&bits) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_U64,{bits.id},{0},0,{0}},&bits) ||
        !source_integer_bits(ctx,XR_NATIVE_OPERATION_INTEGER_LEADING_ZEROS,bits,(SourceValue){0},&leading) ||
        !source_bits_constant(ctx,XR_XIR_I64,63,&top) ||
        !source_bits_binary(ctx,XR_XIR_SUB_INT,top,leading,&exponent) ||
        !source_bits_constant(ctx,XR_XIR_I64,2,&two) ||
        !source_bits_binary(ctx,XR_XIR_DIV_INT,exponent,two,&exponent) ||
        !source_bits_binary(ctx,XR_XIR_MUL_INT,exponent,two,&scale) ||
        !source_bits_constant(ctx,XR_XIR_I64,104,&top) ||
        !source_bits_binary(ctx,XR_XIR_SUB_INT,top,scale,&scale) ||
        !source_bits_constant(ctx,XR_XIR_U64,0,&zero) ||
        !source_bits_constant(ctx,XR_XIR_U64,1,&one) ||
        !source_bits_constant(ctx,XR_XIR_I64,0,&first) ||
        !source_bits_constant(ctx,XR_XIR_I64,53,&bound)) return false;
    SourceSqrtLoop loop={0};
    if (!source_sqrt_loop_open(ctx,zero,first,bound,&loop) ||
        !source_sqrt_pair(ctx,bits,loop.index,scale,zero,one,&pair) ||
        !source_sqrt_restore(ctx,&loop,pair,one,&root,&remainder) ||
        !source_bits_constant(ctx,XR_XIR_I64,1,&first) ||
        !source_sqrt_loop_close(ctx,&loop,root,remainder,first)) return false;
    SourceValue round,rounded,denominator;
    /* If S=q*q+r, the midpoint square is q*q+q+1/4. Since S is
     * integral, r>q rounds upward and an exact midpoint cannot occur. */
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,
            {loop.root.id,loop.remainder.id},{0},0,{0}},&round) &&
        source_bits_binary(ctx,XR_XIR_ADD_INT,loop.root,one,&rounded) &&
        source_integer_pick(ctx,round,rounded,loop.root,&rounded) &&
        source_bits_constant(ctx,XR_XIR_I64,52,&top) &&
        source_bits_binary(ctx,XR_XIR_SUB_INT,top,exponent,&scale) &&
        source_bits_binary(ctx,XR_XIR_SHL_INT,one,scale,&denominator) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_F64,{rounded.id},{0},0,{0}},&rounded) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_F64,{denominator.id},{0},0,{0}},&denominator) &&
        source_bits_binary(ctx,XR_XIR_DIV_FLOAT,rounded,denominator,value);
}

static bool source_integer_sqrt(SourceContext *ctx,SourceValue receiver,SourceValue *value) {
    SourceValue zero,one,negative,empty,safe,root,nan;
    return source_bits_constant(ctx,XR_XIR_I64,0,&zero) &&
        source_bits_constant(ctx,XR_XIR_I64,1,&one) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{receiver.id,zero.id},{0},0,{0}},&negative) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_EQ_INT,XR_XIR_BOOL,{receiver.id,zero.id},{0},0,{0}},&empty) &&
        source_integer_pick(ctx,negative,one,receiver,&safe) &&
        source_integer_pick(ctx,empty,one,safe,&safe) && source_sqrt_positive(ctx,safe,&root) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_FLOAT,XR_XIR_F64,{0},{0},INT64_C(0x7ff8000000000000),{0}},&nan) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_FLOAT,XR_XIR_F64,{0},{0},0,{0}},&zero) &&
        source_integer_pick(ctx,negative,nan,root,&root) && source_integer_pick(ctx,empty,zero,root,value);
}
