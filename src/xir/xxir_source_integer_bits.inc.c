/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_integer_bits.inc.c - Ordinary exact i64 bit recipes
 *
 * KEY CONCEPT: Unsigned intermediate values preserve all two's-complement bits.
 */
static bool source_integer_bit_operation(XrNativeOperation operation) {
    switch (operation) {
    case XR_NATIVE_OPERATION_INTEGER_POPCOUNT: case XR_NATIVE_OPERATION_INTEGER_LEADING_ZEROS:
    case XR_NATIVE_OPERATION_INTEGER_TRAILING_ZEROS: case XR_NATIVE_OPERATION_INTEGER_BYTESWAP:
    case XR_NATIVE_OPERATION_INTEGER_ROTATE_LEFT: case XR_NATIVE_OPERATION_INTEGER_ROTATE_RIGHT:
        return true;
    default: return false;
    }
}
static bool source_bits_constant(SourceContext *ctx,XrXirType type,int64_t bits,SourceValue *value) {
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,type,{0},{0},bits,{0}},value);
}
static bool source_bits_binary(SourceContext *ctx,XrXirOp operation,
    SourceValue left,SourceValue right,SourceValue *value) {
    return source_recipe_record(ctx,(XrXirInstruction){operation,left.type,{left.id,right.id},{0},0,{0}},value);
}
static bool source_bits_shift(SourceContext *ctx,SourceValue bits,int64_t count,SourceValue *value) {
    SourceValue amount;
    return source_bits_constant(ctx,XR_XIR_I64,count,&amount) &&
        source_bits_binary(ctx,XR_XIR_SHR_INT,bits,amount,value);
}
static bool source_bits_popcount(SourceContext *ctx,SourceValue bits,SourceValue *value) {
    SourceValue shifted,mask,left,right;
    /* Each lane counts its original bits before the wider lanes are added.
     * Unsigned multiplication extracts the sum without signed overflow. */
    return source_bits_shift(ctx,bits,1,&shifted) &&
        source_bits_constant(ctx,XR_XIR_U64,INT64_C(0x5555555555555555),&mask) &&
        source_bits_binary(ctx,XR_XIR_AND_INT,shifted,mask,&right) &&
        source_bits_binary(ctx,XR_XIR_SUB_INT,bits,right,&bits) &&
        source_bits_constant(ctx,XR_XIR_U64,INT64_C(0x3333333333333333),&mask) &&
        source_bits_binary(ctx,XR_XIR_AND_INT,bits,mask,&left) &&
        source_bits_shift(ctx,bits,2,&shifted) &&
        source_bits_binary(ctx,XR_XIR_AND_INT,shifted,mask,&right) &&
        source_bits_binary(ctx,XR_XIR_ADD_INT,left,right,&bits) &&
        source_bits_shift(ctx,bits,4,&shifted) &&
        source_bits_binary(ctx,XR_XIR_ADD_INT,bits,shifted,&bits) &&
        source_bits_constant(ctx,XR_XIR_U64,INT64_C(0x0f0f0f0f0f0f0f0f),&mask) &&
        source_bits_binary(ctx,XR_XIR_AND_INT,bits,mask,&bits) &&
        source_bits_constant(ctx,XR_XIR_U64,INT64_C(0x0101010101010101),&mask) &&
        source_bits_binary(ctx,XR_XIR_MUL_INT,bits,mask,&bits) &&
        source_bits_shift(ctx,bits,56,&bits) && source_recipe_record(ctx,
            (XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_I64,{bits.id,0},{0},0,{0}},value);
}
static bool source_bits_byteswap(SourceContext *ctx,SourceValue bits,SourceValue *value) {
    const int64_t masks[]={INT64_C(0x00ff00ff00ff00ff),INT64_C(0x0000ffff0000ffff),INT64_C(0xffffffff)};
    for (unsigned stage=0;stage<3;++stage) {
        SourceValue mask,amount,left,right;
        if (!source_work(ctx,NULL) || !source_bits_constant(ctx,XR_XIR_U64,masks[stage],&mask) ||
            !source_bits_constant(ctx,XR_XIR_I64,(int64_t)(8u<<stage),&amount) ||
            !source_bits_binary(ctx,XR_XIR_SHR_INT,bits,amount,&right) ||
            !source_bits_binary(ctx,XR_XIR_AND_INT,right,mask,&right) ||
            !source_bits_binary(ctx,XR_XIR_AND_INT,bits,mask,&left) ||
            !source_bits_binary(ctx,XR_XIR_SHL_INT,left,amount,&left) ||
            !source_bits_binary(ctx,XR_XIR_OR_INT,left,right,&bits)) return false;
    }
    *value=bits;return true;
}
static bool source_integer_bits(SourceContext *ctx,XrNativeOperation operation,
    SourceValue receiver,SourceValue amount,SourceValue *value) {
    SourceValue bits;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_U64,
        {receiver.id,0},{0},0,{0}},&bits)) return false;
    if (operation==XR_NATIVE_OPERATION_INTEGER_LEADING_ZEROS) {
        for (int64_t count=1;count<=32;count*=2) {
            SourceValue shifted;
            if (!source_work(ctx,NULL) || !source_bits_shift(ctx,bits,count,&shifted) ||
                !source_bits_binary(ctx,XR_XIR_OR_INT,bits,shifted,&bits)) return false;
        }
        SourceValue total,popcount;
        return source_bits_popcount(ctx,bits,&popcount) && source_bits_constant(ctx,XR_XIR_I64,64,&total) &&
            source_bits_binary(ctx,XR_XIR_SUB_INT,total,popcount,value);
    }
    if (operation==XR_NATIVE_OPERATION_INTEGER_TRAILING_ZEROS) {
        SourceValue zero,one,negative;
        /* For zero, the isolated low bit is zero and its predecessor is all
         * ones, so the same identity returns 64 without a special branch. */
        if (!source_bits_constant(ctx,XR_XIR_U64,0,&zero) || !source_bits_constant(ctx,XR_XIR_U64,1,&one) ||
            !source_bits_binary(ctx,XR_XIR_SUB_INT,zero,bits,&negative) ||
            !source_bits_binary(ctx,XR_XIR_AND_INT,bits,negative,&bits) ||
            !source_bits_binary(ctx,XR_XIR_SUB_INT,bits,one,&bits)) return false;
        return source_bits_popcount(ctx,bits,value);
    }
    if (operation==XR_NATIVE_OPERATION_INTEGER_POPCOUNT) return source_bits_popcount(ctx,bits,value);
    if (operation==XR_NATIVE_OPERATION_INTEGER_BYTESWAP) {
        if (!source_bits_byteswap(ctx,bits,&bits)) return false;
    } else {
        bool left=operation==XR_NATIVE_OPERATION_INTEGER_ROTATE_LEFT;
        if (!left && operation!=XR_NATIVE_OPERATION_INTEGER_ROTATE_RIGHT)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"integer bit recipe is not admitted");
        SourceValue zero,mask,opposite,a,b;
        /* Mask both counts, including the opposite of zero. Existing shift
         * semantics then avoid a host shift by 64 for every signed count. */
        if (!source_bits_constant(ctx,XR_XIR_I64,0,&zero) || !source_bits_constant(ctx,XR_XIR_I64,63,&mask) ||
            !source_bits_binary(ctx,XR_XIR_AND_INT,amount,mask,&amount) ||
            !source_bits_binary(ctx,XR_XIR_SUB_INT,zero,amount,&opposite) ||
            !source_bits_binary(ctx,XR_XIR_AND_INT,opposite,mask,&opposite) ||
            !source_bits_binary(ctx,left ? XR_XIR_SHL_INT : XR_XIR_SHR_INT,bits,amount,&a) ||
            !source_bits_binary(ctx,left ? XR_XIR_SHR_INT : XR_XIR_SHL_INT,bits,opposite,&b) ||
            !source_bits_binary(ctx,XR_XIR_OR_INT,a,b,&bits)) return false;
    }
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_I64,
        {bits.id,0},{0},0,{0}},value);
}
