/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_unit_storage.inc.c - Real empty Tuple values at Array storage edges
 *
 * KEY CONCEPT:
 *   Unit expressions retain their effects without inventing a payload identity.
 *   Array value operands use the existing canonical empty Tuple constructor.
 */
static bool source_recipe_unit_array_operands(SourceContext *ctx,XrXirInstruction *op) {
    bool create=op->op==XR_XIR_ARRAY_NEW || op->op==XR_XIR_ARRAY_REPEAT;
    bool table=op->op==XR_XIR_ARRAY_NEW || op->op==XR_XIR_ARRAY_SET;
    if (!create && op->op!=XR_XIR_ARRAY_SET && op->op!=XR_XIR_ARRAY_PUSH) return true;
    SourceFunction *body=&ctx->bodies[ctx->function];
    uint32_t begin=table?op->args[0]:0,count=table?op->args[1]:2;
    if (table && (begin>body->operand_count || count>body->operand_count-begin))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Array storage operand range is invalid");
    if (op->op==XR_XIR_ARRAY_SET && count!=3)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Array assignment needs three operands");
    XrXirType type=op->type;
    if (!create) {
        uint32_t receiver=table?body->operands[begin]:op->args[0];
        uint32_t parameters=ctx->functions[ctx->function].parameter_count;
        if (receiver>=parameters+body->count)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"Array storage receiver is missing");
        type=receiver<parameters?ctx->functions[ctx->function].parameters[receiver]:
            body->recipes[receiver-parameters].instruction.type;
    }
    if (!xr_xir_type_is_array(&ctx->types,type) || xr_xir_array_element(&ctx->types,type)!=XR_XIR_UNIT) return true;
    uint32_t first=op->op==XR_XIR_ARRAY_NEW?0:op->op==XR_XIR_ARRAY_SET?2:1;
    if (first>=count) return true;
    SourceValue unit;
    if (!source_recipe_append(ctx,(XrXirInstruction){XR_XIR_TUPLE_NEW,XR_XIR_UNIT,{0},{0},0,{0}},&unit)) return false;
    for (uint32_t i=first;i<count;++i) {
        if (!source_work(ctx,NULL)) return false;
        if (table) body->operands[begin+i]=unit.id;
        else op->args[i]=unit.id;
    }
    return true;
}
