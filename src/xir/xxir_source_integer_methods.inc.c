/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_integer_methods.inc.c - Definition-owned integer value recipes
 *
 * KEY CONCEPT: Governed member identities select ordinary verified arithmetic.
 */
static uint32_t source_primitive_query_find(SourceContext *ctx,
    XrXirSourceDeclarationKind kind, uint32_t parent, uint32_t identity) {
    for (uint32_t i = 0; i < ctx->query.declaration_count; ++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return 0;
        const XrXirSourceDeclaration *row = &ctx->query.declarations[i];
        if (row->kind == kind && row->parent == parent && row->native_identity == identity)
            return row->id;
    }
    return 0;
}
static bool source_primitive_type(SourceContext *ctx,uint32_t identity,XrXirType *type) {
    switch (identity) {
    case XR_NATIVE_DECLARATION_INTEGER: *type=XR_XIR_I64;return true;
    case XR_NATIVE_DECLARATION_FLOAT: *type=XR_XIR_F64;return true;
    case XR_NATIVE_DECLARATION_BOOLEAN: *type=XR_XIR_BOOL;return true;
    case XR_NATIVE_DECLARATION_RUNE: *type=XR_XIR_RUNE;return true;
    default: return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"primitive declaration identity is not admitted");
    }
}
static bool source_primitive_result_type(SourceContext *ctx,XrNativeTypeTerm term,XrXirType *type) {
    switch (term) {
    case XR_NATIVE_TERM_I64: *type=XR_XIR_I64;return true;
    case XR_NATIVE_TERM_F64: *type=XR_XIR_F64;return true;
    case XR_NATIVE_TERM_STRING: *type=XR_XIR_STRING;return true;
    case XR_NATIVE_TERM_BOOL: *type=XR_XIR_BOOL;return true;
    case XR_NATIVE_TERM_U32: *type=XR_XIR_U32;return true;
    case XR_NATIVE_TERM_NULLABLE_I64: return source_nullable_type(ctx,XR_XIR_I64,type);
    default: return source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"primitive result has no admitted type contract");
    }
}
static uint32_t source_primitive_declaration(SourceContext *ctx,
    const XrNativeTypeDeclaration *native) {
    uint32_t declaration = source_primitive_query_find(ctx,XR_XIR_SOURCE_TYPE,0,native->id);
    if (declaration || ctx->diagnostic.status != XR_XIR_OK) return declaration;
    if (!source_native_admit(ctx,native)) return 0;
    XrXirSourceQueryModule *modules = source_query_append(ctx,ctx->query.modules,
        &ctx->query.module_count,&ctx->query_module_capacity,sizeof(*modules));
    if (!modules) return 0;
    ctx->query.modules = modules;
    uint32_t module = ctx->query.module_count-1;
    modules[module] = (XrXirSourceQueryModule){native->identity,native->source_path,native->source_fingerprint};
    SourceName symbol = {0}; symbol.name = source_owned_text(ctx,native->name);
    XrXirSourceRange range = {module,(int)native->line,(int)native->column,
        (int)native->line,(int)(native->column+source_text_size(ctx,native->name))};
    if (!symbol.name || !source_query_declare(ctx,&symbol,XR_XIR_SOURCE_TYPE,0,range)) return 0;
    XrXirSourceDeclaration *row = (XrXirSourceDeclaration *)&ctx->query.declarations[symbol.declaration-1];
    row->native_identity = native->id; row->exported = true;
    XrXirType type;
    if (!source_primitive_type(ctx,native->id,&type)) return 0;
    row->type = (XrXirSourceType){type,0,true};
    return symbol.declaration;
}
static bool source_primitive_member_reference(SourceContext *ctx, AstNode *node,
    const XrNativeTypeDeclaration *native,const XrNativeMemberDeclaration *member) {
    uint32_t parent = source_primitive_declaration(ctx,native);
    if (!parent) return false;
    uint32_t declaration = source_primitive_query_find(ctx,XR_XIR_SOURCE_MEMBER,parent,member->id);
    if (ctx->diagnostic.status != XR_XIR_OK) return false;
    if (!declaration) {
        SourceName symbol = {0}; symbol.name = source_owned_text(ctx,member->name);
        uint32_t module = ctx->query.declarations[parent-1].range.module;
        XrXirSourceRange range = {module,(int)member->line,(int)member->column,
            (int)member->line,(int)(member->column+source_text_size(ctx,member->name))};
        if (!symbol.name || !source_query_declare(ctx,&symbol,XR_XIR_SOURCE_MEMBER,parent,range)) return false;
        declaration = symbol.declaration;
        const char *signature = source_owned_text(ctx,member->signature);
        XrXirSourceType *parameters = member->parameter_count ?
            source_alloc(ctx,member->parameter_count,sizeof(*parameters)) : NULL;
        if (!signature || (member->parameter_count && !parameters)) return false;
        for (uint32_t p = 0; p < member->parameter_count; ++p)
            parameters[p] = (XrXirSourceType){XR_XIR_I64,0,true};
        XrXirSourceDeclaration *row = (XrXirSourceDeclaration *)&ctx->query.declarations[declaration-1];
        row->native_identity = member->id; row->signature = signature;
        XrXirType result;
        if (!source_primitive_result_type(ctx,member->result,&result)) return false;
        row->type = (XrXirSourceType){result,0,true}; row->exported = true;
        row->parameters = parameters; row->parameter_count = member->parameter_count;
    }
    return source_query_target_token_reference(ctx,node,declaration,XR_XIR_SOURCE_CALL);
}
static bool source_integer_abs(SourceContext *ctx,SourceValue receiver,SourceValue *value) {
    SourceValue count, sign, folded;
    /* Arithmetic right shift yields zero or all ones. The modular identity
     * preserves MIN rather than invoking a signed host negation overflow. */
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},63,{0}},&count) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SHR_INT,XR_XIR_I64,{receiver.id,count.id},{0},0,{0}},&sign) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_XOR_INT,XR_XIR_I64,{receiver.id,sign.id},{0},0,{0}},&folded) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SUB_INT,XR_XIR_I64,{folded.id,sign.id},{0},0,{0}},value);
}
static bool source_integer_extreme(SourceContext *ctx,SourceValue left,
    SourceValue right,bool maximum,SourceValue *value) {
    SourceValue shift,a,b,difference,sign,different,mix,mask,bits;
    /* Select by the left sign when signs differ, otherwise by the wrapped
     * difference sign. This handles both endpoint overflow cases. */
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},63,{0}},&shift) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SHR_INT,XR_XIR_I64,{left.id,shift.id},{0},0,{0}},&a) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SHR_INT,XR_XIR_I64,{right.id,shift.id},{0},0,{0}},&b) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SUB_INT,XR_XIR_I64,{left.id,right.id},{0},0,{0}},&difference) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SHR_INT,XR_XIR_I64,{difference.id,shift.id},{0},0,{0}},&sign) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_XOR_INT,XR_XIR_I64,{a.id,b.id},{0},0,{0}},&different) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_XOR_INT,XR_XIR_I64,{sign.id,a.id},{0},0,{0}},&mix) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_AND_INT,XR_XIR_I64,{mix.id,different.id},{0},0,{0}},&mix) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_XOR_INT,XR_XIR_I64,{sign.id,mix.id},{0},0,{0}},&mask) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_XOR_INT,XR_XIR_I64,{left.id,right.id},{0},0,{0}},&bits) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_AND_INT,XR_XIR_I64,{bits.id,mask.id},{0},0,{0}},&bits) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_XOR_INT,XR_XIR_I64,{bits.id,maximum ? left.id : right.id},{0},0,{0}},value);
}
static bool source_integer_pick(SourceContext *ctx,SourceValue condition,
    SourceValue yes,SourceValue no,SourceValue *value) {
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (yes.type != no.type || condition.type != XR_XIR_BOOL)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"integer recipe selection shape is invalid");
    uint32_t branch = body->count, yes_block = body->block_count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{condition.id},{0},0,{0}},NULL) ||
        !begin_block(ctx)) return false;
    uint32_t yes_end = body->current_block.identity, yes_jump = body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    uint32_t no_block = body->block_count;
    if (!begin_block(ctx)) return false;
    uint32_t no_end = body->current_block.identity, no_jump = body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    uint32_t join = body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[0] = yes_block;
    body->recipes[branch].instruction.targets[1] = no_block;
    body->recipes[yes_jump].instruction.targets[0] = join;
    body->recipes[no_jump].instruction.targets[0] = join;
    SourceValue inputs[] = {{yes_end,XR_XIR_UNIT},yes,{no_end,XR_XIR_UNIT},no};
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_PHI,yes.type,{0},{0},0,{0}},inputs,4,value);
}
static bool source_integer_overflow(SourceContext *ctx,XrXirOp operation,
    SourceValue left,SourceValue right,SourceValue result,SourceValue *overflow) {
    SourceValue zero,a,b,bits;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},&zero)) return false;
    if (operation != XR_XIR_MUL_INT) {
        SourceValue basis = operation == XR_XIR_ADD_INT ? result : left;
        return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_XOR_INT,XR_XIR_I64,
                {basis.id,operation == XR_XIR_ADD_INT ? left.id : right.id},{0},0,{0}},&a) &&
            source_recipe_record(ctx,(XrXirInstruction){XR_XIR_XOR_INT,XR_XIR_I64,
                {basis.id,operation == XR_XIR_ADD_INT ? right.id : result.id},{0},0,{0}},&b) &&
            source_recipe_record(ctx,(XrXirInstruction){XR_XIR_AND_INT,XR_XIR_I64,{a.id,b.id},{0},0,{0}},&bits) &&
            source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{bits.id,zero.id},{0},0,{0}},overflow);
    }
    SourceValue one,negative,minimum,is_zero,divisor,quotient,normal,is_negative,special,not_zero;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},1,{0}},&one) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},-1,{0}},&negative) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},INT64_MIN,{0}},&minimum) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_EQ_INT,XR_XIR_BOOL,{right.id,zero.id},{0},0,{0}},&is_zero) &&
        source_integer_pick(ctx,is_zero,one,right,&divisor) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_DIV_INT,XR_XIR_I64,{result.id,divisor.id},{0},0,{0}},&quotient) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NE_INT,XR_XIR_BOOL,{quotient.id,left.id},{0},0,{0}},&normal) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_EQ_INT,XR_XIR_BOOL,{right.id,negative.id},{0},0,{0}},&is_negative) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_EQ_INT,XR_XIR_BOOL,{left.id,minimum.id},{0},0,{0}},&special) &&
        source_integer_pick(ctx,is_negative,special,normal,&bits) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},0,{0}},&not_zero) &&
        source_integer_pick(ctx,is_zero,not_zero,bits,overflow);
}
static bool source_integer_checked_result(SourceContext *ctx,SourceValue overflow,
    SourceValue result,SourceValue *value) {
    XrXirType nullable;
    if (!source_nullable_type(ctx,XR_XIR_I64,&nullable)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t branch = body->count, yes_block = body->block_count;
    SourceValue yes,no;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BRANCH,XR_XIR_UNIT,{overflow.id},{0},0,{0}},NULL) ||
        !begin_block(ctx) || !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NULLABLE_NONE,nullable,{0},{0},0,{0}},&yes)) return false;
    uint32_t yes_end = body->current_block.identity, yes_jump = body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    uint32_t no_block = body->block_count;
    if (!begin_block(ctx) || !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NULLABLE_SOME,nullable,{result.id,0},{0},0,{0}},&no)) return false;
    uint32_t no_end = body->current_block.identity, no_jump = body->count;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    uint32_t join = body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[0] = yes_block; body->recipes[branch].instruction.targets[1] = no_block;
    body->recipes[yes_jump].instruction.targets[0] = join; body->recipes[no_jump].instruction.targets[0] = join;
    SourceValue inputs[] = {{yes_end,XR_XIR_UNIT},yes,{no_end,XR_XIR_UNIT},no};
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_PHI,nullable,{0},{0},0,{0}},inputs,4,value);
}
typedef enum SourceIntegerControl { SOURCE_INTEGER_ORDINARY, SOURCE_INTEGER_CHECKED,
    SOURCE_INTEGER_SATURATING, SOURCE_INTEGER_OVERFLOW } SourceIntegerControl;
static XrXirOp source_integer_control_operation(XrNativeOperation operation,SourceIntegerControl *mode) {
    switch (operation) {
    case XR_NATIVE_OPERATION_INTEGER_CHECKED_ADD: *mode=SOURCE_INTEGER_CHECKED; return XR_XIR_ADD_INT;
    case XR_NATIVE_OPERATION_INTEGER_CHECKED_SUB: *mode=SOURCE_INTEGER_CHECKED; return XR_XIR_SUB_INT;
    case XR_NATIVE_OPERATION_INTEGER_CHECKED_MUL: *mode=SOURCE_INTEGER_CHECKED; return XR_XIR_MUL_INT;
    case XR_NATIVE_OPERATION_INTEGER_SATURATING_ADD: *mode=SOURCE_INTEGER_SATURATING; return XR_XIR_ADD_INT;
    case XR_NATIVE_OPERATION_INTEGER_SATURATING_SUB: *mode=SOURCE_INTEGER_SATURATING; return XR_XIR_SUB_INT;
    case XR_NATIVE_OPERATION_INTEGER_SATURATING_MUL: *mode=SOURCE_INTEGER_SATURATING; return XR_XIR_MUL_INT;
    case XR_NATIVE_OPERATION_INTEGER_ADD_OVERFLOWS: *mode=SOURCE_INTEGER_OVERFLOW; return XR_XIR_ADD_INT;
    case XR_NATIVE_OPERATION_INTEGER_SUB_OVERFLOWS: *mode=SOURCE_INTEGER_OVERFLOW; return XR_XIR_SUB_INT;
    case XR_NATIVE_OPERATION_INTEGER_MUL_OVERFLOWS: *mode=SOURCE_INTEGER_OVERFLOW; return XR_XIR_MUL_INT;
    default: return XR_XIR_INVALID;
    }
}
static bool source_integer_saturating_result(SourceContext *ctx,SourceValue sign,
    SourceValue overflow,SourceValue result,SourceValue *value) {
    SourceValue shift,maximum,limit;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},63,{0}},&shift) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SHR_INT,XR_XIR_I64,{sign.id,shift.id},{0},0,{0}},&sign) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},INT64_MAX,{0}},&maximum) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_XOR_INT,XR_XIR_I64,{sign.id,maximum.id},{0},0,{0}},&limit) &&
        source_integer_pick(ctx,overflow,limit,result,value);
}
static bool source_integer_text_literal(SourceContext *ctx,AstNode *node,
    const char *text,size_t length,SourceValue *value) {
    uint32_t literal;
    return source_literal_append(ctx,node,text,length,&literal) && source_recipe_record(ctx,
        (XrXirInstruction){XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},literal,{0}},value);
}
static bool source_integer_hex(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    SourceValue zero,negative,magnitude,positive_prefix,negative_prefix,output,started,truth,fifteen;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},&zero) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{receiver.id,zero.id},{0},0,{0}},&negative) ||
        !source_integer_abs(ctx,receiver,&magnitude) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_U64,{magnitude.id,0},{0},0,{0}},&magnitude) ||
        !source_integer_text_literal(ctx,node,"0x",2,&positive_prefix) ||
        !source_integer_text_literal(ctx,node,"-0x",3,&negative_prefix) ||
        !source_integer_pick(ctx,negative,negative_prefix,positive_prefix,&output) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},0,{0}},&started) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},1,{0}},&truth) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_U64,{0},{0},15,{0}},&fifteen)) return false;
    for (int digit=15; digit>=0; --digit) {
        SourceValue shift,nibble,nonzero,ten,decimal,offset,letter,ascii,rune,text,candidate;
        if (!((ctx)->diagnostic.status == XR_XIR_OK) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},digit*4,{0}},&shift) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_SHR_INT,XR_XIR_U64,{magnitude.id,shift.id},{0},0,{0}},&nibble) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_AND_INT,XR_XIR_U64,{nibble.id,fifteen.id},{0},0,{0}},&nibble) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_I64,{nibble.id,0},{0},0,{0}},&nibble) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_NE_INT,XR_XIR_BOOL,{nibble.id,zero.id},{0},0,{0}},&nonzero) ||
            !source_integer_pick(ctx,started,truth,digit ? nonzero : truth,&started) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},10,{0}},&ten) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{nibble.id,ten.id},{0},0,{0}},&decimal) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},48,{0}},&offset) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},87,{0}},&letter) ||
            !source_integer_pick(ctx,decimal,offset,letter,&offset) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ADD_INT,XR_XIR_I64,{offset.id,nibble.id},{0},0,{0}},&ascii) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_INTEGER_TO_RUNE,XR_XIR_RUNE,{ascii.id,0},{0},0,{0}},&rune) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TO_STRING,XR_XIR_STRING,{rune.id,0},{0},0,{0}},&text) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONCAT_STRING,XR_XIR_STRING,{output.id,text.id},{0},0,{0}},&candidate) ||
            !source_integer_pick(ctx,started,candidate,output,&output)) return false;
    }
    *value=output; return true;
}
#include "xxir_source_integer_bits.inc.c"
#include "xxir_source_integer_sqrt.inc.c"
static bool source_integer_call(SourceContext *ctx,AstNode *node,
    SourceValue receiver,SourceValue *value) {
    CallExprNode *call = &node->as.call_expr;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_INTEGER);
    if (!source_native_admit(ctx,native)) return false;
    const XrNativeMemberDeclaration *member = source_native_member(ctx,native,call->callee->as.member_access.name);
    if (!member || !member->is_public || member->is_static || !member->is_method ||
        member->receiver != XR_NATIVE_RECEIVER_READ)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"integer member has no admitted value-operation contract");
    bool absolute = member->operation == XR_NATIVE_OPERATION_INTEGER_ABS;
    bool text = member->operation == XR_NATIVE_OPERATION_INTEGER_TO_STRING;
    bool hex = member->operation == XR_NATIVE_OPERATION_INTEGER_TO_HEX;
    bool floating = member->operation == XR_NATIVE_OPERATION_INTEGER_TO_F64;
    bool square_root = member->operation == XR_NATIVE_OPERATION_INTEGER_SQRT;
    bool extreme = member->operation == XR_NATIVE_OPERATION_INTEGER_MIN || member->operation == XR_NATIVE_OPERATION_INTEGER_MAX;
    bool bits=source_integer_bit_operation(member->operation);
    bool rotation=member->operation==XR_NATIVE_OPERATION_INTEGER_ROTATE_LEFT ||
        member->operation==XR_NATIVE_OPERATION_INTEGER_ROTATE_RIGHT;
    XrXirOp operation = member->operation == XR_NATIVE_OPERATION_INTEGER_WRAPPING_ADD ? XR_XIR_ADD_INT :
        member->operation == XR_NATIVE_OPERATION_INTEGER_WRAPPING_SUB ? XR_XIR_SUB_INT :
        member->operation == XR_NATIVE_OPERATION_INTEGER_WRAPPING_MUL ? XR_XIR_MUL_INT : XR_XIR_INVALID;
    SourceIntegerControl control=SOURCE_INTEGER_ORDINARY;
    XrXirOp controlled=source_integer_control_operation(member->operation,&control);
    if (controlled!=XR_XIR_INVALID) operation=controlled;
    bool unary = absolute || text || hex || floating || square_root || (bits && !rotation);
    XrNativeTypeTerm result = text || hex ? XR_NATIVE_TERM_STRING : floating || square_root ? XR_NATIVE_TERM_F64 :
        control==SOURCE_INTEGER_CHECKED ? XR_NATIVE_TERM_NULLABLE_I64 :
        control==SOURCE_INTEGER_OVERFLOW ? XR_NATIVE_TERM_BOOL : XR_NATIVE_TERM_I64;
    if ((!unary && !extreme && !bits && operation == XR_XIR_INVALID) ||
        ((absolute || extreme || floating || square_root || hex || bits || control!=SOURCE_INTEGER_ORDINARY) && receiver.type != XR_XIR_I64) || member->result != result ||
        member->parameter_count != (unary ? 0u : 1u) ||
        (!unary && member->parameters[0].type != XR_NATIVE_TERM_I64) ||
        call->type_arg_count || call->default_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"integer operation requires its exact ordinary declaration");
    if (!source_primitive_member_reference(ctx,call->callee,native,member)) return false;
    XrXirType parameter=XR_XIR_I64;SourceValue other={0};uint32_t supplied;
    if (!source_native_read_arguments(ctx,node,&parameter,unary ? 0u : 1u,unary ? 0u : 1u,
        &other,&supplied,"integer operation requires its exact ordinary declaration")) return false;
    if (bits && unary) return source_integer_bits(ctx,member->operation,receiver,(SourceValue){0},value);
    if (absolute) return source_integer_abs(ctx,receiver,value);
    if (hex) return source_integer_hex(ctx,node,receiver,value);
    if (text) return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_TO_STRING,XR_XIR_STRING,{receiver.id,0},{0},0,{0}},value);
    if (floating) return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONVERT_NUMBER,XR_XIR_F64,{receiver.id,0},{0},0,{0}},value);
    if (square_root) return source_integer_sqrt(ctx,receiver,value);
    if (other.type != XR_XIR_I64)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"integer operation argument does not satisfy i64");
    if (rotation) return source_integer_bits(ctx,member->operation,receiver,other,value);
    if (extreme) return source_integer_extreme(ctx,receiver,other,
        member->operation == XR_NATIVE_OPERATION_INTEGER_MAX,value);
    /* Wrapping operates in the receiver's ring. Reducing the declared i64
     * operand first is equivalent to reducing the complete arithmetic result. */
    if (receiver.type != XR_XIR_I64 && !source_recipe_record(ctx,
        (XrXirInstruction){XR_XIR_CONVERT_NUMBER,receiver.type,{other.id,0},{0},0,{0}},&other)) return false;
    SourceValue computed,overflow;
    if (!source_recipe_record(ctx,(XrXirInstruction){operation,receiver.type,
        {receiver.id,other.id},{0},0,{0}},&computed)) return false;
    if (control==SOURCE_INTEGER_ORDINARY) { *value=computed; return true; }
    if (!source_integer_overflow(ctx,operation,receiver,other,computed,&overflow)) return false;
    if (control==SOURCE_INTEGER_OVERFLOW) { *value=overflow; return true; }
    if (control==SOURCE_INTEGER_CHECKED) return source_integer_checked_result(ctx,overflow,computed,value);
    SourceValue sign=receiver;
    if (operation==XR_XIR_MUL_INT && !source_recipe_record(ctx,
        (XrXirInstruction){XR_XIR_XOR_INT,XR_XIR_I64,{receiver.id,other.id},{0},0,{0}},&sign)) return false;
    return source_integer_saturating_result(ctx,sign,overflow,computed,value);
}
static bool source_integer_ground_result(SourceContext *ctx,AstNode *node,
    SourceExpectedType receiver,SourceExpectedType *output) {
    if (!receiver.present) {
        SourceInteger integer;
        AstNode *object=node->as.call_expr.callee->as.member_access.object;
        if (!source_direct_integer(ctx,object,&integer)) return false;
        if (!integer.present) return true;
        SourceNumericRecipe recipe;
        SourceNumericRequest request={&integer,NULL,{false,XR_XIR_UNIT,false,false},true};
        if (!source_numeric_plan(ctx,object,&request,&recipe)) return false;
        receiver=(SourceExpectedType){true,recipe.type,false,false};
    }
    if (!xr_xir_type_is_integer(receiver.type)) return true;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_INTEGER);
    if (!source_native_admit(ctx,native)) return false;
    const XrNativeMemberDeclaration *member=source_native_member(ctx,native,
        node->as.call_expr.callee->as.member_access.name);
    if (!member || member->operation==XR_NATIVE_OPERATION_NONE || member->is_static || !member->is_method) return true;
    XrXirType type=XR_XIR_UNIT;
    switch (member->result) {
    case XR_NATIVE_TERM_I64:
        type=member->operation==XR_NATIVE_OPERATION_INTEGER_WRAPPING_ADD ||
            member->operation==XR_NATIVE_OPERATION_INTEGER_WRAPPING_SUB ||
            member->operation==XR_NATIVE_OPERATION_INTEGER_WRAPPING_MUL ? receiver.type : XR_XIR_I64;
        break;
    case XR_NATIVE_TERM_BOOL: type=XR_XIR_BOOL;break;
    case XR_NATIVE_TERM_STRING: type=XR_XIR_STRING;break;
    case XR_NATIVE_TERM_F64: type=XR_XIR_F64;break;
    case XR_NATIVE_TERM_NULLABLE_I64:
        if (!source_nullable_type(ctx,XR_XIR_I64,&type)) return false;
        break;
    default: return true;
    }
    *output=(SourceExpectedType){true,type,false,false};return true;
}
