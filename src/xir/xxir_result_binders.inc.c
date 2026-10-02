/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_result_binders.inc.c - Restricted callback result roles
 *
 * KEY CONCEPT:
 *   Result polymorphism admits an empty return without granting storage or
 *   resource authority. The full intrinsic body is checked before use.
 */
static bool result_symbol(const XrXirGeneric *g, XrXirType type) {
    uint32_t id = (uint32_t)type;
    return id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT &&
        xr_xir_binder_kind(g,id-XR_XIR_TYPE_PARAMETER_BASE) == XR_XIR_BINDER_RESULT_VARIABLE;
}
static bool result_instruction(const XrXirInstruction *a, XrXirInstruction b) {
    return a->op == b.op && a->type == b.type && a->args[0] == b.args[0] &&
        a->args[1] == b.args[1] && a->targets[0] == b.targets[0] && a->targets[1] == b.targets[1] &&
        a->immediate == b.immediate && a->type_arguments[0] == b.type_arguments[0] &&
        a->type_arguments[1] == b.type_arguments[1];
}
static bool result_panic_recipe(const XrXirModule *m, uint32_t f) {
    const XrXirFunction *function = &m->functions[f];
    if (function->parameter_count != 2 || !function->parameters || function->result != XR_XIR_UNIT ||
        function->parameters[1] != XR_XIR_STRING || function->operand_count || function->operands ||
        function->block_count != 5 || !function->blocks || function->instruction_count != 10 || !function->instructions)
        return false;
    const XrXirTypeNode *action = xr_xir_callable_signature(m->types,function->parameters[0]);
    if (!action || action->parameter_count || action->parameters || action->flags ||
        action->parameter_span != 1 || action->result != XR_XIR_TYPE_PARAMETER_BASE) return false;
    const XrXirBlock blocks[] = {{0,1,0,0},{1,1,4,0},{2,4,0,0},{6,2,0,0},{8,2,0,0}};
    for (uint32_t b = 0; b < 5; ++b) {
        const XrXirBlock *a = &function->blocks[b];
        if (a->first != blocks[b].first || a->count != blocks[b].count ||
            a->panic != blocks[b].panic || a->frontier != blocks[b].frontier) return false;
    }
    const XrXirInstruction instructions[] = {
        {XR_XIR_JUMP,XR_XIR_UNIT,{0},{1,0},0,{0}},
        {XR_XIR_INVOKE_INDIRECT,XR_XIR_TYPE_PARAMETER_BASE,{0},{2,3},0,{0}},
        {XR_XIR_INVOKE_DISCARD,XR_XIR_UNIT,{0},{0},1,{0}},
        {XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},0,{0}},
        {XR_XIR_ASSERT_CONDITION,XR_XIR_UNIT,{5,1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},1,{0}},
        {XR_XIR_THROW,XR_XIR_UNIT,{8,0},{0},0,{0}},
        {XR_XIR_PANIC_CATCH,XR_XIR_UNIT,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}
    };
    for (uint32_t i = 0; i < 10; ++i)
        if (!result_instruction(&function->instructions[i],instructions[i])) return false;
    return true;
}
static bool result_default_recipe(const XrXirModule *m, uint32_t f) {
    const XrXirFunction *function = &m->functions[f];
    if (!m->defaults || !m->defaults->records || !m->declarations ||
        function->parameter_count || function->parameters || function->result != XR_XIR_STRING ||
        function->operand_count || function->operands || function->block_count != 1 || !function->blocks ||
        function->blocks[0].first || function->blocks[0].count != 2 || function->blocks[0].panic ||
        function->blocks[0].frontier || function->instruction_count != 2 || !function->instructions) return false;
    const XrXirInstruction *literal = &function->instructions[0];
    if (literal->immediate < 0 || (uint64_t)literal->immediate >= m->declarations->literal_count ||
        !m->declarations->literals || m->declarations->literals[literal->immediate].length ||
        !result_instruction(literal,(XrXirInstruction){XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},literal->immediate,{0}}) ||
        !result_instruction(&function->instructions[1],(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}})) return false;
    for (uint32_t d = 0; d < m->defaults->count; ++d) {
        const XrXirDefaultBinding *binding = &m->defaults->records[d];
        if (binding->function == f) return binding->owner_kind == XR_XIR_DEFAULT_PARAMETER && binding->ordinal == 1 &&
            binding->owner < m->function_count && result_panic_recipe(m,binding->owner) &&
            xr_xir_binder_kind(&m->generics[binding->owner],0) == XR_XIR_BINDER_RESULT_VARIABLE;
    }
    return false;
}
XR_FUNCDEF XrXirStatus xr_xir_compile_result_binders_verify(const XrXirCompileContext *compile_context, const XrXirModule *m) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *b = &compile_state;
    if (!m || !b) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f = 0; m->generics && f < m->function_count; ++f) {
        const XrXirGeneric *g = &m->generics[f];
        if (!g->parameter_kinds) continue;
        uint64_t work = 20 + (m->defaults ? (uint64_t)m->defaults->count * 21 : 0);
        if (!xir_compile_work(b, work)) return XR_XIR_BUDGET;

        if (g->parameter_count != 1 || !g->constraints || g->argument_count || g->arguments ||
            g->parameter_kinds[0] != XR_XIR_BINDER_RESULT_VARIABLE || g->constraints[0].markers ||
            g->constraints[0].interface_count || g->constraints[0].interfaces ||
            (!result_panic_recipe(m,f) && !result_default_recipe(m,f))) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}
XR_FUNCDEF XrXirStatus xr_xir_compile_result_argument(const XrXirCompileContext *compile_context, const XrXirModule *m, uint32_t caller, XrXirType type) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *b = &compile_state;
    if (!m || !b || caller >= m->function_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(b, 1)) return XR_XIR_BUDGET;

    if (type == XR_XIR_UNIT) return XR_XIR_OK;
    if (m->generics && result_symbol(&m->generics[caller],type)) return XR_XIR_BAD_TYPE;
    XrXirProofContext context = {m,{XR_XIR_CONTEXT_FUNCTION,caller,0}};
    XrXirStatus status = xr_xir_compile_type_storage_prove(b, &context, type);
    return status == XR_XIR_OK ? xr_xir_compile_type_access(b, m, caller, type) : status;
}
/* Every Unit table entry must be consumed only by a result-role substitution.
 * Ordinary generic, nominal and requirement consumers keep their storage rule. */
XR_FUNCDEF XrXirStatus xr_xir_compile_result_unit_use(const XrXirCompileContext *compile_context, const XrXirModule *m, uint32_t f, uint32_t argument) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *b = &compile_state;
    if (!m || !b || f >= m->function_count || !m->generics || !m->functions ||
        argument >= m->generics[f].argument_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function = &m->functions[f];
    if (function->instruction_count && !function->instructions) return XR_XIR_BAD_STRUCTURE;
    bool used = false;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        if (!xir_compile_work(b, 1)) return XR_XIR_BUDGET;

        const XrXirInstruction *op = &function->instructions[i];
        uint32_t first = op->type_arguments[0], count = op->type_arguments[1];
        if (argument < first || argument-first >= count) continue;
        uint32_t target = UINT32_MAX;
        if (op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE || op->op == XR_XIR_FUNCTION_REF) {
            if (op->immediate >= 0 && (uint64_t)op->immediate < m->function_count) target = (uint32_t)op->immediate;
        } else if (op->op == XR_XIR_CALL_DEFAULT || op->op == XR_XIR_INVOKE_DEFAULT)
            target = op->op == XR_XIR_CALL_DEFAULT ? op->targets[0] : op->args[0];
        if (target >= m->function_count || argument-first >= m->generics[target].parameter_count ||
            xr_xir_binder_kind(&m->generics[target],argument-first) != XR_XIR_BINDER_RESULT_VARIABLE)
            return XR_XIR_BAD_TYPE;
        used = true;
    }
    return used ? XR_XIR_OK : XR_XIR_BAD_TYPE;
}
